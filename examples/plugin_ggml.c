/*
 * Riz Tensor Plugin -- GGML Backend (Phase 6.2)
 *
 * Uses ggml's memory management (context, tensor allocation) for
 * efficient, arena-based tensor storage, while performing actual
 * computation with optimized C loops. This avoids the need to link
 * the full ggml-cpu backend (which requires a C++ build chain).
 *
 * When CUDA or full ggml-cpu support is desired, this plugin can
 * be extended to call ggml_graph_compute() instead of the inline loops.
 *
 * Build:  build_ggml.bat
 * Use:    import_native "plugin_ggml.dll"
 */

#include "riz_plugin.h"
#include <ggml.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

static RizPluginAPI G;

/* ---- GGML Tensor wrapper ---- */

typedef struct {
    struct ggml_context* ctx;
    struct ggml_tensor*  t;
    int rows;
    int cols;
} GgmlTensor;

static GgmlTensor* gt_alloc(int rows, int cols) {
    size_t data_size = sizeof(float) * rows * cols;
    struct ggml_init_params p = {
        .mem_size   = data_size + ggml_tensor_overhead() + 512,
        .mem_buffer = NULL,
        .no_alloc   = false,
    };
    struct ggml_context* ctx = ggml_init(p);
    struct ggml_tensor*  t   = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, cols, rows);

    GgmlTensor* gt = (GgmlTensor*)malloc(sizeof(GgmlTensor));
    gt->ctx  = ctx;
    gt->t    = t;
    gt->rows = rows;
    gt->cols = cols;
    return gt;
}

static void gt_dtor(void* ptr) {
    GgmlTensor* gt = (GgmlTensor*)ptr;
    if (gt) { if (gt->ctx) ggml_free(gt->ctx); free(gt); }
}

static float* gt_data(GgmlTensor* gt) { return (float*)gt->t->data; }
static GgmlTensor* as_gt(RizPluginValue v) { return (GgmlTensor*)G.get_native_ptr(v); }
static RizPluginValue wrap(GgmlTensor* gt)  { return G.make_native_ptr(gt, "Tensor", gt_dtor); }

/* ---- API: Creation ---- */

static RizPluginValue fn_zeros(RizPluginValue* a, int n) {
    int r=(int)a[0].as.integer, c=(int)a[1].as.integer;
    GgmlTensor* gt = gt_alloc(r, c);
    memset(gt_data(gt), 0, sizeof(float)*r*c);
    return wrap(gt);
}

static RizPluginValue fn_ones(RizPluginValue* a, int n) {
    int r=(int)a[0].as.integer, c=(int)a[1].as.integer;
    GgmlTensor* gt = gt_alloc(r, c);
    float* d = gt_data(gt);
    for (int i=0;i<r*c;i++) d[i]=1.0f;
    return wrap(gt);
}

static RizPluginValue fn_rand(RizPluginValue* a, int n) {
    int r=(int)a[0].as.integer, c=(int)a[1].as.integer;
    GgmlTensor* gt = gt_alloc(r, c);
    float* d = gt_data(gt);
    for (int i=0;i<r*c;i++) d[i]=(float)rand()/(float)RAND_MAX;
    return wrap(gt);
}

/* ---- API: Element-wise ops ---- */

static RizPluginValue fn_add(RizPluginValue* a, int n) {
    GgmlTensor *A=as_gt(a[0]), *B=as_gt(a[1]);
    if (!A||!B||A->rows!=B->rows||A->cols!=B->cols) return G.make_none();
    GgmlTensor* C = gt_alloc(A->rows, A->cols);
    float *da=gt_data(A), *db=gt_data(B), *dc=gt_data(C);
    int sz = A->rows * A->cols;
    for (int i=0;i<sz;i++) dc[i]=da[i]+db[i];
    return wrap(C);
}

static RizPluginValue fn_mul(RizPluginValue* a, int n) {
    GgmlTensor *A=as_gt(a[0]), *B=as_gt(a[1]);
    if (!A||!B||A->rows!=B->rows||A->cols!=B->cols) return G.make_none();
    GgmlTensor* C = gt_alloc(A->rows, A->cols);
    float *da=gt_data(A), *db=gt_data(B), *dc=gt_data(C);
    int sz = A->rows * A->cols;
    for (int i=0;i<sz;i++) dc[i]=da[i]*db[i];
    return wrap(C);
}

/* ---- API: Matrix multiplication ---- */

static RizPluginValue fn_matmul(RizPluginValue* args, int n) {
    GgmlTensor *A=as_gt(args[0]), *B=as_gt(args[1]);
    if (!A||!B||A->cols!=B->rows) {
        fprintf(stderr,"[ggml] matmul: shape mismatch (%dx%d) x (%dx%d)\n",
                A?A->rows:0,A?A->cols:0,B?B->rows:0,B?B->cols:0);
        return G.make_none();
    }
    GgmlTensor* C = gt_alloc(A->rows, B->cols);
    float *da=gt_data(A), *db=gt_data(B), *dc=gt_data(C);
    int M=A->rows, K=A->cols, N=B->cols;
    for (int i=0;i<M;i++)
        for (int j=0;j<N;j++) {
            float s=0.0f;
            for (int k=0;k<K;k++) s += da[i*K+k]*db[k*N+j];
            dc[i*N+j] = s;
        }
    return wrap(C);
}

/* ---- API: Reductions ---- */

static RizPluginValue fn_sum(RizPluginValue* a, int n) {
    GgmlTensor* A = as_gt(a[0]);
    if (!A) return G.make_float(0);
    float* d=gt_data(A); double s=0;
    for (int i=0;i<A->rows*A->cols;i++) s+=d[i];
    return G.make_float(s);
}

/* ---- API: Inspection  ---- */

static RizPluginValue fn_shape(RizPluginValue* a, int n) {
    GgmlTensor* A = as_gt(a[0]);
    if (!A) return G.make_none();
    RizPluginValue l = G.make_list();
    G.list_append(l.as.list, G.make_int(A->rows));
    G.list_append(l.as.list, G.make_int(A->cols));
    return l;
}

static RizPluginValue fn_print(RizPluginValue* a, int n) {
    GgmlTensor* A = as_gt(a[0]);
    if (!A) { printf("Tensor(null)\n"); return G.make_none(); }
    float* d = gt_data(A);
    printf("Tensor(%dx%d) [ggml] [\n", A->rows, A->cols);
    for (int i=0;i<A->rows;i++) {
        printf("  [");
        for (int j=0;j<A->cols;j++) {
            if (j>0) printf(", ");
            printf("%8.4f", d[i*A->cols+j]);
        }
        printf("]\n");
    }
    printf("]\n");
    return G.make_none();
}

static RizPluginValue fn_free_t(RizPluginValue* a, int n) {
    GgmlTensor* A = as_gt(a[0]);
    if (A && A->ctx) { ggml_free(A->ctx); A->ctx=NULL; A->t=NULL; }
    return G.make_none();
}

/* ---- Entry point ---- */

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;
    srand((unsigned)time(NULL));

    api->register_fn(api->interp, "tensor_zeros",   fn_zeros,   2);
    api->register_fn(api->interp, "tensor_ones",    fn_ones,    2);
    api->register_fn(api->interp, "tensor_rand",    fn_rand,    2);
    api->register_fn(api->interp, "tensor_add",     fn_add,     2);
    api->register_fn(api->interp, "tensor_mul",     fn_mul,     2);
    api->register_fn(api->interp, "tensor_matmul",  fn_matmul,  2);
    api->register_fn(api->interp, "tensor_sum",     fn_sum,     1);
    api->register_fn(api->interp, "tensor_shape",   fn_shape,   1);
    api->register_fn(api->interp, "tensor_print",   fn_print,   1);
    api->register_fn(api->interp, "tensor_free",    fn_free_t,  1);
}
