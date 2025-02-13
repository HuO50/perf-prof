#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <monitor.h>
#include <tep.h>
#include <linux/string.h>
#include <stack_helpers.h>

struct event_care_ctx {
    struct tp_list *tp_list;

    // detect out-of-order
    struct {
        union perf_event *event;
        u64 evtime;
    } *perins_info;

    struct callchain_ctx *cc;
};

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct event_care_ctx *ctx;

    if (!env->event)
        return -1;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;

    tep__ref();

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->perins_info = calloc(prof_dev_nr_ins(dev), sizeof(*ctx->perins_info));
    if (!ctx->perins_info)
        goto free_tp_list;

    if (env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL), stderr);
        dev->pages *= 2;
    }

    dev->private = ctx;
    return 0;

free_tp_list:
    tp_list_free(ctx->tp_list);
failed:
    tep__unref();
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct event_care_ctx *ctx = dev->private;

    if (dev->env->callchain) {
        int i, nr_ins = prof_dev_nr_ins(dev);

        for (i = 0; i < nr_ins; i++)
            if (ctx->perins_info[i].event)
                free(ctx->perins_info[i].event);

        callchain_ctx_free(ctx->cc);
    }
    free(ctx->perins_info);
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static void event_care_exit(struct prof_dev *dev);
static int event_care_init(struct prof_dev *dev)
{
    struct event_care_ctx *ctx;
    struct tp_list *tp_list;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |
                         PERF_SAMPLE_READ | (dev->env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;
    tp_list = ctx->tp_list;

    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(tp_list, tp, i) {
        tp->private = calloc(prof_dev_nr_ins(dev), sizeof(unsigned long));
        if (!tp->private)
            goto failed;

        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(dev->evlist, evsel);
    }

    return 0;

failed:
    event_care_exit(dev);
    return 0;
}

static int event_care_filter(struct prof_dev *dev)
{
    struct event_care_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void event_care_exit(struct prof_dev *dev)
{
    struct event_care_ctx *ctx = dev->private;
    struct tp_list *tp_list = ctx->tp_list;
    struct tp *tp;
    int i;

    for_each_real_tp(tp_list, tp, i) {
        if (tp->private)
            free(tp->private);
    }

    monitor_ctx_exit(dev);
}

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_READ
struct sample_type_header {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   id;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    __u64   period;
    u64     counter;
    u64     read_id; // PERF_FORMAT_ID
    struct callchain callchain;
};

static void print_unorder_event(struct prof_dev *dev, union perf_event *event)
{
    struct event_care_ctx *ctx = dev->private;
    struct tp_list *tp_list = ctx->tp_list;
    struct sample_type_header *hdr = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct tp *tp = NULL;
    int i;
    u64 us;

    evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    for_each_real_tp(tp_list, tp, i) {
        if (tp->evsel == evsel)
            goto found;
    }
    return;

found:
    us = hdr->time/1000;
    prof_dev_print_time(dev, hdr->time, stderr);
    fprintf(stderr, "%16s %6u .... [%03d] %lu.%06lu: %s:%s\n",
            global_comm_get(hdr->tid_entry.tid) ? : "<...>", hdr->tid_entry.tid,
            hdr->cpu_entry.cpu, us/USEC_PER_SEC, us%USEC_PER_SEC,
            tp->sys, tp->name);
    if (dev->env->callchain)
        print_callchain_common(ctx->cc, &hdr->callchain, 0);
}

static void event_care_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct event_care_ctx *ctx = dev->private;
    struct tp_list *tp_list = ctx->tp_list;
    struct sample_type_header *hdr = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct tp *tp = NULL;
    unsigned long *counters;
    int i;

    evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    if (!evsel) {
        fprintf(stderr, "failed to find evsel\n");
        return ;
    }

    for_each_real_tp(tp_list, tp, i) {
        if (tp->evsel == evsel)
            goto found;
    }
    fprintf(stderr, "failed to find tracepoint\n");
    return ;

found:
    counters = tp->private;
    // prof_dev_atomic_enable() will discard some events, and counters will no longer be used to detect
    // lost for the first time.
    if (counters[instance] && hdr->counter - counters[instance] != hdr->period) {
        fprintf(stderr, "%s:%s lost %lu events\n", tp->sys, tp->name, hdr->counter - counters[instance] - 1);
    }
    counters[instance] = hdr->counter;

    if (hdr->time < ctx->perins_info[instance].evtime) {
        print_time(stderr);
        fprintf(stderr, " %s:%s out-of-order %llu < %lu\n", tp->sys, tp->name, hdr->time, ctx->perins_info[instance].evtime);
        if (dev->env->callchain) {
            print_unorder_event(dev, ctx->perins_info[instance].event);
            print_unorder_event(dev, event);
        }
    } else {
        if (dev->env->callchain) {
            if (ctx->perins_info[instance].event)
                free(ctx->perins_info[instance].event);
            ctx->perins_info[instance].event = memdup(event, event->header.size);
        }
        ctx->perins_info[instance].evtime = hdr->time;
    }
}


static const char *event_care_desc[] = PROFILER_DESC("event-care",
    "[OPTION...] -e EVENT",
    "Care if any events are lost or out-of-order.", "",
    "EXAMPLES",
    "    "PROGRAME" event-care -e sched:sched_wakeup -m 64");
static const char *event_care_argv[] = PROFILER_ARGV("event-care",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "call-graph");
static profiler event_care = {
    .name = "event-care",
    .desc = event_care_desc,
    .argv = event_care_argv,
    .pages = 2,
    .init = event_care_init,
    .filter = event_care_filter,
    .deinit = event_care_exit,
    .sample = event_care_sample,
};
PROFILER_REGISTER(event_care);

