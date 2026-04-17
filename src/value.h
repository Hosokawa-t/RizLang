/*
 * Riz Programming Language
 * value.h — Runtime value type definitions
 */

#ifndef RIZ_VALUE_H
#define RIZ_VALUE_H

#include "common.h"

/* ─── Forward Declarations ────────────────────────────── */
typedef struct ASTNode ASTNode;
typedef struct Environment Environment;
typedef struct RizValue RizValue;
typedef struct Chunk Chunk;

/* ─── Value Types ─────────────────────────────────────── */
typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STRING,
    VAL_NONE,
    VAL_FUNCTION,
    VAL_NATIVE_FN,
    VAL_LIST,
    VAL_DICT,
    VAL_STRUCT_DEF,
    VAL_INSTANCE,
    VAL_TRAIT_DEF,
    VAL_NATIVE_PTR,    /* Opaque pointer to plugin-owned data (e.g. Tensor) */
    VAL_VM_CLOSURE     /* Bytecode function for the register VM (separate from AST RizFunction) */
} ValueType;

#ifndef RIZ_API_CALL
#ifdef _WIN32
#define RIZ_API_CALL __cdecl
#else
#define RIZ_API_CALL
#endif
#endif

/* ─── Native Function Pointer ─────────────────────────── */
typedef RizValue (RIZ_API_CALL *NativeFnPtr)(RizValue* args, int arg_count);

/* ─── Riz List ────────────────────────────────────────── */
typedef struct {
    RizValue* items;
    int count;
    int capacity;
    int ref_count;
} RizList;

/* ─── Riz Dictionary ──────────────────────────────────── */
typedef struct {
    char**    keys;
    RizValue* values;
    int       count;
    int       capacity;
    int*      hash_table;     /* O(1) lookup table, stores indices */
    int       hash_capacity;
    int       ref_count;
} RizDict;

/* ─── Riz Struct Definition ───────────────────────────── */
typedef struct {
    char*     name;
    char**    field_names;
    int       field_count;
    /* Method table */
    char**    method_names;
    RizValue* method_values;    /* array of VAL_FUNCTION */
    int       method_count;
    int       method_cap;
} RizStructDef;

/* ─── Riz Instance ────────────────────────────────────── */
typedef struct {
    RizStructDef* def;
    RizValue*     fields;
    int           ref_count;
} RizInstance;

/* ─── Riz Function ────────────────────────────────────── */
typedef struct {
    char*        name;
    char**       params;
    int          param_count;
    ASTNode*     body;
    Environment* closure;
    ASTNode**    param_defaults;  /* NULL entries = required */
    int          required_count;  /* params without defaults */
} RizFunction;

/* ─── Native Function Object ──────────────────────────── */
typedef struct {
    char*       name;
    NativeFnPtr fn;
    int         arity;   /* -1 = variadic */
} NativeFnObj;

/* ─── VM bytecode closure (compiler + vm_execute) ─────── */
typedef struct RizVMClosure {
    Chunk* chunk;
    int    param_count;
    int    stack_slots; /* register window; >= param_count */
    char*  name;
    int    ref_count;   /* shared with constant pool + globals + registers */
} RizVMClosure;

/* ─── Riz Trait Definition ──────────────────────────────── */
typedef struct {
    char*  name;
    char** method_names;
    int*   method_arity;
    int    method_count;
} RizTraitDef;

/* ─── Native Pointer (opaque handle for plugins) ──────── */
typedef void (*RizPtrDestructor)(void* ptr);

typedef struct {
    void*             ptr;          /* The raw C pointer (e.g. Tensor*) */
    char*             type_tag;     /* Human-readable tag: "Tensor", "Model" */
    RizPtrDestructor  destructor;   /* Called on free; NULL = no-op */
    int               ref_count;
} RizNativePtr;

/* ─── The Main Value ──────────────────────────────────── */
struct RizValue {
    ValueType type;
    union {
        int64_t       integer;
        double        floating;
        bool          boolean;
        char*         string;
        RizFunction*  function;
        NativeFnObj*  native;
        RizList*      list;
        RizDict*      dict;
        RizStructDef* struct_def;
        RizInstance*  instance;
        RizTraitDef*  trait_def;
        RizNativePtr* native_ptr;   /* Opaque handle to plugin data */
        RizVMClosure* vm_closure;
    } as;
};

/* ─── Constructors ────────────────────────────────────── */
RizValue riz_int(int64_t v);
RizValue riz_float(double v);
RizValue riz_bool(bool v);
RizValue riz_string(const char* v);
RizValue riz_string_take(char* v);   /* takes ownership */
RizValue riz_none(void);
RizValue riz_fn(RizFunction* fn);
RizValue riz_native(const char* name, NativeFnPtr fn, int arity);
RizValue riz_list_new(void);
RizValue riz_struct_def_new(const char* name, char** fields, int field_count);
RizValue riz_instance_new(RizStructDef* def, RizValue* field_values);
RizValue riz_native_ptr(void* ptr, const char* type_tag, RizPtrDestructor dtor);
RizValue riz_vm_closure_val(RizVMClosure* cl); /* takes ownership of cl and cl->chunk */

/* ─── Operations ──────────────────────────────────────── */
void     riz_value_print(RizValue v);
char*    riz_value_to_string(RizValue v);
bool     riz_value_is_truthy(RizValue v);
bool     riz_value_equal(RizValue a, RizValue b);
RizValue riz_value_copy(RizValue v);
void     riz_value_free(RizValue* v);
const char* riz_value_type_name(RizValue v);

/* ─── List Operations ─────────────────────────────────── */
void     riz_list_append(RizList* list, RizValue v);
RizValue riz_list_get(RizList* list, int index);
int      riz_list_length(RizList* list);

/* ─── Dict Operations ─────────────────────────────────── */
RizValue riz_dict_new(void);
void     riz_dict_set(RizDict* dict, const char* key, RizValue value);
RizValue riz_dict_get(RizDict* dict, const char* key);
bool     riz_dict_has(RizDict* dict, const char* key);
void     riz_dict_delete(RizDict* dict, const char* key);
RizValue riz_dict_keys(RizDict* dict);
RizValue riz_dict_values(RizDict* dict);

#endif /* RIZ_VALUE_H */
