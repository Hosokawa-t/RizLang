/*
 * Riz Programming Language
 * riz_plugin.h — C Plugin API for native extensions (Phase 5)
 *
 * Native plugins (.dll/.so) must implement a single entry point:
 *   RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api);
 *
 * The `api` pointer provides functions for registering native Riz functions.
 *
 * Build your plugin:
 *   gcc -shared -O2 -I<riz_src_dir> -o myplugin.dll myplugin.c
 *
 * Use in Riz:
 *   import_native "myplugin.dll"
 *   import_python            (optional path; defaults to plugin_python.{dll,so,dylib})
 */

#ifndef RIZ_PLUGIN_H
#define RIZ_PLUGIN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── Export Macro ────────────────────────────────────── */
#ifdef _WIN32
  #define RIZ_EXPORT __declspec(dllexport)
#else
  #define RIZ_EXPORT __attribute__((visibility("default")))
#endif

/* ─── Forward declarations ───────────────────────────── */
/* Plugin operates on the same RizValue as the host interpreter.
 * We include value.h so plugins have full access to the struct layout. */
#include "value.h"

typedef RizValue RizPluginValue;

/* ─── Plugin Function Signature ───────────────────────── */
typedef RizPluginValue (*RizPluginFn)(RizPluginValue* args, int arg_count);

/* ─── Plugin API (passed to riz_plugin_init) ──────────── */
typedef struct {
    /* Register a new native function visible to Riz scripts.
     * name:  function name in Riz (e.g., "matrix_mul")
     * fn:    function pointer matching RizPluginFn
     * arity: expected argument count (-1 for variadic) */
    void (*register_fn)(void* interp, const char* name, RizPluginFn fn, int arity);

    /* Empty dict (ref-counted). Use dict_set_fn + define_global for namespaces like `py`. */
    RizPluginValue (*make_dict)(void);
    /* Store a native under key in dict (e.g. key "exec", riz_name "py.exec" for errors). */
    void (*dict_set_fn)(RizPluginValue dict, const char* key, const char* riz_name, RizPluginFn fn, int arity);
    /* Bind an arbitrary value into global scope (e.g. the `py` dict). */
    void (*define_global)(void* interp, const char* name, RizPluginValue value);

    /* Convenience constructors for returning values to Riz: */
    RizPluginValue (*make_int)(int64_t v);
    RizPluginValue (*make_float)(double v);
    RizPluginValue (*make_bool)(bool v);
    RizPluginValue (*make_string)(const char* v);
    RizPluginValue (*make_none)(void);
    RizPluginValue (*make_list)(void);

    /* Native pointer: wraps a C pointer so Riz can hold it as a variable.
     * type_tag: human-readable label (e.g. "Tensor", "Model")
     * dtor:     called when Riz frees this value (NULL = no cleanup) */
    RizPluginValue (*make_native_ptr)(void* ptr, const char* type_tag,
                                      void (*dtor)(void*));

    /* Extract the raw C pointer from a VAL_NATIVE_PTR value: */
    void* (*get_native_ptr)(RizPluginValue v);

    /* List manipulation: */
    void (*list_append)(RizPluginValue list, RizPluginValue v);
    int (*list_length)(RizPluginValue list);
    RizPluginValue (*list_get)(RizPluginValue list, int index);

    /* The interpreter pointer (opaque to plugins): */
    void* interp;

    /* ─── Diagnostics & Error Handling ────────────────────── */
    int (*get_current_line)(void* interp);
    void (*panic)(void* interp, const char* msg);
} RizPluginAPI;

/* ─── Plugin Init Function Signature ──────────────────── */
typedef void (*RizPluginInitFn)(RizPluginAPI* api);

#endif /* RIZ_PLUGIN_H */
