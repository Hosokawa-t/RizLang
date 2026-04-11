/*
 * Riz Programming Language
 * aot_runtime.h — AOT Standalone Runtime Core
 *
 * This provides the full RizValue ecosystem for native compiled binaries,
 * along with the FFI bridge implementation so compiled binaries can load plugins.
 */

#ifndef RIZ_AOT_RUNTIME_H
#define RIZ_AOT_RUNTIME_H

#include "value.h"
#include "riz_plugin.h"
#include <math.h>

/* AOT Line Tracking & Diagnostics */
extern int aot_current_line;
extern const char* aot_source_path;
void aot_panic(void* interp, const char* msg);

/* AOT FFI Loader API */
void aot_load_plugin(const char* lib_path);
void aot_register_user_fn(const char* name, NativeFnPtr fn, int arity);
RizValue aot_call_plugin(const char* name, int arg_count, RizValue* args);

/* AOT Math wrappers */
static inline RizValue aot_add(RizValue a, RizValue b) {
    if (a.type == VAL_NATIVE_PTR || b.type == VAL_NATIVE_PTR) {
        if ((a.type != VAL_NATIVE_PTR || strcmp(a.as.native_ptr->type_tag, "Tensor") == 0) &&
            (b.type != VAL_NATIVE_PTR || strcmp(b.as.native_ptr->type_tag, "Tensor") == 0)) {
            RizValue args[2] = {a, b};
            return aot_call_plugin("tensor_add", 2, args);
        }
    }
    if (a.type == VAL_INT && b.type == VAL_INT) return riz_int(a.as.integer + b.as.integer);
    if (a.type == VAL_FLOAT && b.type == VAL_FLOAT) return riz_float(a.as.floating + b.as.floating);
    double fa = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
    double fb = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
    return riz_float(fa + fb);
}

static inline RizValue aot_sub(RizValue a, RizValue b) {
    if (a.type == VAL_NATIVE_PTR || b.type == VAL_NATIVE_PTR) {
        if ((a.type != VAL_NATIVE_PTR || strcmp(a.as.native_ptr->type_tag, "Tensor") == 0) &&
            (b.type != VAL_NATIVE_PTR || strcmp(b.as.native_ptr->type_tag, "Tensor") == 0)) {
            RizValue args[2] = {a, b};
            return aot_call_plugin("tensor_sub", 2, args);
        }
    }
    if (a.type == VAL_INT && b.type == VAL_INT) return riz_int(a.as.integer - b.as.integer);
    double fa = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
    double fb = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
    return riz_float(fa - fb);
}

static inline RizValue aot_mul(RizValue a, RizValue b) {
    if (a.type == VAL_NATIVE_PTR || b.type == VAL_NATIVE_PTR) {
        if ((a.type != VAL_NATIVE_PTR || strcmp(a.as.native_ptr->type_tag, "Tensor") == 0) &&
            (b.type != VAL_NATIVE_PTR || strcmp(b.as.native_ptr->type_tag, "Tensor") == 0)) {
            RizValue args[2] = {a, b};
            return aot_call_plugin("tensor_matmul", 2, args);
        }
    }
    if (a.type == VAL_INT && b.type == VAL_INT) return riz_int(a.as.integer * b.as.integer);
    double fa = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
    double fb = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
    return riz_float(fa * fb);
}

static inline RizValue aot_div(RizValue a, RizValue b) {
    double fa = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
    double fb = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
    return riz_float(fa / fb);
}

static inline RizValue aot_mod(RizValue a, RizValue b) {
    if (a.type == VAL_INT && b.type == VAL_INT) return riz_int(a.as.integer % b.as.integer);
    return riz_float(0); /* Simplified */
}

static inline RizValue aot_idiv(RizValue a, RizValue b) {
    if (a.type == VAL_INT && b.type == VAL_INT) return riz_int(a.as.integer / b.as.integer);
    return riz_int(0); /* Simplified */
}

static inline RizValue aot_pow(RizValue a, RizValue b) {
    double fa = (a.type == VAL_FLOAT) ? a.as.floating : (double)a.as.integer;
    double fb = (b.type == VAL_FLOAT) ? b.as.floating : (double)b.as.integer;
    return riz_float(pow(fa, fb));
}

static inline RizValue aot_neg(RizValue a) {
    if (a.type == VAL_INT) return riz_int(-a.as.integer);
    if (a.type == VAL_FLOAT) return riz_float(-a.as.floating);
    return riz_none();
}

static inline double aot_num(RizValue a) {
    if (a.type == VAL_INT) return (double)a.as.integer;
    if (a.type == VAL_FLOAT) return a.as.floating;
    return 0;
}

static inline RizValue aot_index(RizValue obj, RizValue idx) {
    if (obj.type == VAL_LIST) {
        int i = 0;
        if (idx.type == VAL_INT) i = (int)idx.as.integer;
        else if (idx.type == VAL_FLOAT) i = (int)idx.as.floating;
        if (i < 0) i += obj.as.list->count;
        return riz_list_get(obj.as.list, i);
    }
    return riz_none();
}

/* Helpers */
static inline void aot_print(int n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++) {
        RizValue v = __builtin_va_arg(ap, RizValue);
        if (i > 0) printf(" ");
        riz_value_print(v);
    }
    printf("\n");
    __builtin_va_end(ap);
}

#endif
