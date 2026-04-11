/*
 * Riz Tensor Plugin -- Dummy CPU Backend (Phase 6.1)
 *
 * A minimal, self-contained tensor implementation in plain C.
 * Validates the VAL_NATIVE_PTR pipeline and the Universal Tensor API
 * before we integrate heavier backends (GGML, libtorch).
 *
 * Build:
 *   gcc -shared -O2 -Isrc -o plugin_tensor.dll examples/plugin_tensor.c
 *
 * Use in Riz:
 *   import_native "plugin_tensor.dll"
 *   let a = tensor_rand(3, 3)
 *   let b = tensor_rand(3, 3)
 *   let c = tensor_matmul(a, b)
 *   tensor_print(c)
 */

#include "riz_plugin.h"
#include "riz_tensor.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- Globals stored from the API bridge ---- */
static RizPluginAPI G;

/* ---- Tensor lifecycle ---- */

static RizTensor* tensor_alloc(int rows, int cols) {
    RizTensor* t = (RizTensor*)malloc(sizeof(RizTensor));
    t->rows = rows;
    t->cols = cols;
    t->size = rows * cols;
    t->data = (float*)calloc(t->size, sizeof(float));
    return t;
}

static void tensor_destructor(void* ptr) {
    RizTensor* t = (RizTensor*)ptr;
    if (t) { free(t->data); free(t); }
}

/* ---- Helper: extract tensor from a Riz value ---- */

static RizTensor* as_tensor(RizPluginValue v) {
    return (RizTensor*)G.get_native_ptr(v);
}

static RizPluginValue wrap_tensor(RizTensor* t) {
    return G.make_native_ptr(t, "Tensor", tensor_destructor);
}

/* ---- Tensor API functions exposed to Riz ---- */

static RizPluginValue fn_tensor_zeros(RizPluginValue* args, int argc) {
    int rows = (int)args[0].as.integer;
    int cols = (int)args[1].as.integer;
    return wrap_tensor(tensor_alloc(rows, cols));
}

static RizPluginValue fn_tensor_ones(RizPluginValue* args, int argc) {
    int rows = (int)args[0].as.integer;
    int cols = (int)args[1].as.integer;
    RizTensor* t = tensor_alloc(rows, cols);
    for (int i = 0; i < t->size; i++) t->data[i] = 1.0f;
    return wrap_tensor(t);
}

static RizPluginValue fn_tensor_rand(RizPluginValue* args, int argc) {
    int rows = (int)args[0].as.integer;
    int cols = (int)args[1].as.integer;
    RizTensor* t = tensor_alloc(rows, cols);
    for (int i = 0; i < t->size; i++)
        t->data[i] = (float)rand() / (float)RAND_MAX;
    return wrap_tensor(t);
}

static RizPluginValue fn_tensor_add(RizPluginValue* args, int argc) {
    RizTensor* a = as_tensor(args[0]);
    RizTensor* b = as_tensor(args[1]);
    if (!a || !b || a->size != b->size) {
        fprintf(stderr, "[tensor] add: shape mismatch\n");
        return G.make_none();
    }
    RizTensor* c = tensor_alloc(a->rows, a->cols);
    for (int i = 0; i < a->size; i++)
        c->data[i] = a->data[i] + b->data[i];
    return wrap_tensor(c);
}

static RizPluginValue fn_tensor_mul(RizPluginValue* args, int argc) {
    RizTensor* a = as_tensor(args[0]);
    RizTensor* b = as_tensor(args[1]);
    if (!a || !b || a->size != b->size) {
        fprintf(stderr, "[tensor] mul: shape mismatch\n");
        return G.make_none();
    }
    RizTensor* c = tensor_alloc(a->rows, a->cols);
    for (int i = 0; i < a->size; i++)
        c->data[i] = a->data[i] * b->data[i];
    return wrap_tensor(c);
}

static RizPluginValue fn_tensor_matmul(RizPluginValue* args, int argc) {
    RizTensor* a = as_tensor(args[0]);
    RizTensor* b = as_tensor(args[1]);
    if (!a || !b || a->cols != b->rows) {
        fprintf(stderr, "[tensor] matmul: incompatible shapes (%dx%d) x (%dx%d)\n",
                a ? a->rows : 0, a ? a->cols : 0, b ? b->rows : 0, b ? b->cols : 0);
        return G.make_none();
    }
    RizTensor* c = tensor_alloc(a->rows, b->cols);
    for (int i = 0; i < a->rows; i++)
        for (int j = 0; j < b->cols; j++) {
            float sum = 0.0f;
            for (int k = 0; k < a->cols; k++)
                sum += a->data[i * a->cols + k] * b->data[k * b->cols + j];
            c->data[i * c->cols + j] = sum;
        }
    return wrap_tensor(c);
}

static RizPluginValue fn_tensor_sum(RizPluginValue* args, int argc) {
    RizTensor* a = as_tensor(args[0]);
    if (!a) return G.make_float(0.0);
    double sum = 0.0;
    for (int i = 0; i < a->size; i++) sum += a->data[i];
    return G.make_float(sum);
}

static RizPluginValue fn_tensor_shape(RizPluginValue* args, int argc) {
    RizTensor* a = as_tensor(args[0]);
    if (!a) return G.make_none();
    RizPluginValue list = G.make_list();
    G.list_append(list.as.list, G.make_int(a->rows));
    G.list_append(list.as.list, G.make_int(a->cols));
    return list;
}

static RizPluginValue fn_tensor_print(RizPluginValue* args, int argc) {
    RizTensor* t = as_tensor(args[0]);
    if (!t) { printf("Tensor(null)\n"); return G.make_none(); }
    printf("Tensor(%dx%d) [\n", t->rows, t->cols);
    for (int i = 0; i < t->rows; i++) {
        printf("  [");
        for (int j = 0; j < t->cols; j++) {
            if (j > 0) printf(", ");
            printf("%8.4f", t->data[i * t->cols + j]);
        }
        printf("]\n");
    }
    printf("]\n");
    return G.make_none();
}

static RizPluginValue fn_tensor_free(RizPluginValue* args, int argc) {
    /* The destructor will be called automatically when the RizValue is freed.
     * This function is a manual override for explicit cleanup. */
    RizTensor* t = as_tensor(args[0]);
    if (t) { free(t->data); t->data = NULL; t->size = 0; }
    return G.make_none();
}

/* ---- Plugin entry point ---- */

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;  /* cache the API bridge */
    srand((unsigned)time(NULL));

    api->register_fn(api->interp, "tensor_zeros",   fn_tensor_zeros,   2);
    api->register_fn(api->interp, "tensor_ones",    fn_tensor_ones,    2);
    api->register_fn(api->interp, "tensor_rand",    fn_tensor_rand,    2);
    api->register_fn(api->interp, "tensor_add",     fn_tensor_add,     2);
    api->register_fn(api->interp, "tensor_mul",     fn_tensor_mul,     2);
    api->register_fn(api->interp, "tensor_matmul",  fn_tensor_matmul,  2);
    api->register_fn(api->interp, "tensor_sum",     fn_tensor_sum,     1);
    api->register_fn(api->interp, "tensor_shape",   fn_tensor_shape,   1);
    api->register_fn(api->interp, "tensor_print",   fn_tensor_print,   1);
    api->register_fn(api->interp, "tensor_free",    fn_tensor_free,    1);
}
