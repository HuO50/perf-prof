#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/string.h>
#include <linux/rblist.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <termios.h>
#include <monitor.h>
#include <count_helpers.h>


struct hrcount_ctx {
    struct perf_evsel *leader;
    struct tp_list *tp_list;
    int nr_ins;
    int ins_oncpu;
    u64 *counters;
    u64 *perins_pos;
    struct count_dist *count_dist;
    int hist_size;
    u64 period;
    bool packed_display;

    u64 rounds;
    int slots;
    int round_nr;

    bool need_reset;

    bool pipe_char; // "|"
    int tp_sys_name_max_len;
    int all_counters_max_len;
    int *pertp_max_len;
};

static void hrcount_sigwinch(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    struct tp *tp;
    int i, len = 0;

    if (!dev->tty.istty)
        return ;

    ctx->packed_display = ctx->hist_size <= 5;
    if (!ctx->packed_display)
        return;
    /*
     * [INS] |tp  |tp  |
     * [002] |1 2 |2 3 |
     */
    if (dev->env->perins)
        len += ctx->ins_oncpu ? 6 /*[%03d] */ : 9 /*[%06d] */;
    for_each_real_tp(ctx->tp_list, tp, i) {
        len += ctx->pertp_max_len[i] + 1 /*|*/;
    }
    len += 1 /*|*/;

    ctx->packed_display = len <= dev->tty.col;
}
static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct hrcount_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (!env->event) {
        free(ctx);
        return -1;
    }

    if (!env->interval)
        env->interval = 1000;
    if (!env->sample_period)
        env->sample_period = env->interval * 1000000UL;

    // For stat, if you specify the --period parameter, it actually
    // refers to the reading interval.
    if (strcmp(dev->prof->name, "stat") == 0 &&
        env->interval * 1000000UL > env->sample_period) {
        int interval = env->interval;
        env->interval = env->sample_period / 1000000UL;
        env->sample_period = interval * 1000000UL;
    }

    tep__ref();

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->ins_oncpu = prof_dev_ins_oncpu(dev);
    ctx->nr_ins = prof_dev_nr_ins(dev);
    ctx->counters = calloc(ctx->nr_ins, (ctx->tp_list->nr_tp + 1) * sizeof(u64));
    if (!ctx->counters)
        goto failed;

    ctx->perins_pos = calloc(ctx->nr_ins, sizeof(u64));
    if (!ctx->perins_pos)
        goto failed;

    ctx->rounds = 0;
    ctx->slots = 2;
    ctx->round_nr = 0;

    if (env->interval * 1000000UL > env->sample_period)
        ctx->hist_size = env->interval * 1000000UL / env->sample_period;
    else
        ctx->hist_size = env->sample_period / (env->interval * 1000000UL);
    ctx->count_dist = count_dist_new(env->perins, true, false, ctx->hist_size * ctx->slots);
    if (!ctx->count_dist)
        goto failed;

    ctx->period = env->sample_period;
    ctx->pipe_char = ctx->hist_size > 1 || ctx->tp_list->nr_real_tp > 1;

    ctx->all_counters_max_len = 2;
    ctx->pertp_max_len = calloc(ctx->tp_list->nr_tp, sizeof(int));
    if (!ctx->pertp_max_len)
        goto failed;
    else {
        int i, len;
        struct tp *tp;
        for_each_real_tp(ctx->tp_list, tp, i) {
            ctx->pertp_max_len[i] = strlen(tp->alias ?: tp->name);
            len = ctx->hist_size * (ctx->all_counters_max_len+1/* ' ' */) - 1;
            if (ctx->pertp_max_len[i] < len)
                ctx->pertp_max_len[i] = len;

            if (tp->alias)
                len = strlen(tp->alias);
            else
                len = strlen(tp->sys) + 1 + strlen(tp->name); //sys:name
            if (len > ctx->tp_sys_name_max_len)
                ctx->tp_sys_name_max_len = len;
        }
    }
    hrcount_sigwinch(dev);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    count_dist_free(ctx->count_dist);
    if (ctx->counters)
        free(ctx->counters);
    if (ctx->perins_pos)
        free(ctx->perins_pos);
    if (ctx->pertp_max_len)
        free(ctx->pertp_max_len);
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int hrcount_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct hrcount_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = PERF_COUNT_SW_CPU_CLOCK,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0, //ns
        .freq          = 0,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ,
        .read_format   = PERF_FORMAT_ID | PERF_FORMAT_GROUP,
        .pinned        = 0,
        .disabled      = 1,
        .watermark     = 0,
    };
    struct perf_event_attr tp_attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 0,
        .disabled      = 0,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    // For hrcount, it can only be attached to cpu.
    // For stat, it can be attached to cpu and pid.
    if (strcmp(dev->prof->name, "hrcount") == 0 &&
        !prof_dev_ins_oncpu(dev)) {
        fprintf(stderr, "hrcount can only be attached to cpu.\n");
        return -1;
    }

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    attr.sample_period = ctx->period;
    attr.wakeup_events = ctx->hist_size; // Wake up every N events
    ctx->leader = evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    for_each_real_tp(ctx->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &tp_attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }

    perf_evlist__set_leader(evlist);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int hrcount_filter(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    struct tp *tp;
    int i, err;

    for_each_real_tp(ctx->tp_list, tp, i) {
        if (tp->filter && tp->filter[0]) {
            err = perf_evsel__apply_filter(tp->evsel, tp->filter);
            if (err < 0)
                return err;
        }
    }
    return 0;
}

static void hrcount_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

static void hrcount_sigusr(struct prof_dev *dev, int signum)
{
    struct hrcount_ctx *ctx = dev->private;
    if (signum == SIGWINCH) {
        if (ctx->packed_display) {
            hrcount_sigwinch(dev);
        }
    }
}

static void hrcount_reset(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    print_time(stdout);
    printf("hrcount reset\n");
    perf_evsel__disable(ctx->leader);
    perf_evsel__enable(ctx->leader);
    count_dist_reset(ctx->count_dist);
    memset(ctx->perins_pos, 0, ctx->nr_ins * sizeof(u64));
    ctx->rounds = 0;
    ctx->round_nr = 0;
}

/*
 * [INS] tp  1| 20| total 21
 */
static void direct_print(void *opaque, struct count_node *node)
{
    struct prof_dev *dev = opaque;
    struct hrcount_ctx *ctx = dev->private;
    int i, h;
    char buf[64];
    struct tp *tp = &ctx->tp_list->tp[node->id];

    if (tp->alias)
        snprintf(buf, sizeof(buf), "%s", tp->alias);
    else
        snprintf(buf, sizeof(buf), "%s:%s", tp->sys, tp->name);

    if (dev->env->perins)
        printf(ctx->ins_oncpu ? "[%03d] " : "[%6d] ",
               ctx->ins_oncpu ? prof_dev_ins_cpu(dev, node->ins) : prof_dev_ins_thread(dev, node->ins));
    printf("%*s ", ctx->tp_sys_name_max_len, buf);

    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    printf("%*lu", ctx->all_counters_max_len, node->hist[h++]);
    for (i = 1; i < ctx->hist_size; i++) {
        printf("|%*lu", ctx->all_counters_max_len, node->hist[h++]);
    }
    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    memset(&node->hist[h], 0, sizeof(u64) * ctx->hist_size);
    printf(" | total %lu\n", node->sum);
    node->sum = 0;
}

/*
 * [INS] |tp  |tp  |
 * [002] |1 2 |2 3 |
 */
static void packed_print(void *opaque, struct count_node *node)
{
    int i, h, len = 0, max_len;
    u64 max = node->max;
    struct {
        u64 ins;
        u64 id;
        int line_len;
        struct prof_dev *dev;
    } *iter = opaque;
    struct prof_dev *dev = iter->dev;
    struct hrcount_ctx *ctx = dev->private;

    node->max = 0;
    len = strsize(max);
    max_len = ctx->hist_size * (len+1/* ' ' */) - 1;
    if (ctx->pertp_max_len[node->id] < max_len)
        ctx->pertp_max_len[node->id] = max_len;
    max_len = ((ctx->pertp_max_len[node->id] + 1) / ctx->hist_size) - 1 /* ' ' */;

    if (dev->env->perins && iter->ins != node->ins) {
        iter->ins = node->ins;
        iter->id = 0;
        iter->line_len = printf(ctx->ins_oncpu ? "[%03d] " : "[%6d] ",
            ctx->ins_oncpu ? prof_dev_ins_cpu(dev, node->ins) : prof_dev_ins_thread(dev, node->ins));
    }

    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    if (ctx->pipe_char)
        len = printf("|%-*lu", max_len, node->hist[h++]) - 1/* '|' */;
    else
        len = printf("%-*lu", max_len, node->hist[h++]);
    for (i = 1; i < ctx->hist_size; i++) {
        len += printf(" %-*lu", max_len, node->hist[h++]);
    }
    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    memset(&node->hist[h], 0, sizeof(u64) * ctx->hist_size);
    iter->line_len += len;

    iter->line_len += printf("%*s", ctx->pertp_max_len[node->id] - len, "");
    iter->id ++;

    if (iter->id == ctx->tp_list->nr_real_tp) {
        if (ctx->pipe_char)
            iter->line_len += printf("|");
        printf("\n");
        if (dev->tty.istty && iter->line_len > dev->tty.col)
            ctx->packed_display = false;
    }
}

static void __hrcount_interval(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    int len, i;
    u64 print_pos = (ctx->rounds + 1) * ctx->hist_size;
    u64 max_pos = 0;

    // Determine if all instances are complete
    for (i = 0; i < ctx->nr_ins; i++) {
        if (ctx->perins_pos[i] < print_pos)
            return ;
        if (ctx->perins_pos[i] > max_pos)
            max_pos = ctx->perins_pos[i];
    }
    if (ctx->nr_ins >= 2 && ctx->hist_size >= 2 &&
        max_pos - print_pos >= ctx->hist_size/2)
        ctx->need_reset = true;

    len = strsize(count_dist_max(ctx->count_dist));
    if (len > ctx->all_counters_max_len)
        ctx->all_counters_max_len = len;

    print_time(stdout);
    printf("\n");

    if (ctx->packed_display) {
        struct {
            u64 ins;
            u64 id;
            int line_len;
            struct prof_dev *dev;
        } iter;
        struct tp *tp;
        if (dev->env->perins)
            printf(ctx->ins_oncpu ? "[CPU] " : "[THREAD] ");
        for_each_real_tp(ctx->tp_list, tp, i) {
            printf("%s%-*s", ctx->pipe_char ? "|" : "", ctx->pertp_max_len[i], tp->alias ?: tp->name);
        }
        printf("%s\n", ctx->pipe_char ? "|" : "");
        iter.ins = ~0UL;
        iter.id = 0;
        iter.line_len = 0;
        iter.dev = dev;
        count_dist_print(ctx->count_dist, packed_print, &iter);
    } else
        count_dist_print(ctx->count_dist, direct_print, dev);

    ctx->rounds ++;
}

static void hrcount_interval(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    __hrcount_interval(dev);
    if (ctx->need_reset) {
        ctx->need_reset = false;
        hrcount_reset(dev);
    }
}

static void hrcount_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct hrcount_ctx *ctx = dev->private;
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ
    struct sample_type_data {
        struct {
            __u32    pid;
            __u32    tid;
        }    tid_entry;
        __u64  time;
        struct {
            __u32    cpu;
            __u32    reserved;
        }    cpu_entry;
        struct {
            __u64 nr;
            struct {
                __u64 value;
                __u64 id;
            } ctnr[0];
        } groups;
    } *data = (void *)event->sample.array;
    int n = ctx->tp_list->nr_tp;
    u64 *ins_counter = ctx->counters + instance * (n + 1);
    u64 counter, cpu_clock = 0;
    u64 i, j;
    int verbose = dev->env->verbose;
    u64 print_pos = (ctx->rounds + 1) * ctx->hist_size;

    for (i = 0; i < data->groups.nr; i++) {
        struct perf_evsel *evsel;
        struct tp *tp;
        evsel = perf_evlist__id_to_evsel(dev->evlist, data->groups.ctnr[i].id, NULL);
        if (!evsel)
            continue;
        if (evsel == ctx->leader) {
            cpu_clock = data->groups.ctnr[i].value - ins_counter[n];
            ins_counter[n] = data->groups.ctnr[i].value;
            if (cpu_clock >= ctx->period * 2) {
                ctx->perins_pos[instance] += cpu_clock / ctx->period - 1;
                verbose = VERBOSE_NOTICE;
            }
            continue;
        }
        for_each_real_tp(ctx->tp_list, tp, j) {
            if (tp->evsel == evsel) {
                counter = data->groups.ctnr[i].value - ins_counter[j];
                ins_counter[j] = data->groups.ctnr[i].value;
                count_dist_insert(ctx->count_dist, instance, j, 0, ctx->perins_pos[instance], counter);
                break;
            }
        }
    }

    ctx->perins_pos[instance] ++;

    if (verbose) {
        if (dev->print_title) print_time(stdout);
        printf(" %6d/%-6d [%03d]  %llu.%06llu: %s: cpu-clock: %lu ns\n", data->tid_entry.pid, data->tid_entry.tid,
                data->cpu_entry.cpu, data->time/NSEC_PER_SEC, (data->time%NSEC_PER_SEC)/1000, dev->prof->name, cpu_clock);
    }

    /* KERNEL BUG: maybe stuck
     *
     * perf_swevent_hrtimer ->
     *   __perf_event_overflow(throttle=1) ->
     *     __perf_event_account_interrupt ->
     *       perf_log_throttle
     *
     * After the perf event is throttled, it needs to wait for a tick to resume.
     * However, tick may be closed by nohz. It takes a long time to be unthrottled.
    **/
    if (ctx->perins_pos[instance] >= print_pos) {
        ctx->round_nr ++;
        if (ctx->round_nr >= ctx->nr_ins) {
            ctx->round_nr = 0;
            __hrcount_interval(dev);
        }
    }
}

static void __common_help(struct help_ctx *hctx, const char *name)
{
    int i, j;
    struct env *env = hctx->env;

    printf(PROGRAME " %s ", name);
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            if (tp->alias)
                printf("alias=%s/", tp->alias);
            if (!tp->alias)
                printf("[");
            if (!tp->alias)
                printf("alias=./");
            if (!tp->alias)
                printf("]");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->sample_period)
        printf("--period %lu ", env->sample_period);
    common_help(hctx, true, true, false, true, false, true, true);

    if (!env->sample_period)
        printf("[--period ns] ");
    common_help(hctx, false, true, false, true, false, true, true);
    printf("\n");
}

static void hrcount_help(struct help_ctx *hctx)
{
    __common_help(hctx, "hrcount");
}


static const char *hrcount_desc[] = PROFILER_DESC("hrcount",
    "[OPTION...] -e EVENT[...] [--period ns] [--perins]",
    "High-resolution counter.",
    "",
    "SYNOPSIS",
    "    High-resolution counters are capable of displaying count changes at millisecond or",
    "    microsecond granularity.",
    "",
    "EXAMPLES",
    "    "PROGRAME" hrcount -e sched:sched_switch -C 0 --period 50ms -i 1000",
    "    "PROGRAME" hrcount -e sched:sched_switch,sched:sched_wakeup -C 0-5 --period 50ms -i 1000");
static const char *hrcount_argv[] = PROFILER_ARGV("hrcount",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "period", "perins");
static profiler hrcount = {
    .name = "hrcount",
    .desc = hrcount_desc,
    .argv = hrcount_argv,
    .pages = 2,
    .help = hrcount_help,
    .init = hrcount_init,
    .filter = hrcount_filter,
    .deinit = hrcount_exit,
    .sigusr = hrcount_sigusr,
    .interval = hrcount_interval,
    .sample = hrcount_sample,
};
PROFILER_REGISTER(hrcount);

static void stat_help(struct help_ctx *hctx)
{
    __common_help(hctx, "stat");
}

static int stat_read(struct prof_dev *dev, struct perf_evsel *leader, struct perf_counts_values *count, int instance)
{
    struct hrcount_ctx *ctx = dev->private;
    struct perf_counts {
        u64 nr;
        struct {
            u64 value;
            u64 id;
        } ctnr[0];
    } *groups = (void *)count;
    int n = ctx->tp_list->nr_tp;
    u64 *ins_counter = ctx->counters + instance * (n + 1);
    u64 counter, cpu_clock;
    u64 i, j;

    if (leader != ctx->leader)
        return 0;

    for (i = 0; i < groups->nr; i++) {
        struct perf_evsel *evsel;
        struct tp *tp;
        evsel = perf_evlist__id_to_evsel(dev->evlist, groups->ctnr[i].id, NULL);
        if (!evsel)
            continue;
        if (evsel == ctx->leader) {
            cpu_clock = groups->ctnr[i].value - ins_counter[n];
            ins_counter[n] = groups->ctnr[i].value;
            if (cpu_clock >= ctx->period * 2) {
                ctx->perins_pos[instance] += cpu_clock / ctx->period - 1;
            }
            continue;
        }
        for_each_real_tp(ctx->tp_list, tp, j) {
            if (tp->evsel == evsel) {
                counter = groups->ctnr[i].value - ins_counter[j];
                ins_counter[j] = groups->ctnr[i].value;
                count_dist_insert(ctx->count_dist, instance, j, 0, ctx->perins_pos[instance], counter);
                break;
            }
        }
    }

    ctx->perins_pos[instance] ++;
    return 1;
}


static const char *stat_desc[] = PROFILER_DESC("stat",
    "[OPTION...] -e EVENT[...] [--period ns] [--perins]",
    "Periodic counter with lower resolution.",
    "",
    "SYNOPSIS",
    "    Based on hrcount. See '"PROGRAME" hrcount -h' for more information.",
    "",
    "EXAMPLES",
    "    "PROGRAME" stat -e sched:sched_switch -C 0 -i 1000 --period 100ms",
    "    "PROGRAME" stat -e sched:sched_switch,sched:sched_wakeup -C 0-5 -i 1000");
static const char *stat_argv[] = PROFILER_ARGV("stat",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "perins", "period");
static profiler stat = {
    .name = "stat",
    .desc = stat_desc,
    .argv = stat_argv,
    .pages = 1,
    .help = stat_help,
    .init = hrcount_init,
    .filter = hrcount_filter,
    .deinit = hrcount_exit,
    .sigusr = hrcount_sigusr,
    .interval = hrcount_interval,
    .read = stat_read,
};
PROFILER_REGISTER(stat);

