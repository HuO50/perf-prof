#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <monitor.h>

#ifdef CONFIG_LIBBPF

#include "perf_event.skel.h"

static int libbpf_print_fn(enum libbpf_print_level level,
            const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    return vfprintf(stderr, format, args);
}

int bpf_filter_open(struct bpf_filter *filter)
{
    struct perf_event_bpf *obj = NULL;
    struct rlimit old_rlim;
    struct rlimit new_rlim;
    bool restore = false;
    int err;

    libbpf_set_print(libbpf_print_fn);

    obj = perf_event_bpf__open();
    if (!obj) {
        fprintf(stderr, "failed to open and/or load BPF object\n");
        return -1;
    }

    bpf_program__set_type(obj->progs.perf_event_do_filter, BPF_PROG_TYPE_PERF_EVENT);

    #define ASSIGN(a) obj->rodata->a = filter->a
    ASSIGN(filter_irqs_disabled);
    ASSIGN(irqs_disabled);
    ASSIGN(filter_tif_need_resched);
    ASSIGN(tif_need_resched);
    ASSIGN(filter_exclude_pid);
    ASSIGN(exclude_pid);
    ASSIGN(filter_nr_running);
    ASSIGN(nr_running_min);
    ASSIGN(nr_running_max);

    // Bump memlock so we can get reasonably sized bpf maps or progs.
    if (getrlimit(RLIMIT_MEMLOCK, &old_rlim) == 0) {
        new_rlim.rlim_cur = RLIM_INFINITY;
        new_rlim.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_MEMLOCK, &new_rlim) == 0)
            restore = true;
        else {
            fprintf(stderr, "Couldn't bump rlimit(MEMLOCK), %s(%d)\n", strerror(errno), errno);
        }
    }

    err = perf_event_bpf__load(obj);

    if (restore)
        setrlimit(RLIMIT_MEMLOCK, &old_rlim);

    if (err) {
        fprintf(stderr, "failed to load BPF object: %d\n", err);
        goto cleanup;
    }

    filter->obj = obj;
    filter->bpf_fd = bpf_program__fd(obj->progs.perf_event_do_filter);

    return 0;

cleanup:
    perf_event_bpf__destroy(obj);
    filter->obj = NULL;
    filter->bpf_fd = -1;
    return err;
}

void bpf_filter_close(struct bpf_filter *filter)
{
    if (filter && filter->obj) {
        perf_event_bpf__destroy(filter->obj);
        filter->obj = NULL;
        filter->bpf_fd = -1;
    }
}

#else

int bpf_filter_open(struct bpf_filter *filter)
{
    filter->obj = NULL;
    filter->bpf_fd = -1;
    return 0;
}
void bpf_filter_close(struct bpf_filter *filter) {}

#endif


int bpf_filter_init(struct bpf_filter *filter, struct env *env)
{
    int need_bpf = 0;

    filter->obj = NULL;
    filter->bpf_fd = -1;

    if (env->irqs_disabled_set) {
        filter->filter_irqs_disabled = true;
        filter->irqs_disabled = env->irqs_disabled;
        need_bpf ++;
    }

    if (env->tif_need_resched_set) {
        filter->filter_tif_need_resched = true;
        filter->tif_need_resched = env->tif_need_resched;
        need_bpf ++;
    }

    if (env->exclude_pid_set) {
        filter->filter_exclude_pid = true;
        filter->exclude_pid = env->exclude_pid;
        need_bpf ++;
    }

    if (env->nr_running_min_set || env->nr_running_max_set) {
        filter->filter_nr_running = true;
        filter->nr_running_min = env->nr_running_min_set ? env->nr_running_min : 0;
        filter->nr_running_max = env->nr_running_max_set ? env->nr_running_max : 0xffffffff;
        need_bpf ++;
    }

    return need_bpf;
}

