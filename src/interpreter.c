/*
 * Riz Programming Language
 * interpreter.c — Tree-walking interpreter (Phase 3: +struct/impl, try/catch/throw)
 *
 * Phase 2 additions:
 *   - Dictionary type with {key: value} literals
 *   - Pattern matching (match expression)
 *   - String/List/Dict method calls (obj.method())
 *   - import "file.riz" support
 *   - New builtins: format, sorted, reversed, enumerate, zip, keys, values, assert, exit
 *   - More builtins: clamp, sign, floor, ceil, round, all, any, bool, ord, chr, extend
 *   - debug / panic builtins; call stack on uncaught throw
 */

#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "riz_import.h"
#include "riz_plugin.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ═══ Forward declarations ═══ */

static RizValue eval(Interpreter* I, ASTNode* node);
static void exec_block(Interpreter* I, ASTNode* block);
static RizValue call_function(Interpreter* I, RizFunction* fn, RizValue* args, int argc);
static void interpreter_clear_error_stack(Interpreter* I);
static void interpreter_snapshot_error_stack(Interpreter* I);
static Interpreter* g_interp = NULL;

/* Forward: struct method helper */
void riz_struct_add_method(RizStructDef* def, const char* name, RizValue fn_val);

/* ═══════════════════════════════════════════════════════
 *  Built-in / Native Functions (Phase 1 + Phase 2)
 * ═══════════════════════════════════════════════════════ */

RizValue native_print(RizValue* args, int argc) {
    for (int i = 0; i < argc; i++) { if (i > 0) printf(" "); riz_value_print(args[i]); }
    printf("\n"); return riz_none();
}
RizValue native_len(RizValue* args, int argc) {
    if (argc!=1){ riz_runtime_error("len() takes 1 argument"); return riz_none(); }
    if (args[0].type==VAL_LIST)   return riz_int(args[0].as.list->count);
    if (args[0].type==VAL_STRING) return riz_int((int64_t)strlen(args[0].as.string));
    if (args[0].type==VAL_DICT)   return riz_int(args[0].as.dict->count);
    riz_runtime_error("len() argument must be list, string, or dict"); return riz_none();
}
RizValue native_range(RizValue* args, int argc) {
    int64_t start=0,stop=0,step=1;
    if (argc==1&&args[0].type==VAL_INT) stop=args[0].as.integer;
    else if (argc==2&&args[0].type==VAL_INT&&args[1].type==VAL_INT) { start=args[0].as.integer; stop=args[1].as.integer; }
    else if (argc==3&&args[0].type==VAL_INT&&args[1].type==VAL_INT&&args[2].type==VAL_INT) { start=args[0].as.integer; stop=args[1].as.integer; step=args[2].as.integer; }
    else { riz_runtime_error("range() takes 1-3 int args"); return riz_none(); }
    if (step==0) { riz_runtime_error("range() step cannot be zero"); return riz_none(); }
    RizValue list = riz_list_new();
    if (step>0) for(int64_t i=start;i<stop;i+=step) riz_list_append(list.as.list, riz_int(i));
    else        for(int64_t i=start;i>stop;i+=step) riz_list_append(list.as.list, riz_int(i));
    return list;
}
RizValue native_type(RizValue* a, int c) { return c==1 ? riz_string(riz_value_type_name(a[0])) : riz_none(); }
RizValue native_str(RizValue* a, int c) {
    if (c!=1) return riz_none();
    char* s = riz_value_to_string(a[0]); return riz_string_take(s);
}
RizValue native_int_cast(RizValue* a, int c) {
    if (c!=1) return riz_none();
    switch (a[0].type) {
        case VAL_INT: return a[0]; case VAL_FLOAT: return riz_int((int64_t)a[0].as.floating);
        case VAL_BOOL: return riz_int(a[0].as.boolean?1:0);
        case VAL_STRING: { char*e; int64_t v=strtoll(a[0].as.string,&e,10);
            if(*e) { riz_runtime_error("Cannot convert '%s' to int",a[0].as.string); return riz_none(); }
            return riz_int(v); }
        default: riz_runtime_error("Cannot convert %s to int",riz_value_type_name(a[0])); return riz_none();
    }
}
RizValue native_float_cast(RizValue* a, int c) {
    if (c!=1) return riz_none();
    switch (a[0].type) {
        case VAL_INT: return riz_float((double)a[0].as.integer); case VAL_FLOAT: return a[0];
        case VAL_BOOL: return riz_float(a[0].as.boolean?1.0:0.0);
        case VAL_STRING: { char*e; double v=strtod(a[0].as.string,&e);
            if(*e) { riz_runtime_error("Cannot convert '%s' to float",a[0].as.string); return riz_none(); }
            return riz_float(v); }
        default: riz_runtime_error("Cannot convert %s to float",riz_value_type_name(a[0])); return riz_none();
    }
}
RizValue native_input(RizValue* a, int c) {
    if (c>0&&a[0].type==VAL_STRING) { printf("%s",a[0].as.string); fflush(stdout); }
    char buf[RIZ_LINE_BUF_SIZE];
    if (fgets(buf,sizeof(buf),stdin)) { size_t l=strlen(buf); if(l>0&&buf[l-1]=='\n')buf[l-1]='\0'; return riz_string(buf); }
    return riz_string("");
}
RizValue native_append(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("append(list, val) expected"); return riz_none(); }
    riz_list_append(a[0].as.list, riz_value_copy(a[1])); return riz_none();
}
RizValue native_pop(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_LIST) return riz_none();
    RizList* l=a[0].as.list; if(l->count==0){riz_runtime_error("pop() from empty list");return riz_none();}
    return l->items[--l->count];
}
RizValue native_abs(RizValue* a, int c) {
    if (c!=1) return riz_none();
    if (a[0].type==VAL_INT){ int64_t v=a[0].as.integer; return riz_int(v<0?-v:v); }
    if (a[0].type==VAL_FLOAT) return riz_float(fabs(a[0].as.floating));
    return riz_none();
}
RizValue native_min(RizValue* a, int c) {
    if (c==0) return riz_none();
    if (c==1&&a[0].type==VAL_LIST) {
        RizList*l=a[0].as.list; if(!l->count)return riz_none(); RizValue b=l->items[0];
        for(int i=1;i<l->count;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=l->items[i].type==VAL_INT?(double)l->items[i].as.integer:l->items[i].as.floating;if(cv<bv)b=l->items[i];}
        return riz_value_copy(b);
    }
    RizValue b=a[0]; for(int i=1;i<c;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=a[i].type==VAL_INT?(double)a[i].as.integer:a[i].as.floating;if(cv<bv)b=a[i];}
    return riz_value_copy(b);
}
RizValue native_max(RizValue* a, int c) {
    if (c==0) return riz_none();
    if (c==1&&a[0].type==VAL_LIST) {
        RizList*l=a[0].as.list; if(!l->count)return riz_none(); RizValue b=l->items[0];
        for(int i=1;i<l->count;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=l->items[i].type==VAL_INT?(double)l->items[i].as.integer:l->items[i].as.floating;if(cv>bv)b=l->items[i];}
        return riz_value_copy(b);
    }
    RizValue b=a[0]; for(int i=1;i<c;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=a[i].type==VAL_INT?(double)a[i].as.integer:a[i].as.floating;if(cv>bv)b=a[i];}
    return riz_value_copy(b);
}
RizValue native_sum(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_LIST) return riz_none();
    bool hf=false; double t=0;
    for(int i=0;i<a[0].as.list->count;i++){
        if(a[0].as.list->items[i].type==VAL_INT) t+=(double)a[0].as.list->items[i].as.integer;
        else if(a[0].as.list->items[i].type==VAL_FLOAT){t+=a[0].as.list->items[i].as.floating;hf=true;}
    }
    return hf?riz_float(t):riz_int((int64_t)t);
}

typedef struct {
    const RizValue* items;
    int start;
    int end;
    double partial;
} ParallelSumTask;

static int native_cpu_count_raw(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)(info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1);
#else
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && n <= INT_MAX) return (int)n;
#endif
    return 1;
#endif
}

static double sum_numeric_slice(const RizValue* items, int start, int end) {
    double total = 0.0;
    for (int i = start; i < end; i++) {
        if (items[i].type == VAL_INT) total += (double)items[i].as.integer;
        else total += items[i].as.floating;
    }
    return total;
}

static RizValue make_numeric_sum_value(double total, bool has_float) {
    return has_float ? riz_float(total) : riz_int((int64_t)total);
}

#ifdef _WIN32
static DWORD WINAPI parallel_sum_worker(LPVOID param) {
    ParallelSumTask* task = (ParallelSumTask*)param;
    task->partial = sum_numeric_slice(task->items, task->start, task->end);
    return 0;
}
#endif

RizValue native_cpu_count(RizValue* args, int argc) {
    (void)args;
    if (argc != 0) {
        riz_runtime_error("cpu_count() takes no arguments");
        return riz_none();
    }
    return riz_int((int64_t)native_cpu_count_raw());
}

RizValue native_parallel_sum(RizValue* args, int argc) {
    if (argc < 1 || argc > 2 || args[0].type != VAL_LIST) {
        riz_runtime_error("parallel_sum(list[, workers]) expected");
        return riz_none();
    }

    int requested_workers = 0;
    if (argc == 2) {
        if (args[1].type != VAL_INT) {
            riz_runtime_error("parallel_sum(): workers must be int");
            return riz_none();
        }
        if (args[1].as.integer <= 0) {
            riz_runtime_error("parallel_sum(): workers must be >= 1");
            return riz_none();
        }
        requested_workers = (args[1].as.integer > INT_MAX) ? INT_MAX : (int)args[1].as.integer;
    }

    RizList* list = args[0].as.list;
    if (list->count == 0) return riz_int(0);

    bool has_float = false;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type == VAL_INT) continue;
        if (list->items[i].type == VAL_FLOAT) {
            has_float = true;
            continue;
        }
        riz_runtime_error("parallel_sum(): list must contain only int/float");
        return riz_none();
    }

    int workers = requested_workers > 0 ? requested_workers : native_cpu_count_raw();
    if (workers < 1) workers = 1;
    if (workers > list->count) workers = list->count;

    if (workers <= 1 || list->count < 4096) {
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

#ifdef _WIN32
    ParallelSumTask* tasks = (ParallelSumTask*)calloc((size_t)workers, sizeof(ParallelSumTask));
    HANDLE* handles = (HANDLE*)calloc((size_t)workers, sizeof(HANDLE));
    if (!tasks || !handles) {
        free(tasks);
        free(handles);
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

    int base = list->count / workers;
    int rem = list->count % workers;
    int start = 0;
    int launched = 0;
    bool spawn_failed = false;

    for (int i = 0; i < workers; i++) {
        int span = base + (i < rem ? 1 : 0);
        tasks[i].items = list->items;
        tasks[i].start = start;
        tasks[i].end = start + span;
        start += span;

        handles[i] = CreateThread(NULL, 0, parallel_sum_worker, &tasks[i], 0, NULL);
        if (!handles[i]) {
            spawn_failed = true;
            break;
        }
        launched++;
    }

    if (spawn_failed) {
        for (int i = 0; i < launched; i++) {
            WaitForSingleObject(handles[i], INFINITE);
            CloseHandle(handles[i]);
        }
        free(tasks);
        free(handles);
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

    double total = 0.0;
    for (int i = 0; i < workers; i++) {
        WaitForSingleObject(handles[i], INFINITE);
        CloseHandle(handles[i]);
        total += tasks[i].partial;
    }
    free(tasks);
    free(handles);
    return make_numeric_sum_value(total, has_float);
#else
    /* Non-Windows builds keep compatibility by falling back to serial sum. */
    double total = sum_numeric_slice(list->items, 0, list->count);
    return make_numeric_sum_value(total, has_float);
#endif
}

RizValue native_time_fn(RizValue* a, int c) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ft.dwHighDateTime;
    t <<= 32;
    t |= ft.dwLowDateTime;
    return riz_float((double)t / 10000000.0 - 11644473600.0);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return riz_float((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0);
#endif
}

/* Phase 2 native map/filter — forward declared */
RizValue native_map(RizValue* a, int c);
RizValue native_filter(RizValue* a, int c);

/* Phase 2 new builtins */

RizValue native_format(RizValue* args, int argc) {
    if (argc<1||args[0].type!=VAL_STRING) { riz_runtime_error("format() first arg must be string"); return riz_none(); }
    const char* tmpl = args[0].as.string;
    size_t cap=256; char* res=(char*)malloc(cap); size_t len=0;
    int arg_idx = 1;
    for (const char* p = tmpl; *p; p++) {
        if (*p=='{' && *(p+1)=='}') {
            if (arg_idx < argc) {
                char* s = riz_value_to_string(args[arg_idx++]);
                size_t sl=strlen(s); while(len+sl+1>=cap){cap*=2;res=(char*)realloc(res,cap);}
                memcpy(res+len,s,sl); len+=sl; free(s);
            }
            p++; /* skip '}' */
        } else {
            if(len+2>=cap){cap*=2;res=(char*)realloc(res,cap);}
            res[len++]=*p;
        }
    }
    res[len]='\0'; return riz_string_take(res);
}

RizValue native_sorted(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) { riz_runtime_error("sorted() takes 1 list arg"); return riz_none(); }
    RizList* src = args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=0;i<src->count;i++) riz_list_append(result.as.list, riz_value_copy(src->items[i]));
    /* insertion sort */
    RizList* l = result.as.list;
    for (int i=1;i<l->count;i++) {
        RizValue key=l->items[i]; int j=i-1;
        double kv = key.type==VAL_INT?(double)key.as.integer:key.as.floating;
        while(j>=0) {
            double jv=l->items[j].type==VAL_INT?(double)l->items[j].as.integer:l->items[j].as.floating;
            if (jv<=kv) break;
            l->items[j+1]=l->items[j]; j--;
        }
        l->items[j+1]=key;
    }
    return result;
}

RizValue native_reversed(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) return riz_none();
    RizList* src = args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=src->count-1;i>=0;i--) riz_list_append(result.as.list, riz_value_copy(src->items[i]));
    return result;
}

RizValue native_enumerate(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) return riz_none();
    RizList* src=args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=0;i<src->count;i++) {
        RizValue pair = riz_list_new();
        riz_list_append(pair.as.list, riz_int(i));
        riz_list_append(pair.as.list, riz_value_copy(src->items[i]));
        riz_list_append(result.as.list, pair);
    }
    return result;
}

RizValue native_zip(RizValue* args, int argc) {
    if (argc!=2||args[0].type!=VAL_LIST||args[1].type!=VAL_LIST) return riz_none();
    RizList*a=args[0].as.list; RizList*b=args[1].as.list;
    int cnt = a->count<b->count ? a->count : b->count;
    RizValue result = riz_list_new();
    for (int i=0;i<cnt;i++) {
        RizValue pair = riz_list_new();
        riz_list_append(pair.as.list, riz_value_copy(a->items[i]));
        riz_list_append(pair.as.list, riz_value_copy(b->items[i]));
        riz_list_append(result.as.list, pair);
    }
    return result;
}

RizValue native_keys(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_DICT){riz_runtime_error("keys() takes 1 dict arg");return riz_none();}
    return riz_dict_keys(a[0].as.dict);
}
RizValue native_values(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_DICT){riz_runtime_error("values() takes 1 dict arg");return riz_none();}
    return riz_dict_values(a[0].as.dict);
}

RizValue native_assert(RizValue* args, int argc) {
    if (argc<1) { riz_runtime_error("assert() requires at least 1 argument"); return riz_none(); }
    if (!riz_value_is_truthy(args[0])) {
        if (argc>=2 && args[1].type==VAL_STRING)
            riz_runtime_error("Assertion failed: %s", args[1].as.string);
        else
            riz_runtime_error("Assertion failed");
    }
    return riz_none();
}

/* Observable debugging (stderr); returns first argument unchanged. Optional label via second arg. */
RizValue native_debug(RizValue* args, int argc) {
    if (argc < 1) { riz_runtime_error("debug() requires at least 1 argument"); return riz_none(); }
    int line = (g_interp && g_interp->current_line > 0) ? g_interp->current_line : 0;
    fprintf(stderr, COL_DIM "[debug line %d]" COL_RESET " ", line);
    if (argc >= 2) {
        char* lab = riz_value_to_string(args[1]);
        fprintf(stderr, "%s: ", lab);
        free(lab);
    }
    char* s = riz_value_to_string(args[0]);
    fprintf(stderr, "%s\n", s);
    free(s);
    return riz_value_copy(args[0]);
}

/* Fatal error with optional message (any value); prints call stack then exit(1). */
RizValue native_panic(RizValue* args, int argc) {
    char* msg = argc >= 1 ? riz_value_to_string(args[0]) : riz_strdup("panic");
    fprintf(stderr, COL_RED COL_BOLD "panic:" COL_RESET " %s\n", msg);
    free(msg);
    if (g_interp && g_interp->call_stack_len > 0) {
        fprintf(stderr, COL_DIM "Call stack (innermost last):\n" COL_RESET);
        for (int i = 0; i < g_interp->call_stack_len; i++)
            fprintf(stderr, COL_DIM "  at %s\n" COL_RESET, g_interp->call_stack[i]);
    }
    exit(1);
    return riz_none();
}

RizValue native_exit(RizValue* args, int argc) {
    int code = 0; if (argc == 1 && args[0].type == VAL_INT) code = args[0].as.integer;
    exit(code);
    return riz_none();
}

/* Phase 4: File I/O */
RizValue native_read_file(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) { riz_runtime_error("read_file requires 1 string argument (path)"); return riz_none(); }
    FILE* f = fopen(args[0].as.string, "rb");
    if (!f) return riz_none();
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* string = (char*)malloc(fsize + 1);
    fread(string, 1, fsize, f);
    fclose(f);
    string[fsize] = '\0';
    return riz_string_take(string);
}

RizValue native_write_file(RizValue* args, int argc) {
    if (argc != 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) { riz_runtime_error("write_file requires (path: str, content: str)"); return riz_none(); }
    FILE* f = fopen(args[0].as.string, "wb");
    if (!f) return riz_bool(false);
    size_t len = strlen(args[1].as.string);
    fwrite(args[1].as.string, 1, len, f);
    fclose(f);
    return riz_bool(true);
}

RizValue native_has_key(RizValue* args, int argc) {
    if (argc!=2||args[0].type!=VAL_DICT||args[1].type!=VAL_STRING) return riz_bool(false);
    return riz_bool(riz_dict_has(args[0].as.dict, args[1].as.string));
}

/* Exported for AOT (aot_runtime.c links interpreter.c). */
RizValue native_clamp(RizValue* args, int argc) {
    if (argc != 3) { riz_runtime_error("clamp(value, lo, hi) takes 3 arguments"); return riz_none(); }
    double v, lo, hi;
    if (args[0].type == VAL_INT) v = (double)args[0].as.integer;
    else if (args[0].type == VAL_FLOAT) v = args[0].as.floating;
    else { riz_runtime_error("clamp(): value must be int or float"); return riz_none(); }
    if (args[1].type == VAL_INT) lo = (double)args[1].as.integer;
    else if (args[1].type == VAL_FLOAT) lo = args[1].as.floating;
    else { riz_runtime_error("clamp(): lo must be int or float"); return riz_none(); }
    if (args[2].type == VAL_INT) hi = (double)args[2].as.integer;
    else if (args[2].type == VAL_FLOAT) hi = args[2].as.floating;
    else { riz_runtime_error("clamp(): hi must be int or float"); return riz_none(); }
    if (lo > hi) { double t = lo; lo = hi; hi = t; }
    double c = v < lo ? lo : (v > hi ? hi : v);
    bool all_int = (args[0].type == VAL_INT && args[1].type == VAL_INT && args[2].type == VAL_INT);
    if (all_int && c == floor(c)) return riz_int((int64_t)c);
    return riz_float(c);
}

RizValue native_sign(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("sign(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) {
        int64_t x = args[0].as.integer;
        return riz_int(x > 0 ? 1 : (x < 0 ? -1 : 0));
    }
    if (args[0].type == VAL_FLOAT) {
        double x = args[0].as.floating;
        return riz_int(x > 0.0 ? 1 : (x < 0.0 ? -1 : 0));
    }
    riz_runtime_error("sign(): expected int or float"); return riz_none();
}

RizValue native_floor_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("floor(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(floor(args[0].as.floating));
    riz_runtime_error("floor(): expected int or float"); return riz_none();
}

RizValue native_ceil_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("ceil(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(ceil(args[0].as.floating));
    riz_runtime_error("ceil(): expected int or float"); return riz_none();
}

RizValue native_round_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("round(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(round(args[0].as.floating));
    riz_runtime_error("round(): expected int or float"); return riz_none();
}

RizValue native_all(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_LIST) { riz_runtime_error("all(list) takes 1 list"); return riz_none(); }
    RizList* L = args[0].as.list;
    for (int i = 0; i < L->count; i++)
        if (!riz_value_is_truthy(L->items[i])) return riz_bool(false);
    return riz_bool(true);
}

RizValue native_any(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_LIST) { riz_runtime_error("any(list) takes 1 list"); return riz_none(); }
    RizList* L = args[0].as.list;
    for (int i = 0; i < L->count; i++)
        if (riz_value_is_truthy(L->items[i])) return riz_bool(true);
    return riz_bool(false);
}

RizValue native_as_bool(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("bool(x) takes 1 argument"); return riz_none(); }
    return riz_bool(riz_value_is_truthy(args[0]));
}

RizValue native_ord(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) { riz_runtime_error("ord(s) takes 1 string"); return riz_none(); }
    const char* s = args[0].as.string;
    size_t n = strlen(s);
    if (n != 1) { riz_runtime_error("ord() expects a single-character string"); return riz_none(); }
    return riz_int((int64_t)(unsigned char)s[0]);
}

RizValue native_chr(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_INT) { riz_runtime_error("chr(n) takes 1 int"); return riz_none(); }
    int64_t n = args[0].as.integer;
    if (n < 0 || n > 255) { riz_runtime_error("chr(): n must be in 0..255"); return riz_none(); }
    char buf[2] = { (char)n, '\0' };
    return riz_string(buf);
}

RizValue native_extend(RizValue* args, int argc) {
    if (argc != 2 || args[0].type != VAL_LIST || args[1].type != VAL_LIST) {
        riz_runtime_error("extend(list, other) requires two lists"); return riz_none();
    }
    RizList* a = args[0].as.list;
    RizList* b = args[1].as.list;
    for (int i = 0; i < b->count; i++)
        riz_list_append(a, riz_value_copy(b->items[i]));
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Method dispatch for built-in types
 * ═══════════════════════════════════════════════════════ */

RizValue string_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    (void)I;
    const char* s = obj.as.string;
    size_t slen = strlen(s);

    if (strcmp(method,"upper")==0 && argc==0) {
        char* r=riz_strdup(s); for(size_t i=0;i<slen;i++) r[i]=(char)toupper((unsigned char)r[i]);
        return riz_string_take(r);
    }
    if (strcmp(method,"lower")==0 && argc==0) {
        char* r=riz_strdup(s); for(size_t i=0;i<slen;i++) r[i]=(char)tolower((unsigned char)r[i]);
        return riz_string_take(r);
    }
    if (strcmp(method,"trim")==0 && argc==0) {
        while(slen>0 && isspace((unsigned char)s[0])) { s++; slen--; }
        while(slen>0 && isspace((unsigned char)s[slen-1])) slen--;
        char* r=(char*)malloc(slen+1); memcpy(r,s,slen); r[slen]='\0';
        return riz_string_take(r);
    }
    if (strcmp(method,"split")==0) {
        const char* sep = (argc>=1 && args[0].type==VAL_STRING) ? args[0].as.string : " ";
        size_t seplen = strlen(sep);
        RizValue result = riz_list_new();
        const char* p = s;
        if (seplen==0) {
            /* split into chars */
            for(size_t i=0;i<slen;i++) { char c[2]={s[i],'\0'}; riz_list_append(result.as.list,riz_string(c)); }
        } else {
            while(1) {
                const char* found = strstr(p, sep);
                if (!found) { riz_list_append(result.as.list, riz_string(p)); break; }
                size_t part_len = found - p;
                char* part=(char*)malloc(part_len+1); memcpy(part,p,part_len); part[part_len]='\0';
                riz_list_append(result.as.list, riz_string_take(part));
                p = found + seplen;
            }
        }
        return result;
    }
    if (strcmp(method,"contains")==0 && argc==1 && args[0].type==VAL_STRING) {
        return riz_bool(strstr(s, args[0].as.string) != NULL);
    }
    if (strcmp(method,"starts_with")==0 && argc==1 && args[0].type==VAL_STRING) {
        return riz_bool(strncmp(s, args[0].as.string, strlen(args[0].as.string)) == 0);
    }
    if (strcmp(method,"ends_with")==0 && argc==1 && args[0].type==VAL_STRING) {
        size_t sl=strlen(args[0].as.string);
        return riz_bool(slen>=sl && strcmp(s+slen-sl, args[0].as.string)==0);
    }
    if (strcmp(method,"replace")==0 && argc==2 && args[0].type==VAL_STRING && args[1].type==VAL_STRING) {
        const char* old=args[0].as.string; const char* new_s=args[1].as.string;
        size_t old_len=strlen(old), new_len=strlen(new_s);
        if (old_len==0) return riz_string(s);
        /* Count occurrences */
        int count=0; const char* p=s;
        while((p=strstr(p,old))!=NULL){count++;p+=old_len;}
        size_t rlen = slen + count*(new_len - old_len);
        char* r=(char*)malloc(rlen+1); char* dst=r; p=s;
        while(1) {
            const char* found=strstr(p,old);
            if(!found){strcpy(dst,p);break;}
            memcpy(dst,p,found-p);dst+=found-p;
            memcpy(dst,new_s,new_len);dst+=new_len;
            p=found+old_len;
        }
        return riz_string_take(r);
    }
    if (strcmp(method,"chars")==0 && argc==0) {
        RizValue result = riz_list_new();
        for(size_t i=0;i<slen;i++){char c[2]={s[i],'\0'}; riz_list_append(result.as.list,riz_string(c));}
        return result;
    }
    if (strcmp(method,"repeat")==0 && argc==1 && args[0].type==VAL_INT) {
        int64_t n=args[0].as.integer; if(n<=0)return riz_string("");
        char* r=(char*)malloc(slen*n+1);
        for(int64_t i=0;i<n;i++) memcpy(r+i*slen,s,slen);
        r[slen*n]='\0'; return riz_string_take(r);
    }
    if (strcmp(method,"find")==0 && argc==1 && args[0].type==VAL_STRING) {
        const char* found = strstr(s, args[0].as.string);
        return riz_int(found ? (int64_t)(found - s) : -1);
    }
    riz_runtime_error("str has no method '%s'", method);
    return riz_none();
}

RizValue list_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    RizList* list = obj.as.list;

    if (strcmp(method,"push")==0 && argc==1) {
        riz_list_append(list, riz_value_copy(args[0])); return riz_none();
    }
    if (strcmp(method,"pop")==0 && argc==0) {
        if(list->count==0){riz_runtime_error("pop() from empty list");return riz_none();}
        return list->items[--list->count];
    }
    if (strcmp(method,"sort")==0 && argc==0) {
        for(int i=1;i<list->count;i++){
            RizValue key=list->items[i];int j=i-1;
            double kv=key.type==VAL_INT?(double)key.as.integer:key.as.floating;
            while(j>=0){double jv=list->items[j].type==VAL_INT?(double)list->items[j].as.integer:list->items[j].as.floating;if(jv<=kv)break;list->items[j+1]=list->items[j];j--;}
            list->items[j+1]=key;
        }
        return riz_none();
    }
    if (strcmp(method,"reverse")==0 && argc==0) {
        for(int i=0,j=list->count-1;i<j;i++,j--){RizValue t=list->items[i];list->items[i]=list->items[j];list->items[j]=t;}
        return riz_none();
    }
    if (strcmp(method,"join")==0) {
        const char* sep=(argc>=1&&args[0].type==VAL_STRING)?args[0].as.string:"";
        size_t cap=128; char* r=(char*)malloc(cap); size_t len=0; size_t sl=strlen(sep);
        for(int i=0;i<list->count;i++){
            if(i>0){while(len+sl+1>=cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,sep,sl);len+=sl;}
            char* s=riz_value_to_string(list->items[i]);size_t il=strlen(s);
            while(len+il+1>=cap){cap*=2;r=(char*)realloc(r,cap);}
            memcpy(r+len,s,il);len+=il;free(s);
        }
        r[len]='\0'; return riz_string_take(r);
    }
    if (strcmp(method,"contains")==0 && argc==1) {
        for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) return riz_bool(true);
        return riz_bool(false);
    }
    if (strcmp(method,"index")==0 && argc==1) {
        for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) return riz_int(i);
        return riz_int(-1);
    }
    if (strcmp(method,"count")==0 && argc==1) {
        int n=0; for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) n++;
        return riz_int(n);
    }
    if (strcmp(method,"slice")==0) {
        int start=(argc>=1&&args[0].type==VAL_INT)?(int)args[0].as.integer:0;
        int end=(argc>=2&&args[1].type==VAL_INT)?(int)args[1].as.integer:list->count;
        if(start<0)start+=list->count; if(end<0)end+=list->count;
        if(start<0)start=0; if(end>list->count)end=list->count;
        RizValue result = riz_list_new();
        for(int i=start;i<end;i++) riz_list_append(result.as.list, riz_value_copy(list->items[i]));
        return result;
    }
    if (strcmp(method,"map")==0 && argc==1) {
        RizValue fn_val=args[0]; RizValue result=riz_list_new();
        for(int i=0;i<list->count;i++){
            RizValue item=list->items[i]; RizValue mapped;
            if(fn_val.type==VAL_FUNCTION) mapped=call_function(I,fn_val.as.function,&item,1);
            else if(fn_val.type==VAL_NATIVE_FN) mapped=fn_val.as.native->fn(&item,1);
            else {riz_runtime_error("map() arg must be callable");return riz_none();}
            riz_list_append(result.as.list,mapped);
        }
        return result;
    }
    if (strcmp(method,"filter")==0 && argc==1) {
        RizValue fn_val=args[0]; RizValue result=riz_list_new();
        for(int i=0;i<list->count;i++){
            RizValue item=list->items[i]; RizValue keep;
            if(fn_val.type==VAL_FUNCTION) keep=call_function(I,fn_val.as.function,&item,1);
            else if(fn_val.type==VAL_NATIVE_FN) keep=fn_val.as.native->fn(&item,1);
            else {riz_runtime_error("filter() arg must be callable");return riz_none();}
            if(riz_value_is_truthy(keep)) riz_list_append(result.as.list, riz_value_copy(item));
        }
        return result;
    }
    riz_runtime_error("list has no method '%s'", method);
    return riz_none();
}

RizValue dict_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    (void)I;
    RizDict* d = obj.as.dict;

    if (strcmp(method,"keys")==0 && argc==0)   return riz_dict_keys(d);
    if (strcmp(method,"values")==0 && argc==0) return riz_dict_values(d);
    if (strcmp(method,"has")==0 && argc==1 && args[0].type==VAL_STRING) return riz_bool(riz_dict_has(d,args[0].as.string));
    if (strcmp(method,"get")==0 && argc>=1 && args[0].type==VAL_STRING) {
        if(riz_dict_has(d,args[0].as.string)) return riz_dict_get(d,args[0].as.string);
        return argc>=2 ? riz_value_copy(args[1]) : riz_none();
    }
    if (strcmp(method,"set")==0 && argc==2 && args[0].type==VAL_STRING) {
        riz_dict_set(d,args[0].as.string,riz_value_copy(args[1])); return riz_none();
    }
    if (strcmp(method,"delete")==0 && argc==1 && args[0].type==VAL_STRING) {
        riz_dict_delete(d,args[0].as.string); return riz_none();
    }
    if (strcmp(method,"items")==0 && argc==0) {
        RizValue result = riz_list_new();
        for(int i=0;i<d->count;i++){
            RizValue pair=riz_list_new();
            riz_list_append(pair.as.list,riz_string(d->keys[i]));
            riz_list_append(pair.as.list,riz_value_copy(d->values[i]));
            riz_list_append(result.as.list,pair);
        }
        return result;
    }
    if (strcmp(method,"merge")==0 && argc==1 && args[0].type==VAL_DICT) {
        RizDict* other = args[0].as.dict;
        for(int i=0;i<other->count;i++) riz_dict_set(d,other->keys[i],riz_value_copy(other->values[i]));
        return riz_none();
    }
    /* Namespace dicts: callable values under string keys (e.g. py.exec on global `py`) */
    if (riz_dict_has(d, method)) {
        RizValue slot = riz_dict_get(d, method);
        if (slot.type == VAL_NATIVE_FN) {
            NativeFnObj* n = slot.as.native;
            if (n->arity >= 0 && argc != n->arity) {
                riz_runtime_error("%s() takes %d arg(s), %d given", n->name, n->arity, argc);
                return riz_none();
            }
            return n->fn(args, argc);
        }
        if (slot.type == VAL_FUNCTION)
            return call_function(I, slot.as.function, args, argc);
    }
    riz_runtime_error("dict has no method '%s'", method);
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Method call detection and dispatch
 * ═══════════════════════════════════════════════════════ */

static RizValue eval_method_call(Interpreter* I, ASTNode* node) {
    ASTNode* callee_node = node->as.call.callee;
    RizValue obj = eval(I, callee_node->as.member.object);
    const char* method = callee_node->as.member.member;

    int argc = node->as.call.arg_count;
    RizValue* args = NULL;
    if (argc > 0) {
        args = RIZ_ALLOC_ARRAY(RizValue, argc);
        for (int i = 0; i < argc; i++) args[i] = eval(I, node->as.call.args[i]);
    }

    RizValue result;
    if (obj.type == VAL_STRING)     result = string_method(I, obj, method, args, argc);
    else if (obj.type == VAL_LIST)  result = list_method(I, obj, method, args, argc);
    else if (obj.type == VAL_DICT)  result = dict_method(I, obj, method, args, argc);
    else if (obj.type == VAL_INSTANCE) {
        /* Instance method: look up method in struct definition */
        RizInstance* inst = obj.as.instance;
        RizStructDef* def = inst->def;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->method_names[i], method) == 0) {
                RizFunction* fn = def->method_values[i].as.function;
                /* Prepend self */
                int full_argc = argc + 1;
                RizValue* full_args = RIZ_ALLOC_ARRAY(RizValue, full_argc);
                full_args[0] = obj;
                for (int j = 0; j < argc; j++) full_args[j+1] = args[j];
                result = call_function(I, fn, full_args, full_argc);
                free(full_args);
                free(args);
                return result;
            }
        }
        riz_runtime_error("'%s' has no method '%s'", def->name, method);
        result = riz_none();
    } else if (obj.type == VAL_STRUCT_DEF) {
        /* Static method: StructName.method() */
        RizStructDef* def = obj.as.struct_def;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->method_names[i], method) == 0) {
                RizFunction* fn = def->method_values[i].as.function;
                result = call_function(I, fn, args, argc);
                free(args);
                return result;
            }
        }
        riz_runtime_error("'%s' has no static method '%s'", def->name, method);
        result = riz_none();
    }
    else { riz_runtime_error("'%s' has no method '%s'", riz_value_type_name(obj), method); result = riz_none(); }

    free(args);
    return result;
}

/* ═══════════════════════════════════════════════════════
 *  Register built-ins
 * ═══════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════
 *  Phase 5: FFI Plugin Loader
 * ═══════════════════════════════════════════════════════ */

/* FFI API callbacks — these are passed to plugins so they can register functions */
static void ffi_register_fn(void* interp_ptr, const char* name, RizPluginFn fn, int arity) {
    Interpreter* I = (Interpreter*)interp_ptr;
    env_define(I->globals, name, riz_native(name, (NativeFnPtr)fn, arity), false);
}

static void ffi_register_fn_vm(void* interp_ptr, const char* name, RizPluginFn fn, int arity) {
    RizVM* vm = (RizVM*)interp_ptr;
    env_define(vm->globals, name, riz_native(name, (NativeFnPtr)fn, arity), false);
}

static RizPluginValue ffi_make_dict(void) {
    return riz_dict_new();
}

static void ffi_dict_set_fn(RizPluginValue dict, const char* key, const char* riz_name, RizPluginFn fn, int arity) {
    if (dict.type != VAL_DICT) return;
    riz_dict_set(dict.as.dict, key, riz_native(riz_name, (NativeFnPtr)fn, arity));
}

static void ffi_define_global(void* interp_ptr, const char* name, RizPluginValue value) {
    Interpreter* I = (Interpreter*)interp_ptr;
    env_define(I->globals, name, value, false);
}

static void ffi_define_global_vm(void* interp_ptr, const char* name, RizPluginValue value) {
    RizVM* vm = (RizVM*)interp_ptr;
    env_define(vm->globals, name, value, false);
}

static RizPluginValue ffi_make_int(int64_t v)      { return riz_int(v); }
static RizPluginValue ffi_make_float(double v)     { return riz_float(v); }
static RizPluginValue ffi_make_bool(bool v)        { return riz_bool(v); }
static RizPluginValue ffi_make_string(const char*v){ return riz_string(v); }
static RizPluginValue ffi_make_none(void)          { return riz_none(); }
static RizPluginValue ffi_make_list(void)          { return riz_list_new(); }
static RizPluginValue ffi_make_native_ptr(void* p, const char* tag, void(*dtor)(void*)) {
    return riz_native_ptr(p, tag, dtor);
}
static void* ffi_get_native_ptr(RizPluginValue v) {
    if (v.type == VAL_NATIVE_PTR && v.as.native_ptr) return v.as.native_ptr->ptr;
    return NULL;
}
static void ffi_list_append(RizPluginValue lst, RizPluginValue v) { if(lst.type == VAL_LIST) riz_list_append(lst.as.list, v); }
static int ffi_list_len(RizPluginValue v) { return v.type == VAL_LIST ? v.as.list->count : 0; }
static RizPluginValue ffi_list_get(RizPluginValue v, int index) {
    if (v.type != VAL_LIST || index < 0 || index >= v.as.list->count) return riz_none();
    return v.as.list->items[index];
}

static int ffi_get_current_line(void* interp) {
    Interpreter* I = (Interpreter*)interp;
    return I->current_line;
}

static int ffi_get_current_line_vm(void* interp) {
    (void)interp;
    return 0;
}

static void ffi_panic(void* interp, const char* msg) {
    Interpreter* I = (Interpreter*)interp;
    fprintf(stderr, "\n\033[1;31m[Riz AI Panic]\033[0m %s\n", msg);
    fprintf(stderr, "  --> interpreter mode, line: %d\n", I->current_line);
    I->had_error = true;
    /* Optional: We could cleanly longjmp out, but for plugin panics mimicking AOT 
       we can just exit for now or let the runtime error system handle it. */
    exit(1);
}

static void ffi_panic_vm(void* interp, const char* msg) {
    RizVM* vm = (RizVM*)interp;
    fprintf(stderr, "\n\033[1;31m[Riz VM plugin panic]\033[0m %s\n", msg);
    vm->had_error = true;
    exit(1);
}

bool riz_plugin_load_vm(Environment* env, RizVM* vm, const char* path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s' (error %lu)", path, GetLastError());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)GetProcAddress(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        FreeLibrary(lib);
        return false;
    }
#else
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s': %s", path, dlerror());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)dlsym(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        dlclose(lib);
        return false;
    }
#endif
    RizPluginAPI api = {0};
    api.register_fn = ffi_register_fn_vm;
    api.make_int = ffi_make_int;
    api.make_float = ffi_make_float;
    api.make_bool = ffi_make_bool;
    api.make_string = ffi_make_string;
    api.make_none = ffi_make_none;
    api.make_list = ffi_make_list;
    api.make_native_ptr = ffi_make_native_ptr;
    api.get_native_ptr = ffi_get_native_ptr;
    api.list_append = ffi_list_append;
    api.list_length = ffi_list_len;
    api.list_get = ffi_list_get;
    api.interp = vm;
    api.get_current_line = ffi_get_current_line_vm;
    api.panic = ffi_panic_vm;
    api.make_dict = ffi_make_dict;
    api.dict_set_fn = ffi_dict_set_fn;
    api.define_global = ffi_define_global_vm;
    (void)env;
    init_fn(&api);

    void** np = (void**)realloc(vm->native_libs, sizeof(void*) * (size_t)(vm->native_lib_count + 1));
    if (!np) {
        riz_runtime_error("Out of memory tracking native library");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return false;
    }
    vm->native_libs = np;
    vm->native_libs[vm->native_lib_count++] = lib;
    return true;
}

static bool load_native_plugin(Interpreter* I, const char* path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s' (error %lu)", path, GetLastError());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)GetProcAddress(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        FreeLibrary(lib);
        return false;
    }
#else
    /* POSIX: dlopen/dlsym */
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s': %s", path, dlerror());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)dlsym(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        dlclose(lib);
        return false;
    }
#endif
    /* Build the API bridge and call the plugin's init */
    RizPluginAPI api = {0};
    api.register_fn  = ffi_register_fn;
    api.make_int        = ffi_make_int;
    api.make_float      = ffi_make_float;
    api.make_bool       = ffi_make_bool;
    api.make_string     = ffi_make_string;
    api.make_none       = ffi_make_none;
    api.make_list       = ffi_make_list;
    api.make_native_ptr = ffi_make_native_ptr;
    api.get_native_ptr  = ffi_get_native_ptr;
    api.list_append     = ffi_list_append;
    api.list_length     = ffi_list_len;
    api.list_get        = ffi_list_get;
    api.interp          = I;
    api.get_current_line = ffi_get_current_line;
    api.panic           = ffi_panic;
    api.make_dict       = ffi_make_dict;
    api.dict_set_fn     = ffi_dict_set_fn;
    api.define_global   = ffi_define_global;
    init_fn(&api);

    /* Track the handle so we can free it later */
    I->loaded_libs = realloc(I->loaded_libs, sizeof(void*) * (I->lib_count + 1));
    I->loaded_libs[I->lib_count++] = lib;
    return true;
}

void riz_vm_seed_builtins(Environment* g) {
    /* Phase 1 */
    env_define(g, "print",    riz_native("print",    native_print,      -1), false);
    env_define(g, "len",      riz_native("len",      native_len,         1), false);
    env_define(g, "range",    riz_native("range",    native_range,      -1), false);
    env_define(g, "type",     riz_native("type",     native_type,        1), false);
    env_define(g, "str",      riz_native("str",      native_str,         1), false);
    env_define(g, "int",      riz_native("int",      native_int_cast,    1), false);
    env_define(g, "float",    riz_native("float",    native_float_cast,  1), false);
    env_define(g, "input",    riz_native("input",    native_input,      -1), false);
    env_define(g, "append",   riz_native("append",   native_append,      2), false);
    env_define(g, "pop",      riz_native("pop",      native_pop,         1), false);
    env_define(g, "abs",      riz_native("abs",      native_abs,         1), false);
    env_define(g, "min",      riz_native("min",      native_min,        -1), false);
    env_define(g, "max",      riz_native("max",      native_max,        -1), false);
    env_define(g, "sum",      riz_native("sum",      native_sum,         1), false);
    env_define(g, "parallel_sum", riz_native("parallel_sum", native_parallel_sum, -1), false);
    env_define(g, "cpu_count", riz_native("cpu_count", native_cpu_count, 0), false);
    env_define(g, "map",      riz_native("map",      native_map,         2), false);
    env_define(g, "filter",   riz_native("filter",   native_filter,      2), false);
    /* Phase 2 */
    env_define(g, "format",   riz_native("format",   native_format,     -1), false);
    env_define(g, "sorted",   riz_native("sorted",   native_sorted,      1), false);
    env_define(g, "reversed", riz_native("reversed", native_reversed,    1), false);
    env_define(g, "enumerate",riz_native("enumerate",native_enumerate,   1), false);
    env_define(g, "zip",      riz_native("zip",      native_zip,         2), false);
    env_define(g, "keys",     riz_native("keys",     native_keys,        1), false);
    env_define(g, "values",   riz_native("values",   native_values,      1), false);
    env_define(g, "assert",   riz_native("assert",   native_assert,     -1), false);
    env_define(g, "exit",     riz_native("exit",     native_exit,       -1), false);
    env_define(g, "read_file",riz_native("read_file", native_read_file,   1), false);
    env_define(g, "write_file",riz_native("write_file", native_write_file, 2), false);
    env_define(g, "has_key",  riz_native("has_key",  native_has_key,     2), false);
    env_define(g, "clamp",    riz_native("clamp",    native_clamp,       3), false);
    env_define(g, "sign",     riz_native("sign",     native_sign,        1), false);
    env_define(g, "floor",    riz_native("floor",    native_floor_fn,    1), false);
    env_define(g, "ceil",     riz_native("ceil",     native_ceil_fn,     1), false);
    env_define(g, "round",    riz_native("round",    native_round_fn,    1), false);
    env_define(g, "all",      riz_native("all",      native_all,         1), false);
    env_define(g, "any",      riz_native("any",      native_any,         1), false);
    env_define(g, "bool",     riz_native("bool",     native_as_bool,     1), false);
    env_define(g, "ord",      riz_native("ord",      native_ord,         1), false);
    env_define(g, "chr",      riz_native("chr",      native_chr,         1), false);
    env_define(g, "extend",   riz_native("extend",   native_extend,      2), false);
    env_define(g, "debug",    riz_native("debug",    native_debug,      -1), false);
    env_define(g, "panic",    riz_native("panic",    native_panic,      -1), false);
    env_define(g, "time",     riz_native("time",     native_time_fn,     0), false);
}

static void register_builtins(Interpreter* I) {
    riz_vm_seed_builtins(I->globals);
}

/* ═══════════════════════════════════════════════════════
 *  Interpreter lifecycle
 * ═══════════════════════════════════════════════════════ */

Interpreter* interpreter_new(void) {
    Interpreter* I = RIZ_ALLOC(Interpreter);
    I->globals = env_new(NULL);
    I->current_env = I->globals;
    I->signal = SIG_NONE;
    I->had_error = false;
    I->imported_files = NULL;
    I->import_count = 0;
    I->loaded_libs = NULL;
    I->lib_count = 0;
    I->program_ast = NULL;
    register_builtins(I);
    return I;
}

void interpreter_free(Interpreter* interp) {
    if (!interp) return;
    interpreter_clear_error_stack(interp);
    for (int i = 0; i < interp->call_stack_len; i++) free(interp->call_stack[i]);
    free(interp->call_stack);
    interp->call_stack = NULL;
    interp->call_stack_len = 0;
    interp->call_stack_cap = 0;
    for (int i = 0; i < interp->import_count; i++) free(interp->imported_files[i]);
    free(interp->imported_files);
    env_free_deep(interp->globals);
    interp->globals = NULL;
    /* Unload native plugins after releasing globals (native fn wrappers). */
    for (int i = 0; i < interp->lib_count; i++) {
#ifdef _WIN32
        FreeLibrary((HMODULE)interp->loaded_libs[i]);
#else
        dlclose(interp->loaded_libs[i]);
#endif
    }
    free(interp->loaded_libs);
    if (interp->program_ast) {
        ast_free(interp->program_ast);
        interp->program_ast = NULL;
    }
    free(interp);
}

/* ═══════════════════════════════════════════════════════
 *  Call a function value
 * ═══════════════════════════════════════════════════════ */

static void call_stack_push(Interpreter* I, const char* name) {
    if (I->call_stack_len >= I->call_stack_cap) {
        int n = I->call_stack_cap ? I->call_stack_cap * 2 : 8;
        I->call_stack = (char**)realloc(I->call_stack, sizeof(char*) * (size_t)n);
        I->call_stack_cap = n;
    }
    I->call_stack[I->call_stack_len++] = riz_strdup(name ? name : "fn");
}

static void call_stack_pop(Interpreter* I) {
    if (I->call_stack_len <= 0) return;
    I->call_stack_len--;
    free(I->call_stack[I->call_stack_len]);
    I->call_stack[I->call_stack_len] = NULL;
}

static void interpreter_clear_error_stack(Interpreter* I) {
    if (!I) return;
    for (int i = 0; i < I->error_stack_len; i++) free(I->error_stack[i]);
    free(I->error_stack);
    I->error_stack = NULL;
    I->error_stack_len = 0;
}

static void interpreter_snapshot_error_stack(Interpreter* I) {
    interpreter_clear_error_stack(I);
    if (I->call_stack_len <= 0) return;
    I->error_stack_len = I->call_stack_len;
    I->error_stack = (char**)malloc(sizeof(char*) * (size_t)I->error_stack_len);
    for (int i = 0; i < I->error_stack_len; i++)
        I->error_stack[i] = riz_strdup(I->call_stack[i]);
}

static RizValue call_function(Interpreter* I, RizFunction* fn, RizValue* args, int argc) {
    /* Handle default parameters */
    int required = fn->param_defaults ? fn->required_count : fn->param_count;
    if (argc < required || argc > fn->param_count) {
        riz_runtime_error("Function '%s' expects %d-%d args, got %d",
                          fn->name ? fn->name : "anonymous", required, fn->param_count, argc);
        return riz_none();
    }
    Environment* call_env = env_new(fn->closure);
    for (int i = 0; i < argc; i++) env_define(call_env, fn->params[i], args[i], true);
    /* Fill in defaults for missing args */
    for (int i = argc; i < fn->param_count; i++) {
        if (fn->param_defaults && fn->param_defaults[i]) {
            RizValue defval = eval(I, fn->param_defaults[i]);
            env_define(call_env, fn->params[i], defval, true);
        } else {
            env_define(call_env, fn->params[i], riz_none(), true);
        }
    }
    Environment* saved = I->current_env; I->current_env = call_env;
    call_stack_push(I, fn->name);
    exec_block(I, fn->body);
    RizValue result = riz_none();
    if (I->signal == SIG_RETURN) { result = I->signal_value; I->signal = SIG_NONE; }
    call_stack_pop(I);
    I->current_env = saved;
    return result;
}

/* map/filter native functions (need call_function) */
RizValue native_map(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("map(list,fn) expected"); return riz_none(); }
    RizList*src=a[0].as.list; RizValue fn_val=a[1]; RizValue result=riz_list_new();
    for(int i=0;i<src->count;i++){RizValue item=src->items[i];RizValue mapped;
        if(fn_val.type==VAL_FUNCTION)mapped=call_function(g_interp,fn_val.as.function,&item,1);
        else if(fn_val.type==VAL_NATIVE_FN)mapped=fn_val.as.native->fn(&item,1);
        else{riz_runtime_error("map() 2nd arg must be callable");return riz_none();}
        riz_list_append(result.as.list,mapped);}
    return result;
}
RizValue native_filter(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("filter(list,fn) expected"); return riz_none(); }
    RizList*src=a[0].as.list; RizValue fn_val=a[1]; RizValue result=riz_list_new();
    for(int i=0;i<src->count;i++){RizValue item=src->items[i];RizValue keep;
        if(fn_val.type==VAL_FUNCTION)keep=call_function(g_interp,fn_val.as.function,&item,1);
        else if(fn_val.type==VAL_NATIVE_FN)keep=fn_val.as.native->fn(&item,1);
        else{riz_runtime_error("filter() 2nd arg must be callable");return riz_none();}
        if(riz_value_is_truthy(keep))riz_list_append(result.as.list,riz_value_copy(item));}
    return result;
}

/* ═══════════════════════════════════════════════════════
 *  Import — load and execute another .riz file
 * ═══════════════════════════════════════════════════════ */

static void run_import(Interpreter* I, const char* path) {
    char resolved[1024];
    const char* use_path = path;
    if (riz_import_resolve(resolved, sizeof(resolved), path))
        use_path = resolved;

    /* Check if already imported */
    for (int i = 0; i < I->import_count; i++)
        if (strcmp(I->imported_files[i], use_path) == 0) return;

    /* Track */
    I->imported_files = (char**)realloc(I->imported_files, sizeof(char*) * (I->import_count + 1));
    I->imported_files[I->import_count++] = riz_strdup(use_path);

    /* Read file */
    FILE* f = fopen(use_path, "rb");
    if (!f) { riz_runtime_error("Cannot import '%s': file not found", path); return; }
    fseek(f, 0, SEEK_END); long length = ftell(f); fseek(f, 0, SEEK_SET);
    char* source = (char*)malloc(length + 1);
    size_t bytes_read = fread(source, 1, length, f); source[bytes_read] = '\0'; fclose(f);

    /* Parse and execute */
    Lexer lexer; lexer_init(&lexer, source);
    Parser parser; parser_init(&parser, &lexer);
    ASTNode* program = parser_parse(&parser);
    if (!parser.had_error) eval(I, program);
    free(source);
}

/* ═══════════════════════════════════════════════════════
 *  Expression Evaluation
 * ═══════════════════════════════════════════════════════ */

static RizValue eval_binary(Interpreter* I, ASTNode* node) {
    TokenType op = node->as.binary.op;

    /* Short-circuit logical */
    if (op == TOK_AND) { RizValue l=eval(I,node->as.binary.left); return riz_bool(!riz_value_is_truthy(l)?false:riz_value_is_truthy(eval(I,node->as.binary.right))); }
    if (op == TOK_OR)  { RizValue l=eval(I,node->as.binary.left); return riz_bool(riz_value_is_truthy(l)?true:riz_value_is_truthy(eval(I,node->as.binary.right))); }

    RizValue left = eval(I, node->as.binary.left);
    RizValue right = eval(I, node->as.binary.right);

    /* Tensor / Native Operator Intercept */
    if (left.type == VAL_NATIVE_PTR || right.type == VAL_NATIVE_PTR) {
        if ((left.type != VAL_NATIVE_PTR || strcmp(left.as.native_ptr->type_tag, "Tensor") == 0) &&
            (right.type != VAL_NATIVE_PTR || strcmp(right.as.native_ptr->type_tag, "Tensor") == 0)) {
            const char* op_fn = NULL;
            if (op == TOK_PLUS)  op_fn = "tensor_add";
            if (op == TOK_MINUS) op_fn = "tensor_sub";
            if (op == TOK_STAR)  op_fn = "tensor_matmul";
            
            if (op_fn) {
                RizValue ffi_fn;
                if (env_get(I->globals, op_fn, &ffi_fn)) {
                    if (ffi_fn.type == VAL_NATIVE_FN) {
                        RizValue args[2] = {left, right};
                        return ffi_fn.as.native->fn(args, 2);
                    }
                }
            }
        }
    }

    /* String concatenation */
    if (op==TOK_PLUS && left.type==VAL_STRING && right.type==VAL_STRING) {
        size_t la=strlen(left.as.string),lb=strlen(right.as.string);
        char*r=(char*)malloc(la+lb+1);memcpy(r,left.as.string,la);memcpy(r+la,right.as.string,lb+1);
        return riz_string_take(r);
    }
    /* String + other → auto-concat */
    if (op==TOK_PLUS && (left.type==VAL_STRING||right.type==VAL_STRING)) {
        char*ls=riz_value_to_string(left);char*rs=riz_value_to_string(right);
        size_t la=strlen(ls),lb=strlen(rs);char*r=(char*)malloc(la+lb+1);
        memcpy(r,ls,la);memcpy(r+la,rs,lb+1);free(ls);free(rs);return riz_string_take(r);
    }
    /* String repetition */
    if (op==TOK_STAR && left.type==VAL_STRING && right.type==VAL_INT) {
        int64_t n=right.as.integer; if(n<=0)return riz_string("");
        size_t l=strlen(left.as.string);char*r=(char*)malloc(l*n+1);
        for(int64_t i=0;i<n;i++)memcpy(r+i*l,left.as.string,l);r[l*n]='\0';return riz_string_take(r);
    }
    /* List concatenation */
    if (op==TOK_PLUS && left.type==VAL_LIST && right.type==VAL_LIST) {
        RizValue result = riz_list_new();
        for(int i=0;i<left.as.list->count;i++) riz_list_append(result.as.list, riz_value_copy(left.as.list->items[i]));
        for(int i=0;i<right.as.list->count;i++) riz_list_append(result.as.list, riz_value_copy(right.as.list->items[i]));
        return result;
    }
    /* Dict merge: d1 + d2 */
    if (op==TOK_PLUS && left.type==VAL_DICT && right.type==VAL_DICT) {
        RizValue result = riz_dict_new();
        RizDict* l=left.as.dict; RizDict* r=right.as.dict;
        for(int i=0;i<l->count;i++) riz_dict_set(result.as.dict, l->keys[i], riz_value_copy(l->values[i]));
        for(int i=0;i<r->count;i++) riz_dict_set(result.as.dict, r->keys[i], riz_value_copy(r->values[i]));
        return result;
    }

    /* Equality */
    if (op==TOK_EQ)  return riz_bool(riz_value_equal(left, right));
    if (op==TOK_NEQ) return riz_bool(!riz_value_equal(left, right));

    /* Phase 3: 'in' operator — membership test */
    if (op==TOK_IN) {
        if (right.type==VAL_LIST) {
            for(int i=0;i<right.as.list->count;i++) if(riz_value_equal(left,right.as.list->items[i])) return riz_bool(true);
            return riz_bool(false);
        }
        if (right.type==VAL_DICT && left.type==VAL_STRING) return riz_bool(riz_dict_has(right.as.dict,left.as.string));
        if (right.type==VAL_STRING && left.type==VAL_STRING) return riz_bool(strstr(right.as.string,left.as.string)!=NULL);
        riz_runtime_error("'in' not supported for %s",riz_value_type_name(right)); return riz_bool(false);
    }

    /* Arithmetic */
    bool use_float = (left.type==VAL_FLOAT||right.type==VAL_FLOAT);
    double lf,rf; int64_t li,ri;
    if (use_float) { lf=left.type==VAL_INT?(double)left.as.integer:left.as.floating; rf=right.type==VAL_INT?(double)right.as.integer:right.as.floating; }
    else if (left.type==VAL_INT&&right.type==VAL_INT) { li=left.as.integer; ri=right.as.integer; }
    else { riz_runtime_error("Unsupported operand types: %s and %s",riz_value_type_name(left),riz_value_type_name(right)); I->had_error=true; return riz_none(); }

    switch (op) {
        case TOK_PLUS:  return use_float?riz_float(lf+rf):riz_int(li+ri);
        case TOK_MINUS: return use_float?riz_float(lf-rf):riz_int(li-ri);
        case TOK_STAR:  return use_float?riz_float(lf*rf):riz_int(li*ri);
        case TOK_SLASH:
            if(use_float){if(rf==0){riz_runtime_error("Division by zero");return riz_none();}return riz_float(lf/rf);}
            else{if(ri==0){riz_runtime_error("Division by zero");return riz_none();}return riz_float((double)li/(double)ri);}
        case TOK_FLOOR_DIV:
            if(use_float){if(rf==0){riz_runtime_error("Division by zero");return riz_none();}return riz_int((int64_t)floor(lf/rf));}
            else{if(ri==0){riz_runtime_error("Division by zero");return riz_none();}return riz_int(li/ri);}
        case TOK_PERCENT:
            if(use_float){if(rf==0){riz_runtime_error("Modulo by zero");return riz_none();}return riz_float(fmod(lf,rf));}
            else{if(ri==0){riz_runtime_error("Modulo by zero");return riz_none();}return riz_int(li%ri);}
        case TOK_POWER:
            if(use_float)return riz_float(pow(lf,rf));
            else{if(ri<0)return riz_float(pow((double)li,(double)ri));
                int64_t r=1,b=li,e=ri;while(e>0){if(e&1)r*=b;b*=b;e>>=1;}return riz_int(r);}
        case TOK_LT:  return riz_bool(use_float?lf<rf:li<ri);
        case TOK_GT:  return riz_bool(use_float?lf>rf:li>ri);
        case TOK_LTE: return riz_bool(use_float?lf<=rf:li<=ri);
        case TOK_GTE: return riz_bool(use_float?lf>=rf:li>=ri);
        default: riz_runtime_error("Unknown binary operator"); return riz_none();
    }
}

static RizValue eval_unary(Interpreter* I, ASTNode* node) {
    RizValue v = eval(I, node->as.unary.operand);
    switch (node->as.unary.op) {
        case TOK_MINUS:
            if(v.type==VAL_INT) return riz_int(-v.as.integer);
            if(v.type==VAL_FLOAT) return riz_float(-v.as.floating);
            riz_runtime_error("Cannot negate %s",riz_value_type_name(v)); return riz_none();
        case TOK_NOT: case TOK_BANG: return riz_bool(!riz_value_is_truthy(v));
        default: return riz_none();
    }
}

static RizValue eval_call(Interpreter* I, ASTNode* node) {
    /* Method call: obj.method(args) */
    ASTNode* callee_node = node->as.call.callee;
    if (callee_node->type == NODE_MEMBER) {
        /* Check if it's a property (not method) by trying method dispatch first */
        return eval_method_call(I, node);
    }

    RizValue callee = eval(I, callee_node);
    int argc = node->as.call.arg_count;
    RizValue* args = NULL;
    if (argc > 0) { args = RIZ_ALLOC_ARRAY(RizValue, argc); for(int i=0;i<argc;i++) args[i]=eval(I,node->as.call.args[i]); }

    RizValue result;
    if (callee.type == VAL_NATIVE_FN) {
        NativeFnObj* n=callee.as.native;
        if(n->arity>=0&&argc!=n->arity){riz_runtime_error("%s() takes %d arg(s), %d given",n->name,n->arity,argc);result=riz_none();}
        else result=n->fn(args,argc);
    } else if (callee.type == VAL_FUNCTION) {
        result = call_function(I, callee.as.function, args, argc);
    } else if (callee.type == VAL_STRUCT_DEF) {
        /* Struct constructor: StructName(field1, field2, ...) */
        RizStructDef* def = callee.as.struct_def;
        if (argc != def->field_count) {
            riz_runtime_error("Struct '%s' expects %d fields, got %d", def->name, def->field_count, argc);
            result = riz_none();
        } else {
            result = riz_instance_new(def, args);
        }
    } else { riz_runtime_error("Cannot call %s",riz_value_type_name(callee)); result=riz_none(); }

    free(args);
    return result;
}

static RizValue eval_pipe(Interpreter* I, ASTNode* node) {
    RizValue left = eval(I, node->as.pipe.left);
    ASTNode* right = node->as.pipe.right;

    if (right->type == NODE_CALL) {
        RizValue callee = eval(I, right->as.call.callee);
        int oc = right->as.call.arg_count, ac=oc+1;
        RizValue* args = RIZ_ALLOC_ARRAY(RizValue, ac);
        args[0]=left; for(int i=0;i<oc;i++) args[i+1]=eval(I,right->as.call.args[i]);
        RizValue result;
        if(callee.type==VAL_NATIVE_FN) result=callee.as.native->fn(args,ac);
        else if(callee.type==VAL_FUNCTION) result=call_function(I,callee.as.function,args,ac);
        else{riz_runtime_error("Pipe target must be callable");result=riz_none();}
        free(args); return result;
    }
    if (right->type == NODE_IDENTIFIER) {
        RizValue callee = eval(I, right);
        if(callee.type==VAL_NATIVE_FN) return callee.as.native->fn(&left,1);
        if(callee.type==VAL_FUNCTION)  return call_function(I,callee.as.function,&left,1);
        riz_runtime_error("Pipe target must be callable"); return riz_none();
    }
    riz_runtime_error("Invalid pipe target"); return riz_none();
}

/* ═══ Match expression ═══ */

static RizValue eval_match(Interpreter* I, ASTNode* node) {
    RizValue subject = eval(I, node->as.match_expr.subject);

    for (int i = 0; i < node->as.match_expr.arm_count; i++) {
        RizMatchArm* arm = &node->as.match_expr.arms[i];
        ASTNode* pattern = arm->pattern;
        bool matched = false;

        /* Wildcard: _ */
        if (pattern->type == NODE_IDENTIFIER && strcmp(pattern->as.identifier.name, "_") == 0) {
            matched = true;
        }
        /* Identifier binding (not _): bind the value and always match */
        else if (pattern->type == NODE_IDENTIFIER) {
            /* Create scope with binding */
            Environment* match_env = env_new(I->current_env);
            env_define(match_env, pattern->as.identifier.name, riz_value_copy(subject), false);
            Environment* saved = I->current_env;
            I->current_env = match_env;

            /* Check guard */
            if (arm->guard) {
                RizValue gv = eval(I, arm->guard);
                if (!riz_value_is_truthy(gv)) { I->current_env = saved; continue; }
            }

            /* Execute body */
            RizValue result;
            if (arm->body->type == NODE_BLOCK) { exec_block(I, arm->body); result = riz_none(); }
            else result = eval(I, arm->body);

            I->current_env = saved;
            return result;
        }
        /* Literal pattern: compare with subject */
        else {
            RizValue pv = eval(I, pattern);
            matched = riz_value_equal(subject, pv);
        }

        if (matched) {
            /* Check guard */
            if (arm->guard) {
                RizValue gv = eval(I, arm->guard);
                if (!riz_value_is_truthy(gv)) continue;
            }
            /* Execute body */
            if (arm->body->type == NODE_BLOCK) { exec_block(I, arm->body); return riz_none(); }
            return eval(I, arm->body);
        }
    }
    /* No arm matched */
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Main eval dispatch
 * ═══════════════════════════════════════════════════════ */

static RizValue eval(Interpreter* I, ASTNode* node) {
    if (!node) return riz_none();
    I->current_line = node->line;
    switch (node->type) {
        /* Literals */
        case NODE_INT_LIT:    return riz_int(node->as.int_lit.value);
        case NODE_FLOAT_LIT:  return riz_float(node->as.float_lit.value);
        case NODE_STRING_LIT: return riz_string(node->as.string_lit.value);
        case NODE_BOOL_LIT:   return riz_bool(node->as.bool_lit.value);
        case NODE_NONE_LIT:   return riz_none();

        case NODE_LIST_LIT: {
            RizValue list = riz_list_new();
            for(int i=0;i<node->as.list_lit.count;i++) riz_list_append(list.as.list, eval(I,node->as.list_lit.items[i]));
            return list;
        }
        case NODE_DICT_LIT: {
            RizValue dict = riz_dict_new();
            for(int i=0;i<node->as.dict_lit.count;i++){
                RizValue key = eval(I, node->as.dict_lit.keys[i]);
                RizValue val = eval(I, node->as.dict_lit.values[i]);
                char* key_str;
                if (key.type==VAL_STRING) key_str=riz_strdup(key.as.string);
                else key_str=riz_value_to_string(key);
                riz_dict_set(dict.as.dict, key_str, val);
                free(key_str);
            }
            return dict;
        }
        case NODE_IDENTIFIER: {
            RizValue val;
            if(!env_get(I->current_env, node->as.identifier.name, &val)){
                riz_runtime_error("Undefined variable '%s'",node->as.identifier.name); I->had_error=true; return riz_none();
            }
            return val;
        }
        case NODE_UNARY:      return eval_unary(I, node);
        case NODE_BINARY:     return eval_binary(I, node);
        case NODE_CALL:       return eval_call(I, node);
        case NODE_PIPE:       return eval_pipe(I, node);
        case NODE_MATCH_EXPR: return eval_match(I, node);

        /* Ternary: value if condition else other */
        case NODE_TERNARY: {
            RizValue cond = eval(I, node->as.ternary.condition);
            if (riz_value_is_truthy(cond)) return eval(I, node->as.ternary.true_expr);
            else return eval(I, node->as.ternary.false_expr);
        }

        /* List comprehension: [expr for var in iter if cond] */
        case NODE_LIST_COMP: {
            RizValue iterable = eval(I, node->as.list_comp.iterable);
            if (iterable.type != VAL_LIST) {
                riz_runtime_error("Cannot iterate over %s in list comprehension", riz_value_type_name(iterable));
                return riz_none();
            }
            RizValue result = riz_list_new();
            RizList* list = iterable.as.list;
            for (int i = 0; i < list->count; i++) {
                Environment* le = env_new(I->current_env);
                Environment* saved = I->current_env;
                I->current_env = le;
                env_define(le, node->as.list_comp.var_name, riz_value_copy(list->items[i]), false);
                bool include = true;
                if (node->as.list_comp.condition) {
                    RizValue cv = eval(I, node->as.list_comp.condition);
                    include = riz_value_is_truthy(cv);
                }
                if (include) {
                    RizValue v = eval(I, node->as.list_comp.expr);
                    riz_list_append(result.as.list, v);
                }
                I->current_env = saved;
            }
            return result;
        }

        /* Slice: obj[start:end:step] */
        case NODE_SLICE: {
            RizValue obj = eval(I, node->as.slice.object);
            if (obj.type == VAL_LIST) {
                int count = obj.as.list->count;
                int start = 0, end = count, step = 1;
                if (node->as.slice.start) { RizValue sv = eval(I, node->as.slice.start); if (sv.type==VAL_INT) { start=(int)sv.as.integer; if(start<0)start+=count; } }
                if (node->as.slice.end)   { RizValue ev = eval(I, node->as.slice.end);   if (ev.type==VAL_INT) { end=(int)ev.as.integer; if(end<0)end+=count; } }
                if (node->as.slice.step)  { RizValue tv = eval(I, node->as.slice.step);  if (tv.type==VAL_INT) step=(int)tv.as.integer; }
                if (step == 0) { riz_runtime_error("Slice step cannot be zero"); return riz_none(); }
                if (start < 0) start = 0; if (end > count) end = count;
                RizValue result = riz_list_new();
                if (step > 0) { for (int i = start; i < end; i += step) riz_list_append(result.as.list, riz_value_copy(obj.as.list->items[i])); }
                else { for (int i = (end > 0 ? end - 1 : count - 1); i >= start; i += step) riz_list_append(result.as.list, riz_value_copy(obj.as.list->items[i])); }
                return result;
            }
            if (obj.type == VAL_STRING) {
                int slen = (int)strlen(obj.as.string);
                int start = 0, end = slen, step = 1;
                if (node->as.slice.start) { RizValue sv = eval(I, node->as.slice.start); if (sv.type==VAL_INT) { start=(int)sv.as.integer; if(start<0)start+=slen; } }
                if (node->as.slice.end)   { RizValue ev = eval(I, node->as.slice.end);   if (ev.type==VAL_INT) { end=(int)ev.as.integer; if(end<0)end+=slen; } }
                if (node->as.slice.step)  { RizValue tv = eval(I, node->as.slice.step);  if (tv.type==VAL_INT) step=(int)tv.as.integer; }
                if (step == 0) { riz_runtime_error("Slice step cannot be zero"); return riz_none(); }
                if (start < 0) start = 0; if (end > slen) end = slen;
                size_t cap = (size_t)(end - start + 1); char* buf = (char*)malloc(cap + 1); int len = 0;
                if (step > 0) { for (int i = start; i < end; i += step) buf[len++] = obj.as.string[i]; }
                else { for (int i = (end > 0 ? end - 1 : slen - 1); i >= start; i += step) buf[len++] = obj.as.string[i]; }
                buf[len] = '\0';
                return riz_string_take(buf);
            }
            riz_runtime_error("Cannot slice %s", riz_value_type_name(obj));
            return riz_none();
        }

        case NODE_INDEX: {
            RizValue obj = eval(I, node->as.index_expr.object);
            RizValue idx = eval(I, node->as.index_expr.index);
            if (obj.type==VAL_LIST && idx.type==VAL_INT) {
                int64_t i=idx.as.integer; if(i<0)i+=obj.as.list->count;
                return riz_list_get(obj.as.list, (int)i);
            }
            if (obj.type==VAL_STRING && idx.type==VAL_INT) {
                int64_t i=idx.as.integer; size_t len=strlen(obj.as.string);
                if(i<0)i+=(int64_t)len;
                if(i<0||i>=(int64_t)len){riz_runtime_error("String index out of range");return riz_none();}
                char buf[2]={obj.as.string[i],'\0'}; return riz_string(buf);
            }
            /* Dict indexing: d["key"] */
            if (obj.type==VAL_DICT && idx.type==VAL_STRING) {
                return riz_dict_get(obj.as.dict, idx.as.string);
            }
            riz_runtime_error("Cannot index %s with %s",riz_value_type_name(obj),riz_value_type_name(idx));
            return riz_none();
        }

        case NODE_ASSIGN: {
            RizValue val = eval(I, node->as.assign.value);
            if(!env_set(I->current_env,node->as.assign.name,val)) I->had_error=true;
            return val;
        }

        case NODE_COMPOUND_ASSIGN: {
            RizValue current; if(!env_get(I->current_env,node->as.compound_assign.name,&current)){riz_runtime_error("Undefined variable '%s'",node->as.compound_assign.name);return riz_none();}
            RizValue rhs = eval(I, node->as.compound_assign.value);
            ASTNode synth; synth.type=NODE_BINARY; synth.line=node->line; synth.as.binary.op=node->as.compound_assign.op;
            ASTNode ln,rn;
            if(current.type==VAL_INT){ln.type=NODE_INT_LIT;ln.as.int_lit.value=current.as.integer;}
            else if(current.type==VAL_FLOAT){ln.type=NODE_FLOAT_LIT;ln.as.float_lit.value=current.as.floating;}
            else if(current.type==VAL_STRING){ln.type=NODE_STRING_LIT;ln.as.string_lit.value=current.as.string;}
            else{riz_runtime_error("Cannot use compound assignment on %s",riz_value_type_name(current));return riz_none();}
            if(rhs.type==VAL_INT){rn.type=NODE_INT_LIT;rn.as.int_lit.value=rhs.as.integer;}
            else if(rhs.type==VAL_FLOAT){rn.type=NODE_FLOAT_LIT;rn.as.float_lit.value=rhs.as.floating;}
            else if(rhs.type==VAL_STRING){rn.type=NODE_STRING_LIT;rn.as.string_lit.value=rhs.as.string;}
            else{riz_runtime_error("Cannot use compound assignment with %s",riz_value_type_name(rhs));return riz_none();}
            synth.as.binary.left=&ln;synth.as.binary.right=&rn;
            RizValue result=eval_binary(I,&synth);
            env_set(I->current_env,node->as.compound_assign.name,result);
            return result;
        }

        case NODE_MEMBER: {
            RizValue obj = eval(I, node->as.member.object);
            const char* m = node->as.member.member;
            /* Properties (non-callable) */
            if (obj.type==VAL_LIST && strcmp(m,"length")==0) return riz_int(obj.as.list->count);
            if (obj.type==VAL_STRING && strcmp(m,"length")==0) return riz_int((int64_t)strlen(obj.as.string));
            if (obj.type==VAL_DICT) {
                if (riz_dict_has(obj.as.dict, m)) return riz_dict_get(obj.as.dict, m);
                if (strcmp(m,"length")==0 || strcmp(m,"count")==0) return riz_int(obj.as.dict->count);
            }
            /* Instance field access */
            if (obj.type==VAL_INSTANCE) {
                RizInstance* inst = obj.as.instance;
                for (int i = 0; i < inst->def->field_count; i++) {
                    if (strcmp(inst->def->field_names[i], m) == 0)
                        return riz_value_copy(inst->fields[i]);
                }
                riz_runtime_error("'%s' has no field '%s'", inst->def->name, m);
                return riz_none();
            }
            riz_runtime_error("'%s' has no member '%s'",riz_value_type_name(obj),m);
            return riz_none();
        }

        case NODE_LAMBDA: {
            RizFunction* fn = RIZ_ALLOC(RizFunction);
            fn->name=riz_strdup("<lambda>"); fn->param_count=node->as.lambda.param_count;
            fn->params=RIZ_ALLOC_ARRAY(char*,fn->param_count);
            for(int i=0;i<fn->param_count;i++) fn->params[i]=riz_strdup(node->as.lambda.params[i]);
            fn->body=node->as.lambda.body; fn->closure=I->current_env;
            fn->param_defaults=NULL; fn->required_count=fn->param_count;
            return riz_fn(fn);
        }

        /* ─── Statements ─── */
        case NODE_EXPR_STMT: return eval(I, node->as.expr_stmt.expr);

        case NODE_LET_DECL: {
            RizValue val = eval(I, node->as.let_decl.initializer);
            env_define(I->current_env, node->as.let_decl.name, val, node->as.let_decl.is_mutable);
            return riz_none();
        }
        case NODE_FN_DECL: {
            RizFunction* fn = RIZ_ALLOC(RizFunction);
            fn->name=riz_strdup(node->as.fn_decl.name); fn->param_count=node->as.fn_decl.param_count;
            fn->params=RIZ_ALLOC_ARRAY(char*,fn->param_count);
            for(int i=0;i<fn->param_count;i++) fn->params[i]=riz_strdup(node->as.fn_decl.params[i]);
            fn->body=node->as.fn_decl.body; fn->closure=I->current_env;
            /* Default parameters (Phase 3) */
            fn->param_defaults = node->as.fn_decl.param_defaults; /* share with AST */
            if (fn->param_defaults) {
                fn->required_count = 0;
                for (int i = 0; i < fn->param_count; i++) {
                    if (!fn->param_defaults[i]) fn->required_count = i + 1;
                }
            } else {
                fn->required_count = fn->param_count;
            }
            env_define(I->current_env, fn->name, riz_fn(fn), false);
            return riz_none();
        }
        case NODE_RETURN_STMT: {
            RizValue val = node->as.return_stmt.value ? eval(I,node->as.return_stmt.value) : riz_none();
            I->signal=SIG_RETURN; I->signal_value=val; return val;
        }
        case NODE_IF_STMT: {
            RizValue cond = eval(I, node->as.if_stmt.condition);
            if (riz_value_is_truthy(cond)) exec_block(I, node->as.if_stmt.then_branch);
            else if (node->as.if_stmt.else_branch) {
                if(node->as.if_stmt.else_branch->type==NODE_IF_STMT) eval(I,node->as.if_stmt.else_branch);
                else exec_block(I, node->as.if_stmt.else_branch);
            }
            return riz_none();
        }
        case NODE_WHILE_STMT: {
            while(true) {
                RizValue c=eval(I,node->as.while_stmt.condition); if(!riz_value_is_truthy(c))break;
                exec_block(I,node->as.while_stmt.body);
                if(I->signal==SIG_BREAK){I->signal=SIG_NONE;break;}
                if(I->signal==SIG_CONTINUE){I->signal=SIG_NONE;continue;}
                if(I->signal==SIG_RETURN)break;
            }
            return riz_none();
        }
        case NODE_FOR_STMT: {
            RizValue iterable = eval(I, node->as.for_stmt.iterable);
            if(iterable.type!=VAL_LIST){riz_runtime_error("Cannot iterate over %s",riz_value_type_name(iterable));return riz_none();}
            RizList* list = iterable.as.list;
            bool did_break = false;
            for(int i=0;i<list->count;i++){
                Environment* le=env_new(I->current_env); Environment* saved=I->current_env; I->current_env=le;
                env_define(le,node->as.for_stmt.var_name,riz_value_copy(list->items[i]),false);
                if(node->as.for_stmt.body->type==NODE_BLOCK){
                    ASTNode* b=node->as.for_stmt.body;
                    for(int j=0;j<b->as.block.count;j++){eval(I,b->as.block.statements[j]);if(I->signal!=SIG_NONE)break;}
                } else eval(I,node->as.for_stmt.body);
                I->current_env=saved;
                if(I->signal==SIG_BREAK){I->signal=SIG_NONE;did_break=true;break;}
                if(I->signal==SIG_CONTINUE){I->signal=SIG_NONE;continue;}
                if(I->signal==SIG_RETURN)break;
            }
            /* for...else: else block runs if loop completed without break */
            if (!did_break && I->signal == SIG_NONE && node->as.for_stmt.else_branch) {
                exec_block(I, node->as.for_stmt.else_branch);
            }
            return riz_none();
        }
        case NODE_BREAK_STMT:    I->signal=SIG_BREAK; return riz_none();
        case NODE_CONTINUE_STMT: I->signal=SIG_CONTINUE; return riz_none();

        case NODE_IMPORT: run_import(I, node->as.import_stmt.path); return riz_none();
        case NODE_IMPORT_NATIVE: load_native_plugin(I, node->as.import_native.path); return riz_none();
        case NODE_BLOCK:  exec_block(I, node); return riz_none();

        /* Phase 3: struct declaration */
        case NODE_STRUCT_DECL: {
            /* Create field names array (owned by struct def) */
            char** fields = RIZ_ALLOC_ARRAY(char*, node->as.struct_decl.field_count);
            for (int i = 0; i < node->as.struct_decl.field_count; i++)
                fields[i] = riz_strdup(node->as.struct_decl.field_names[i]);
            RizValue def = riz_struct_def_new(node->as.struct_decl.name, fields, node->as.struct_decl.field_count);
            env_define(I->current_env, node->as.struct_decl.name, def, false);
            return riz_none();
        }

        /* Phase 3: impl block */
        case NODE_IMPL_DECL: {
            RizValue sv;
            if (!env_get(I->current_env, node->as.impl_decl.struct_name, &sv) || sv.type != VAL_STRUCT_DEF) {
                riz_runtime_error("'%s' is not a struct", node->as.impl_decl.struct_name);
                return riz_none();
            }
            RizStructDef* def = sv.as.struct_def;
            for (int i = 0; i < node->as.impl_decl.method_count; i++) {
                ASTNode* method = node->as.impl_decl.methods[i];
                RizFunction* fn = RIZ_ALLOC(RizFunction);
                fn->name = riz_strdup(method->as.fn_decl.name);
                fn->param_count = method->as.fn_decl.param_count;
                fn->params = RIZ_ALLOC_ARRAY(char*, fn->param_count);
                for (int j = 0; j < fn->param_count; j++)
                    fn->params[j] = riz_strdup(method->as.fn_decl.params[j]);
                fn->body = method->as.fn_decl.body;
                fn->closure = I->current_env;
                fn->param_defaults = method->as.fn_decl.param_defaults;
                if (fn->param_defaults) {
                    fn->required_count = 0;
                    for (int j = 0; j < fn->param_count; j++) {
                        if (!fn->param_defaults[j]) fn->required_count = j + 1;
                    }
                } else {
                    fn->required_count = fn->param_count;
                }
                riz_struct_add_method(def, fn->name, riz_fn(fn));
            }
            return riz_none();
        }

        /* Phase 4: trait declaration */
        case NODE_TRAIT_DECL: {
            RizTraitDef* def = RIZ_ALLOC(RizTraitDef);
            def->name = riz_strdup(node->as.trait_decl.name);
            def->method_count = node->as.trait_decl.method_count;
            def->method_names = RIZ_ALLOC_ARRAY(char*, def->method_count);
            def->method_arity = RIZ_ALLOC_ARRAY(int, def->method_count);
            for (int i = 0; i < def->method_count; i++) {
                ASTNode* method = node->as.trait_decl.methods[i];
                def->method_names[i] = riz_strdup(method->as.fn_decl.name);
                def->method_arity[i] = method->as.fn_decl.param_count;
            }
            RizValue val; val.type = VAL_TRAIT_DEF; val.as.trait_def = def;
            env_define(I->current_env, def->name, val, false);
            return riz_none();
        }

        /* Phase 4: impl Trait for Struct block */
        case NODE_IMPL_TRAIT_DECL: {
            RizValue trait_val, struct_val;
            if (!env_get(I->current_env, node->as.impl_trait_decl.trait_name, &trait_val) || trait_val.type != VAL_TRAIT_DEF) {
                riz_runtime_error("'%s' is not a trait", node->as.impl_trait_decl.trait_name);
                return riz_none();
            }
            if (!env_get(I->current_env, node->as.impl_trait_decl.struct_name, &struct_val) || struct_val.type != VAL_STRUCT_DEF) {
                riz_runtime_error("'%s' is not a struct", node->as.impl_trait_decl.struct_name);
                return riz_none();
            }
            RizTraitDef* trait_def = trait_val.as.trait_def;
            RizStructDef* struct_def = struct_val.as.struct_def;

            int provided_count = node->as.impl_trait_decl.method_count;
            ASTNode** methods = node->as.impl_trait_decl.methods;

            for (int i = 0; i < trait_def->method_count; i++) {
                const char* t_name = trait_def->method_names[i];
                int t_arity = trait_def->method_arity[i];
                bool found = false;
                for (int j = 0; j < provided_count; j++) {
                    if (strcmp(methods[j]->as.fn_decl.name, t_name) == 0) {
                        if (methods[j]->as.fn_decl.param_count != t_arity) {
                            riz_runtime_error("Method '%s' in impl for '%s' expects %d params but trait requires %d", 
                                              t_name, struct_def->name, methods[j]->as.fn_decl.param_count, t_arity);
                            return riz_none();
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    riz_runtime_error("Struct '%s' does not implement required trait method '%s'", struct_def->name, t_name);
                    return riz_none();
                }
            }

            for (int i = 0; i < provided_count; i++) {
                ASTNode* method = methods[i];
                RizFunction* fn = RIZ_ALLOC(RizFunction);
                fn->name = riz_strdup(method->as.fn_decl.name);
                fn->param_count = method->as.fn_decl.param_count;
                fn->params = RIZ_ALLOC_ARRAY(char*, fn->param_count);
                for (int j = 0; j < fn->param_count; j++) fn->params[j] = riz_strdup(method->as.fn_decl.params[j]);
                fn->body = method->as.fn_decl.body; fn->closure = I->current_env;
                fn->param_defaults = method->as.fn_decl.param_defaults;
                if (fn->param_defaults) {
                    fn->required_count = 0;
                    for (int j = 0; j < fn->param_count; j++) if (!fn->param_defaults[j]) fn->required_count = j + 1;
                } else { fn->required_count = fn->param_count; }
                riz_struct_add_method(struct_def, fn->name, riz_fn(fn));
            }
            return riz_none();
        }

        /* Phase 3: try/catch */
        case NODE_TRY_STMT: {
            exec_block(I, node->as.try_stmt.try_block);
            if (I->signal == SIG_THROW) {
                I->signal = SIG_NONE;
                RizValue error_val = I->signal_value;
                interpreter_clear_error_stack(I);
                Environment* catch_env = env_new(I->current_env);
                env_define(catch_env, node->as.try_stmt.catch_var, error_val, false);
                Environment* saved = I->current_env;
                I->current_env = catch_env;
                exec_block(I, node->as.try_stmt.catch_block);
                I->current_env = saved;
            }
            return riz_none();
        }

        /* Phase 3: throw */
        case NODE_THROW_STMT: {
            RizValue val = eval(I, node->as.throw_stmt.value);
            interpreter_snapshot_error_stack(I);
            I->signal = SIG_THROW;
            I->signal_value = val;
            return riz_none();
        }

        /* Phase 3: member assignment — obj.field = value */
        case NODE_MEMBER_ASSIGN: {
            RizValue obj = eval(I, node->as.member_assign.object);
            RizValue val = eval(I, node->as.member_assign.value);
            const char* member = node->as.member_assign.member;
            if (obj.type == VAL_INSTANCE) {
                RizInstance* inst = obj.as.instance;
                for (int i = 0; i < inst->def->field_count; i++) {
                    if (strcmp(inst->def->field_names[i], member) == 0) {
                        inst->fields[i] = val;
                        return val;
                    }
                }
                riz_runtime_error("'%s' has no field '%s'", inst->def->name, member);
            } else if (obj.type == VAL_DICT) {
                riz_dict_set(obj.as.dict, member, val);
                return val;
            } else {
                riz_runtime_error("Cannot assign to member of %s", riz_value_type_name(obj));
            }
            return riz_none();
        }

        /* Phase 3: index assignment — obj[idx] = value */
        case NODE_INDEX_ASSIGN: {
            RizValue obj = eval(I, node->as.index_assign.object);
            RizValue idx = eval(I, node->as.index_assign.index);
            RizValue val = eval(I, node->as.index_assign.value);
            if (obj.type == VAL_LIST && idx.type == VAL_INT) {
                int64_t i = idx.as.integer;
                if (i < 0) i += obj.as.list->count;
                if (i < 0 || i >= obj.as.list->count) { riz_runtime_error("Index out of range"); return riz_none(); }
                obj.as.list->items[i] = val;
                return val;
            }
            if (obj.type == VAL_DICT) {
                char* key = (idx.type==VAL_STRING) ? idx.as.string : riz_value_to_string(idx);
                riz_dict_set(obj.as.dict, key, val);
                if (idx.type != VAL_STRING) free(key);
                return val;
            }
            riz_runtime_error("Cannot assign to index of %s", riz_value_type_name(obj));
            return riz_none();
        }

        case NODE_PROGRAM: {
            for(int i=0;i<node->as.program.count;i++){eval(I,node->as.program.declarations[i]);if(I->signal!=SIG_NONE)break;}
            return riz_none();
        }
        default: riz_runtime_error("Unknown AST node type: %d",node->type); return riz_none();
    }
}

/* ═══ Block execution ═══ */

static void exec_block(Interpreter* I, ASTNode* block) {
    if(!block) return;
    if(block->type!=NODE_BLOCK){eval(I,block);return;}
    Environment* be=env_new(I->current_env); Environment* saved=I->current_env; I->current_env=be;
    for(int i=0;i<block->as.block.count;i++){
        eval(I,block->as.block.statements[i]);
        if(I->signal!=SIG_NONE) break;
    }
    I->current_env=saved;
}

/* ═══ Public API ═══ */

void interpreter_exec(Interpreter* interp, ASTNode* program) { g_interp=interp; eval(interp,program); }
RizValue interpreter_eval(Interpreter* interp, ASTNode* node) { g_interp=interp; return eval(interp,node); }

void interpreter_report_pending_signal(Interpreter* interp) {
    if (!interp || interp->signal != SIG_THROW) return;
    fprintf(stderr, COL_RED COL_BOLD "Uncaught exception:" COL_RESET " ");
    char* s = riz_value_to_string(interp->signal_value);
    fprintf(stderr, "%s\n", s);
    free(s);
    if (interp->error_stack_len > 0) {
        fprintf(stderr, COL_DIM "Call stack (innermost last):\n" COL_RESET);
        for (int i = 0; i < interp->error_stack_len; i++)
            fprintf(stderr, COL_DIM "  at %s\n" COL_RESET, interp->error_stack[i]);
    }
    interpreter_clear_error_stack(interp);
    interp->had_error = true;
    interp->signal = SIG_NONE;
}
