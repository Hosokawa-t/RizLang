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
static void _list_add(void* lst, RizPluginValue v) { riz_list_append((RizList*)lst, v); }

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
    api.interp = NULL; /* Opaque dummy interp, not actually used by our robust AOT API */

    init_fn(&api);
}

/* ─── FFI Dispatcher ───────────────────────────────── */

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
