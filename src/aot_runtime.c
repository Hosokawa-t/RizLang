/*
 * Riz Programming Language
 * aot_runtime.c — Native AOT FFI Bridge implementation
 */

#include "aot_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ─── AOT Line Tracking & Diagnostics ──────────────── */

int aot_current_line = 0;
const char* aot_source_path = "";

static int aot_get_line(void* interp) {
    (void)interp;  /* unused in AOT */
    return aot_current_line;
}

void aot_panic(void* interp, const char* msg) {
    (void)interp;
    fprintf(stderr, "\n\033[1;31m[Riz AI Panic]\033[0m %s\n", msg);
    fprintf(stderr, "  --> %s:%d\n\n", aot_source_path ? aot_source_path : "?", aot_current_line);

    if (aot_source_path && strlen(aot_source_path) > 0) {
        FILE* f = fopen(aot_source_path, "r");
        if (f) {
            char line_buf[512];
            int current = 1;
            int start_print = aot_current_line > 2 ? aot_current_line - 2 : 1;
            int end_print = aot_current_line + 2;
            
            while (fgets(line_buf, sizeof(line_buf), f)) {
                if (current >= start_print && current <= end_print) {
                    if (current == aot_current_line) {
                        fprintf(stderr, "\033[1;31m%4d | %s\033[0m", current, line_buf);
                        fprintf(stderr, "     | \033[1;31m^^^^^^\033[0m\n");
                    } else {
                        fprintf(stderr, "\033[90m%4d | \033[0m%s", current, line_buf);
                    }
                }
                current++;
                if (current > end_print) break;
            }
            fclose(f);
        }
    }
    fprintf(stderr, "\n");
    exit(1);
}

/* ─── AOT Function Table ───────────────────────────── */

#define MAX_AOT_FNS 256
static NativeFnObj aot_fns[MAX_AOT_FNS];
static int aot_fn_count = 0;

/* ─── API provided to Plugins ──────────────────────── */

static void aot_register_fn(void* interp, const char* name, RizPluginFn fn, int arity) {
    if (aot_fn_count >= MAX_AOT_FNS) {
        fprintf(stderr, "[AOT-FFI] Function table overflow for '%s'\n", name);
        return;
    }
    aot_fns[aot_fn_count].name = strdup(name);
    aot_fns[aot_fn_count].fn = fn;
    aot_fns[aot_fn_count].arity = arity;
    aot_fn_count++;
}

void aot_register_user_fn(const char* name, NativeFnPtr fn, int arity) {
    aot_register_fn(NULL, name, (RizPluginFn)fn, arity);
}

static RizPluginValue _mk_int(int64_t v) { return riz_int(v); }
static RizPluginValue _mk_float(double v) { return riz_float(v); }
static RizPluginValue _mk_bool(bool v) { return riz_bool(v); }
static RizPluginValue _mk_string(const char* v) { return riz_string(v); }
static RizPluginValue _mk_none(void) { return riz_none(); }
static RizPluginValue _mk_list(void) { return riz_list_new(); }
static RizPluginValue _mk_ptr(void* ptr, const char* tag, void(*d)(void*)) { return riz_native_ptr(ptr, tag, d); }
static void* _get_ptr(RizPluginValue v) { return v.type == VAL_NATIVE_PTR ? v.as.native_ptr->ptr : NULL; }
static void _list_add(RizPluginValue lst, RizPluginValue v) { if(lst.type == VAL_LIST) riz_list_append(lst.as.list, v); }
static int _list_len(RizPluginValue v) { return v.type == VAL_LIST ? v.as.list->count : 0; }
static RizPluginValue _list_get(RizPluginValue v, int index) { 
    if (v.type != VAL_LIST || index < 0 || index >= v.as.list->count) return _mk_none();
    return v.as.list->items[index];
}

/* ─── FFI Bridge Loader ────────────────────────────── */

void aot_load_plugin(const char* lib_path) {
    void* handle = NULL;
#ifdef _WIN32
    handle = LoadLibraryA(lib_path);
#else
    handle = dlopen(lib_path, RTLD_NOW);
#endif
    if (!handle) {
        fprintf(stderr, "[AOT-FFI] Error: Cannot load '%s'\n", lib_path);
        exit(1);
    }

    RizPluginInitFn init_fn = NULL;
#ifdef _WIN32
    init_fn = (RizPluginInitFn)GetProcAddress((HMODULE)handle, "riz_plugin_init");
#else
    init_fn = (RizPluginInitFn)dlsym(handle, "riz_plugin_init");
#endif
    if (!init_fn) {
        fprintf(stderr, "[AOT-FFI] Error: 'riz_plugin_init' not found in '%s'\n", lib_path);
        exit(1);
    }

    RizPluginAPI api;
    api.register_fn = aot_register_fn;
    api.make_int = _mk_int;
    api.make_float = _mk_float;
    api.make_bool = _mk_bool;
    api.make_string = _mk_string;
    api.make_none = _mk_none;
    api.make_list = _mk_list;
    api.make_native_ptr = _mk_ptr;
    api.get_native_ptr = _get_ptr;
    api.list_append = _list_add;
    api.list_length = _list_len;
    api.list_get = _list_get;
    api.interp = NULL; /* Opaque dummy interp, not actually used by our robust AOT API */
    api.get_current_line = aot_get_line;
    api.panic = aot_panic;

    init_fn(&api);
}

/* ─── FFI Dispatcher ───────────────────────────────── */

/* ─── Builtin setup for AOT ───────────────────────── */
extern RizValue native_range(RizValue* a, int c);
extern RizValue native_len(RizValue* a, int c);
extern RizValue native_type(RizValue* a, int c);
extern RizValue native_str(RizValue* a, int c);
extern RizValue native_int_cast(RizValue* a, int c);
extern RizValue native_float_cast(RizValue* a, int c);
extern RizValue native_abs(RizValue* a, int c);
extern RizValue native_min(RizValue* a, int c);
extern RizValue native_max(RizValue* a, int c);
extern RizValue native_sum(RizValue* a, int c);
extern RizValue native_clamp(RizValue* a, int c);
extern RizValue native_sign(RizValue* a, int c);
extern RizValue native_floor_fn(RizValue* a, int c);
extern RizValue native_ceil_fn(RizValue* a, int c);
extern RizValue native_round_fn(RizValue* a, int c);
extern RizValue native_all(RizValue* a, int c);
extern RizValue native_any(RizValue* a, int c);
extern RizValue native_as_bool(RizValue* a, int c);
extern RizValue native_ord(RizValue* a, int c);
extern RizValue native_chr(RizValue* a, int c);
extern RizValue native_extend(RizValue* a, int c);
extern RizValue native_debug(RizValue* a, int c);
extern RizValue native_panic(RizValue* a, int c);

void aot_setup_builtins(void) {
    aot_register_user_fn("range", (NativeFnPtr)native_range, -1);
    aot_register_user_fn("len", (NativeFnPtr)native_len, 1);
    aot_register_user_fn("type", (NativeFnPtr)native_type, 1);
    aot_register_user_fn("str", (NativeFnPtr)native_str, 1);
    aot_register_user_fn("int", (NativeFnPtr)native_int_cast, 1);
    aot_register_user_fn("float", (NativeFnPtr)native_float_cast, 1);
    aot_register_user_fn("abs", (NativeFnPtr)native_abs, 1);
    aot_register_user_fn("min", (NativeFnPtr)native_min, -1);
    aot_register_user_fn("max", (NativeFnPtr)native_max, -1);
    aot_register_user_fn("sum", (NativeFnPtr)native_sum, 1);
    aot_register_user_fn("clamp", (NativeFnPtr)native_clamp, 3);
    aot_register_user_fn("sign", (NativeFnPtr)native_sign, 1);
    aot_register_user_fn("floor", (NativeFnPtr)native_floor_fn, 1);
    aot_register_user_fn("ceil", (NativeFnPtr)native_ceil_fn, 1);
    aot_register_user_fn("round", (NativeFnPtr)native_round_fn, 1);
    aot_register_user_fn("all", (NativeFnPtr)native_all, 1);
    aot_register_user_fn("any", (NativeFnPtr)native_any, 1);
    aot_register_user_fn("bool", (NativeFnPtr)native_as_bool, 1);
    aot_register_user_fn("ord", (NativeFnPtr)native_ord, 1);
    aot_register_user_fn("chr", (NativeFnPtr)native_chr, 1);
    aot_register_user_fn("extend", (NativeFnPtr)native_extend, 2);
    aot_register_user_fn("debug", (NativeFnPtr)native_debug, -1);
    aot_register_user_fn("panic", (NativeFnPtr)native_panic, -1);
}

RizValue aot_call_plugin(const char* name, int arg_count, RizValue* args) {
    for (int i = 0; i < aot_fn_count; i++) {
        if (strcmp(aot_fns[i].name, name) == 0) {
            if (aot_fns[i].arity != -1 && aot_fns[i].arity != arg_count) {
                fprintf(stderr, "[AOT-FFI] Argument mismatch for '%s'\n", name);
                exit(1);
            }
            return aot_fns[i].fn(args, arg_count);
        }
    }
    fprintf(stderr, "[AOT-FFI] Undefined function '%s'\n", name);
    exit(1);
}
