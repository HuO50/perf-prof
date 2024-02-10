#ifndef __TEP_H
#define __TEP_H

#include <event-parse.h>
#include <expr.h>
#include <net.h>
#include <vcpu_info.h>

void pr_stat(const char *fmt, ...);

struct tep_handle *tep__ref(void);
void tep__unref(void);
int tep__event_id(const char *sys, const char *name);
void tep__update_comm(const char *comm, int pid);
const char *tep__pid_to_comm(int pid);
void tep__print_event(unsigned long long ts, int cpu, void *data, int size);
bool tep__event_has_field(int id, const char *field);
bool tep__event_field_size(int id, const char *field);
int tep__event_size(int id);

typedef struct global_var_declare event_fields;
event_fields *tep__event_fields(int id);

struct prof_dev;
struct tp_matcher;

struct tp {
    struct prof_dev *dev;
    union {
        struct perf_evsel *evsel;
        struct prof_dev *source_dev;
    };
    int id; // id > 0: tp:sys; id < 0: profiler;
    char *sys;
    char *name;
    char *filter;
    int stack;
    int max_stack;
    char *alias;
    void *private;
    struct tp_matcher *matcher;

    // top profiler
    struct {
        // long tp_prog_run(struct tp *tp, ...)
        struct expr_prog *field_prog;
        char *field;
        bool event;
        bool top_by;
    } *top_add;
    int nr_top;

    // char *tp_get_comm(struct tp *tp, ...)
    struct expr_prog *comm_prog;
    const char *comm;

    // kmemleak profiler
    struct expr_prog *mem_ptr_prog;
    const char *mem_ptr;
    struct expr_prog *mem_size_prog;
    const char *mem_size;

    // num-dist profiler
    struct expr_prog *num_prog;
    const char *num;

    //multi-trace profiler
    // unsigned long tp_get_key(struct tp *tp, ...)
    struct expr_prog *key_prog;
    const char *key;
    bool untraced;
    bool trigger;

    // event spread
    void *broadcast;
    void *receive;
    bool kernel; // event from kernel

    // vm
    struct vcpu_info *vcpu; // maybe NULL
    const char *vm;

    // A public expression that can be executed by any profiler.
    struct expr_prog *exec_prog;
    const char *exec;

    // cpus
    struct perf_cpu_map *cpus;
};

struct tp_list {
    int nr_tp;
    int nr_real_tp;
    int nr_need_stack;
    bool need_stream_id;
    int nr_top;
    int nr_comm;
    int nr_mem_size;
    int nr_num_prog;
    int nr_untraced;
    int nr_push_to;
    int nr_pull_from;
    int nr_exec_prog;
    struct tp tp[0];
};

struct perf_record_tp {
    struct perf_event_header header;
    u32 id;
    u16 sys_offset;
    u16 name_offset;
    u64 sample_period;
    u64 sample_type;
    u32 event_size;
    u32 unused;
    char str[];
};

struct perf_record_dev {
    struct perf_event_header header;

    // Same as multi_trace_type_header
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU
    u32 pid, tid;
    u64 time;
    u64 id;
    u32 cpu, instance;
    struct prof_dev *dev;
    union perf_event event;
};

enum tp_event_type {
    PERF_RECORD_TP = PERF_RECORD_HEADER_MAX + 1,
    PERF_RECORD_DEV,
};

static inline bool tp_is_dev(struct tp *tp)
{
    return tp->id < 0;
}

#define for_each_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i])

#define for_each_real_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i]) \
         if (!tp_is_dev(_tp))

#define for_each_dev_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i]) \
         if (tp_is_dev(_tp))


struct tp_list *tp_list_new(struct prof_dev *dev, char *event_str);
void tp_list_free(struct tp_list *tp_list);
void tp_update_filter(struct tp *tp, const char *filter);
static inline bool tp_kernel(struct tp *tp)
{
    return tp->kernel;
}
void tp_print_marker(struct tp *tp);

#include <event-spread.h>

struct expr_prog *tp_new_prog(struct tp *tp, char *expr_str);
long tp_prog_run(struct tp *tp, struct expr_prog *prog, void *data, int size);
char *tp_get_comm(struct tp *tp, void *data, int size);
void *tp_get_mem_ptr(struct tp *tp, void *data, int size);
unsigned long tp_get_mem_size(struct tp *tp, void *data, int size);
unsigned long tp_get_key(struct tp *tp, void *data, int size);
unsigned long tp_get_num(struct tp *tp, void *data, int size);

struct perf_evsel *tp_evsel_new(struct tp *tp, struct perf_event_attr *attr);

/*
 * Each tp is used to determine whether it is related to the specified cpu/pid.
 * @raw, @size specifies the tracepoint structure and size. Specify PERF_SAMPLE_RAW
 * to sample the tracepoint structure.
 *
 * Only tp itself knows whether there are fields in its structure that are the
 * same as the specified cpu/pid.
 */
struct tp_matcher {
    struct list_head link;
    const char *sys;
    const char *name;
    bool (*samecpu)(void *raw, int size, int cpu);
    bool (*samepid)(void *raw, int size, int pid);
    bool (*target_cpu)(void *raw, int size, int pid, int *cpu);
};

#define __TP_MATCHER_REGISTER(SYS, NAME, SAMECPU, SAMEPID, TARGET_CPU) \
__attribute__((constructor)) \
static void __PASTE(tp_matcher_register_, __LINE__) (void) \
{ \
    static struct tp_matcher tp_matcher = { \
        .sys = SYS, \
        .name = NAME, \
        .samecpu = SAMECPU, \
        .samepid = SAMEPID, \
        .target_cpu = TARGET_CPU, \
    }; \
    tp_matcher_register(&tp_matcher); \
}

#define TP_MATCHER_REGISTER(a,b,c,d) __TP_MATCHER_REGISTER((a),(b),(c),(d),NULL)
#define TP_MATCHER_REGISTER5(a,b,c,d,e) __TP_MATCHER_REGISTER((a),(b),(c),(d),(e))


void tp_matcher_register(struct tp_matcher *matcher);
struct tp_matcher *tp_matcher_find(char *sys, char *name);

static inline bool tp_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    return tp->matcher && tp->matcher->samecpu ?
            tp->matcher->samecpu(raw, size, cpu) : false;
}

static inline bool tp_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return tp->matcher && tp->matcher->samepid ?
        tp->matcher->samepid(raw, size, pid) : false;
}

static inline bool tp_target_cpu(struct tp *tp, void *raw, int size, int pid, int *cpu)
{
    return tp->matcher && tp->matcher->target_cpu ?
        tp->matcher->target_cpu(raw, size, pid, cpu) : false;
}

#endif

