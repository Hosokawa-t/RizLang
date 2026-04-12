/*
 * Riz Native Extension: math_ext
 * Compile from repo root:
 *   gcc -shared -O2 -Isrc -o examples/intro/math_ext.dll examples/intro/plugin_math.c
 *
 * Provides high-performance math functions to Riz scripts.
 */

#include "riz_plugin.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global API pointer — set during riz_plugin_init */
static RizPluginAPI* ffi_api;

/* ─── Helper: extract a number as double ──────────────── */
static double to_double(RizPluginValue v) {
    if (v.type == VAL_INT) return (double)v.as.integer;
    if (v.type == VAL_FLOAT) return v.as.floating;
    return 0.0;
}

/* ─── Native Functions ────────────────────────────────── */

static RizPluginValue native_math_sin(RizPluginValue* args, int argc) {
    return ffi_api->make_float(sin(to_double(args[0])));
}
static RizPluginValue native_math_cos(RizPluginValue* args, int argc) {
    return ffi_api->make_float(cos(to_double(args[0])));
}
static RizPluginValue native_math_tan(RizPluginValue* args, int argc) {
    return ffi_api->make_float(tan(to_double(args[0])));
}
static RizPluginValue native_math_sqrt(RizPluginValue* args, int argc) {
    return ffi_api->make_float(sqrt(to_double(args[0])));
}
static RizPluginValue native_math_log(RizPluginValue* args, int argc) {
    return ffi_api->make_float(log(to_double(args[0])));
}
static RizPluginValue native_math_exp(RizPluginValue* args, int argc) {
    return ffi_api->make_float(exp(to_double(args[0])));
}
static RizPluginValue native_math_floor(RizPluginValue* args, int argc) {
    return ffi_api->make_int((int64_t)floor(to_double(args[0])));
}
static RizPluginValue native_math_ceil(RizPluginValue* args, int argc) {
    return ffi_api->make_int((int64_t)ceil(to_double(args[0])));
}
static RizPluginValue native_math_pi(RizPluginValue* args, int argc) {
    return ffi_api->make_float(3.14159265358979323846);
}
static RizPluginValue native_math_e(RizPluginValue* args, int argc) {
    return ffi_api->make_float(2.71828182845904523536);
}
static RizPluginValue native_math_random(RizPluginValue* args, int argc) {
    return ffi_api->make_float((double)rand() / (double)RAND_MAX);
}
static RizPluginValue native_math_random_int(RizPluginValue* args, int argc) {
    int64_t lo = args[0].as.integer;
    int64_t hi = args[1].as.integer;
    return ffi_api->make_int(lo + rand() % (hi - lo + 1));
}

/* ─── Plugin Entry Point ─────────────────────────────── */
RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    ffi_api = api;
    srand((unsigned int)time(NULL));

    api->register_fn(api->interp, "math_sin",        native_math_sin,        1);
    api->register_fn(api->interp, "math_cos",        native_math_cos,        1);
    api->register_fn(api->interp, "math_tan",        native_math_tan,        1);
    api->register_fn(api->interp, "math_sqrt",       native_math_sqrt,       1);
    api->register_fn(api->interp, "math_log",        native_math_log,        1);
    api->register_fn(api->interp, "math_exp",        native_math_exp,        1);
    api->register_fn(api->interp, "math_floor",      native_math_floor,      1);
    api->register_fn(api->interp, "math_ceil",       native_math_ceil,       1);
    api->register_fn(api->interp, "math_pi",         native_math_pi,         0);
    api->register_fn(api->interp, "math_e",          native_math_e,          0);
    api->register_fn(api->interp, "math_random",     native_math_random,     0);
    api->register_fn(api->interp, "math_random_int", native_math_random_int, 2);
}
