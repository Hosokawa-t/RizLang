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

#ifdef _WIN32
#define RIZ_API_CALL __cdecl
#else
#define RIZ_API_CALL
#endif

/* ─── Plugin Function Signature ───────────────────────── */
typedef RizPluginValue (RIZ_API_CALL *RizPluginFn)(RizPluginValue* args, int arg_count);

/* ─── Plugin API (passed to riz_plugin_init) ──────────── */

/* ─── Plugin API (passed to riz_plugin_init) ──────────── */
typedef struct {
    size_t size; /* Safety check: sizeof(RizPluginAPI) */

    /* Register a new native function visible to Riz scripts. */
    void (RIZ_API_CALL *register_fn)(void* interp, const char* name, RizPluginFn fn, int arity);

    /* Empty dict (ref-counted). */
    RizPluginValue (RIZ_API_CALL *make_dict)(void);
    void (RIZ_API_CALL *dict_set_fn)(RizPluginValue dict, const char* key, const char* riz_name, RizPluginFn fn, int arity);
    void (RIZ_API_CALL *define_global)(void* interp, const char* name, RizPluginValue value);

    /* Convenience constructors: */
    RizPluginValue (RIZ_API_CALL *make_int)(int64_t v);
    RizPluginValue (RIZ_API_CALL *make_float)(double v);
    RizPluginValue (RIZ_API_CALL *make_bool)(bool v);
    RizPluginValue (RIZ_API_CALL *make_string)(const char* v);
    RizPluginValue (RIZ_API_CALL *make_none)(void);
    RizPluginValue (RIZ_API_CALL *make_list)(void);

    /* Native pointer: */
    RizPluginValue (RIZ_API_CALL *make_native_ptr)(void* ptr, const char* type_tag, void (*dtor)(void*));
    void* (RIZ_API_CALL *get_native_ptr)(RizPluginValue v);

    /* List manipulation: */
    void (RIZ_API_CALL *list_append)(RizPluginValue list, RizPluginValue v);
    int (RIZ_API_CALL *list_length)(RizPluginValue list);
    RizPluginValue (RIZ_API_CALL *list_get)(RizPluginValue list, int index);

    /* The interpreter pointer (opaque to plugins): */
    void* interp;

    /* ─── Diagnostics & Error Handling ────────────────────── */
    int (RIZ_API_CALL *get_current_line)(void* interp);
    void (RIZ_API_CALL *panic)(void* interp, const char* msg);
} RizPluginAPI;

/* ─── Plugin Init Function Signature ──────────────────── */
typedef void (*RizPluginInitFn)(RizPluginAPI* api);

#endif /* RIZ_PLUGIN_H */
