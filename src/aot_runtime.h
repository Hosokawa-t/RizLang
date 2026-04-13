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
#include "common.h"
#include <math.h>

/* AOT Line Tracking & Diagnostics */
extern int aot_current_line;
extern const char* aot_source_path;
void aot_panic(void* interp, const char* msg);

/* AOT FFI Loader API */
void aot_load_plugin(const char* lib_path);
void aot_register_user_fn(const char* name, NativeFnPtr fn, int arity);
void aot_setup_builtins(void);
RizValue aot_call_plugin(const char* name, int arg_count, RizValue* args);
RizPluginFn aot_get_plugin_fn(const char* name);
void riz_runtime_set_cli_context(const char* script_path, int argc, char** argv);

/* AOT Math wrappers */
static inline RizValue aot_add(RizValue a, RizValue b) {
    if (a.type == VAL_NATIVE_PTR || b.type == VAL_NATIVE_PTR) {
        if ((a.type != VAL_NATIVE_PTR || strcmp(a.as.native_ptr->type_tag, "Tensor") == 0) &&
            (b.type != VAL_NATIVE_PTR || strcmp(b.as.native_ptr->type_tag, "Tensor") == 0)) {
            RizValue args[2] = {a, b};
            return aot_call_plugin("tensor_add", 2, args);
        }
    }
    if (a.type == VAL_STRING || b.type == VAL_STRING) {
        char* left = riz_value_to_string(a);
        char* right = riz_value_to_string(b);
        size_t llen = strlen(left);
        size_t rlen = strlen(right);
        char* joined = (char*)malloc(llen + rlen + 1);
        memcpy(joined, left, llen);
        memcpy(joined + llen, right, rlen + 1);
        free(left);
        free(right);
        return riz_string_take(joined);
    }
    if (a.type == VAL_LIST && b.type == VAL_LIST) {
        RizValue out = riz_list_new();
        for (int i = 0; i < a.as.list->count; i++) riz_list_append(out.as.list, riz_value_copy(a.as.list->items[i]));
        for (int i = 0; i < b.as.list->count; i++) riz_list_append(out.as.list, riz_value_copy(b.as.list->items[i]));
        return out;
    }
    if (a.type == VAL_DICT && b.type == VAL_DICT) {
        RizValue out = riz_dict_new();
        for (int i = 0; i < a.as.dict->count; i++) riz_dict_set(out.as.dict, a.as.dict->keys[i], riz_value_copy(a.as.dict->values[i]));
        for (int i = 0; i < b.as.dict->count; i++) riz_dict_set(out.as.dict, b.as.dict->keys[i], riz_value_copy(b.as.dict->values[i]));
        return out;
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

static inline long long aot_to_int(RizValue a) {
    if (a.type == VAL_INT) return a.as.integer;
    if (a.type == VAL_FLOAT) return (long long)a.as.floating;
    return 0;
}

static inline double aot_to_float(RizValue a) {
    if (a.type == VAL_INT) return (double)a.as.integer;
    if (a.type == VAL_FLOAT) return a.as.floating;
    return 0.0;
}

static inline RizValue aot_index(RizValue obj, RizValue idx) {
    if (obj.type == VAL_LIST) {
        int i = 0;
        if (idx.type == VAL_INT) i = (int)idx.as.integer;
        else if (idx.type == VAL_FLOAT) i = (int)idx.as.floating;
        if (i < 0) i += obj.as.list->count;
        return riz_list_get(obj.as.list, i);
    }
    if (obj.type == VAL_STRING && idx.type == VAL_INT) {
        int i = (int)idx.as.integer;
        int len = (int)strlen(obj.as.string);
        if (i < 0) i += len;
        if (i < 0 || i >= len) return riz_none();
        char out[2] = { obj.as.string[i], '\0' };
        return riz_string(out);
    }
    if (obj.type == VAL_DICT) {
        if (idx.type == VAL_STRING) return riz_dict_get(obj.as.dict, idx.as.string);
        char* key = riz_value_to_string(idx);
        RizValue out = riz_dict_get(obj.as.dict, key);
        free(key);
        return out;
    }
    return riz_none();
}

/* Helpers */
#include <stdarg.h>

static inline void aot_print(int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        RizValue v = va_arg(ap, RizValue);
        if (i > 0) printf(" ");
        riz_value_print(v);
    }
    printf("\n");
    va_end(ap);
}

static inline RizValue aot_input(int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        RizValue v = va_arg(ap, RizValue);
        if (i > 0) printf(" ");
        riz_value_print(v);
    }
    va_end(ap);
    
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return riz_string("");
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return riz_string(buf);
}

static inline RizValue aot_member_get(RizValue obj, const char* member) {
    if (obj.type == VAL_LIST && (strcmp(member, "count") == 0 || strcmp(member, "length") == 0)) {
        return riz_int(obj.as.list->count);
    }
    if (obj.type == VAL_STRING && strcmp(member, "length") == 0) {
        return riz_int((int64_t)strlen(obj.as.string));
    }
    if (obj.type == VAL_DICT) {
        if (riz_dict_has(obj.as.dict, member)) return riz_dict_get(obj.as.dict, member);
        if (strcmp(member, "count") == 0 || strcmp(member, "length") == 0) {
            return riz_int(obj.as.dict->count);
        }
    }
    fprintf(stderr, "[AOT] Warning: Dynamic member get '%s' not fully integrated in runtime\n", member);
    return riz_none();
}

static inline void aot_member_set(RizValue obj, const char* member, RizValue val) {
    if (obj.type == VAL_DICT) {
        riz_dict_set(obj.as.dict, member, riz_value_copy(val));
        return;
    }
    fprintf(stderr, "[AOT] Warning: Dynamic member set '%s' not fully integrated in runtime\n", member);
}
struct Interpreter;
extern RizValue string_method(struct Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc);
extern RizValue list_method(struct Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc);
extern RizValue dict_method(struct Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc);

static inline RizValue aot_slice_call(RizValue obj, RizValue start_val, RizValue end_val, RizValue step_val) {
    int start = 0, end = 0, step = 1;
    if (step_val.type == VAL_INT) step = step_val.as.integer;
    if (step == 0) return riz_none();
    
    if (obj.type == VAL_LIST) {
        RizList* list = obj.as.list;
        end = list->count;
        if (start_val.type == VAL_INT) start = start_val.as.integer; else if (step < 0) start = list->count - 1;
        if (end_val.type == VAL_INT) end = end_val.as.integer; else if (step < 0) end = -1;
        
        if (start < 0 && start_val.type == VAL_INT) start += list->count;
        if (end < 0 && end_val.type == VAL_INT) end += list->count;
        if (start < 0) start = -1;
        if (start > list->count) start = list->count;
        if (end < -1) end = -1;
        if (end > list->count) end = list->count;
        
        RizValue result = riz_list_new();
        if (step > 0) {
            for (int i = start; i < end; i += step) riz_list_append(result.as.list, riz_value_copy(list->items[i]));
        } else {
            for (int i = start; i > end; i += step) riz_list_append(result.as.list, riz_value_copy(list->items[i]));
        }
        return result;
    }
    if (obj.type == VAL_STRING) {
        const char* str = obj.as.string;
        int len = strlen(str);
        end = len;
        if (start_val.type == VAL_INT) start = start_val.as.integer; else if (step < 0) start = len - 1;
        if (end_val.type == VAL_INT) end = end_val.as.integer; else if (step < 0) end = -1;
        
        if (start < 0 && start_val.type == VAL_INT) start += len;
        if (end < 0 && end_val.type == VAL_INT) end += len;
        if (start < 0) start = -1;
        if (start > len) start = len;
        if (end < -1) end = -1;
        if (end > len) end = len;
        
        char* buf = (char*)malloc(len + 1);
        int j = 0;
        if (step > 0) {
            for (int i = start; i < end && i >= 0 && i < len; i += step) buf[j++] = str[i];
        } else {
            for (int i = start; i > end && i >= 0 && i < len; i += step) buf[j++] = str[i];
        }
        buf[j] = '\0';
        return riz_string_take(buf);
    }
    return riz_none();
}

static inline RizValue aot_method_call(RizValue obj, const char* method, int argc, RizValue* args) {
    if (obj.type == VAL_STRING) return string_method(NULL, obj, method, args, argc);
    if (obj.type == VAL_LIST) return list_method(NULL, obj, method, args, argc);
    if (obj.type == VAL_DICT) return dict_method(NULL, obj, method, args, argc);
    fprintf(stderr, "[AOT] Warning: Dynamic method call '%s' not fully integrated\n", method);
    return riz_none();
}
#endif
