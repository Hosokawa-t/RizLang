/*
 * Riz Tensor Plugin -- Full Autograd Engine (Phase 6.3)
 *
 * A complete, dependency-free neural network backend for Riz.
 *
 *   Creation:   tensor_zeros, tensor_ones, tensor_rand, tensor_param
 *   Arithmetic: tensor_add, tensor_sub, tensor_mul, tensor_matmul
 *   Activation: tensor_relu, tensor_sigmoid
 *   Broadcast:  tensor_bias_add (add 1xN bias to each row of MxN)
 *   Reduction:  tensor_sum (returns 1x1 Tensor for autograd)
 *   Autograd:   tensor_backward, tensor_grad, tensor_zero_grad
 *   Optimizer:  tensor_sgd
 *   Data:       tensor_set, tensor_get
 *   Inspection: tensor_shape, tensor_print
 *
 * Build:
 *   gcc -shared -O2 -Isrc -o plugin_tensor.dll examples/plugin_tensor.c -lm
 */

#include "riz_plugin.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

static RizPluginAPI G;

/* ==================================================================
 * Tensor with Autograd
 * ================================================================== */

#define OP_NONE      0
#define OP_ADD       1
#define OP_SUB       2
#define OP_MUL       3
#define OP_MATMUL    4
#define OP_RELU      5
#define OP_SIGMOID   6
#define OP_SUM       7
#define OP_BIAS_ADD  8
#define OP_TANH      9
#define OP_SCALE_DIV 10

typedef struct Tensor {
    float* data;
    float* grad;
    int    rows, cols, size;
    int    op;
    struct Tensor* src[2];
    float  scalar;          /* used by scale_div */
    int    requires_grad;
    int    _visited;
} Tensor;

/* ---- Lifecycle ---- */

static Tensor* t_new(int r, int c, int rg) {
    Tensor* t = (Tensor*)calloc(1, sizeof(Tensor));
    t->rows = r; t->cols = c; t->size = r * c;
    t->data = (float*)calloc(t->size, sizeof(float));
    t->requires_grad = rg;
    return t;
}

static void t_dtor(void* p) {
    Tensor* t = (Tensor*)p;
    if (t) { free(t->data); free(t->grad); free(t); }
}

static void t_ensure_grad(Tensor* t) {
    if (!t->grad) t->grad = (float*)calloc(t->size, sizeof(float));
}

static Tensor* as_t(RizPluginValue v) { return (Tensor*)G.get_native_ptr(v); }
static RizPluginValue wrap(Tensor* t) { return G.make_native_ptr(t, "Tensor", t_dtor); }

/* ==================================================================
 * Forward Operations
 * ================================================================== */

static Tensor* fwd_add(Tensor* a, Tensor* b) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad || b->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = a->data[i] + b->data[i];
    c->op = OP_ADD; c->src[0] = a; c->src[1] = b;
    return c;
}

static Tensor* fwd_sub(Tensor* a, Tensor* b) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad || b->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = a->data[i] - b->data[i];
    c->op = OP_SUB; c->src[0] = a; c->src[1] = b;
    return c;
}

static Tensor* fwd_mul(Tensor* a, Tensor* b) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad || b->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = a->data[i] * b->data[i];
    c->op = OP_MUL; c->src[0] = a; c->src[1] = b;
    return c;
}

static Tensor* fwd_matmul(Tensor* a, Tensor* b) {
    int M = a->rows, K = a->cols, N = b->cols;
    Tensor* c = t_new(M, N, a->requires_grad || b->requires_grad);
    #define TILE 32
    for (int i0=0;i0<M;i0+=TILE)
     for (int j0=0;j0<N;j0+=TILE)
      for (int k0=0;k0<K;k0+=TILE) {
        int im=(i0+TILE<M)?i0+TILE:M, jm=(j0+TILE<N)?j0+TILE:N, km=(k0+TILE<K)?k0+TILE:K;
        for (int i=i0;i<im;i++) for (int k=k0;k<km;k++) {
            float v=a->data[i*K+k];
            for (int j=j0;j<jm;j++) c->data[i*N+j]+=v*b->data[k*N+j];
        }
      }
    #undef TILE
    c->op = OP_MATMUL; c->src[0] = a; c->src[1] = b;
    return c;
}

static Tensor* fwd_relu(Tensor* a) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = a->data[i] > 0 ? a->data[i] : 0;
    c->op = OP_RELU; c->src[0] = a;
    return c;
}

static float sigmoidf(float x) {
    if (x > 20) return 1.0f;
    if (x < -20) return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

static Tensor* fwd_sigmoid(Tensor* a) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = sigmoidf(a->data[i]);
    c->op = OP_SIGMOID; c->src[0] = a;
    return c;
}

/* bias_add: (MxN) + (1xN) -> (MxN), broadcasts bias across rows */
static Tensor* fwd_bias_add(Tensor* mat, Tensor* bias) {
    Tensor* c = t_new(mat->rows, mat->cols, mat->requires_grad || bias->requires_grad);
    for (int i = 0; i < mat->rows; i++)
        for (int j = 0; j < mat->cols; j++)
            c->data[i*mat->cols+j] = mat->data[i*mat->cols+j] + bias->data[j];
    c->op = OP_BIAS_ADD; c->src[0] = mat; c->src[1] = bias;
    return c;
}

/* sum: returns 1x1 tensor (keeps autograd chain alive) */
static Tensor* fwd_sum(Tensor* a) {
    Tensor* c = t_new(1, 1, a->requires_grad);
    float s = 0; for (int i = 0; i < a->size; i++) s += a->data[i];
    c->data[0] = s;
    c->op = OP_SUM; c->src[0] = a;
    return c;
}

/* ==================================================================
 * Backward Pass
 * ================================================================== */

static void backward(Tensor* t) {
    if (!t || t->op == OP_NONE || t->_visited) return;
    t->_visited = 1;
    Tensor* a = t->src[0];
    Tensor* b = t->src[1];

    switch (t->op) {
    case OP_ADD:
        if (a->requires_grad) { t_ensure_grad(a); for (int i=0;i<a->size;i++) a->grad[i]+=t->grad[i]; }
        if (b&&b->requires_grad) { t_ensure_grad(b); for (int i=0;i<b->size;i++) b->grad[i]+=t->grad[i]; }
        break;
    case OP_SUB:
        if (a->requires_grad) { t_ensure_grad(a); for (int i=0;i<a->size;i++) a->grad[i]+=t->grad[i]; }
        if (b&&b->requires_grad) { t_ensure_grad(b); for (int i=0;i<b->size;i++) b->grad[i]-=t->grad[i]; }
        break;
    case OP_MUL:
        if (a->requires_grad) { t_ensure_grad(a); for (int i=0;i<a->size;i++) a->grad[i]+=t->grad[i]*b->data[i]; }
        if (b&&b->requires_grad) { t_ensure_grad(b); for (int i=0;i<b->size;i++) b->grad[i]+=t->grad[i]*a->data[i]; }
        break;
    case OP_MATMUL: {
        int M=a->rows, K=a->cols, N=b->cols;
        if (a->requires_grad) {
            t_ensure_grad(a);
            for (int i=0;i<M;i++) for (int k=0;k<K;k++) {
                float s=0; for (int j=0;j<N;j++) s+=t->grad[i*N+j]*b->data[k*N+j];
                a->grad[i*K+k]+=s;
            }
        }
        if (b&&b->requires_grad) {
            t_ensure_grad(b);
            for (int k=0;k<K;k++) for (int j=0;j<N;j++) {
                float s=0; for (int i=0;i<M;i++) s+=a->data[i*K+k]*t->grad[i*N+j];
                b->grad[k*N+j]+=s;
            }
        }
        break;
    }
    case OP_RELU:
        if (a->requires_grad) {
            t_ensure_grad(a);
            for (int i=0;i<a->size;i++) a->grad[i]+=(a->data[i]>0?1.0f:0.0f)*t->grad[i];
        }
        break;
    case OP_SIGMOID:
        if (a->requires_grad) {
            t_ensure_grad(a);
            for (int i=0;i<a->size;i++) {
                float s=t->data[i]; a->grad[i]+=t->grad[i]*s*(1.0f-s);
            }
        }
        break;
    case OP_SUM:
        if (a->requires_grad) {
            t_ensure_grad(a);
            float g=t->grad[0];
            for (int i=0;i<a->size;i++) a->grad[i]+=g;
        }
        break;
    case OP_BIAS_ADD:
        if (a->requires_grad) {
            t_ensure_grad(a);
            for (int i=0;i<a->size;i++) a->grad[i]+=t->grad[i];
        }
        if (b&&b->requires_grad) {
            t_ensure_grad(b);
            for (int i=0;i<a->rows;i++)
                for (int j=0;j<a->cols;j++) b->grad[j]+=t->grad[i*a->cols+j];
        }
        break;
    case OP_TANH:
        if (a->requires_grad) {
            t_ensure_grad(a);
            for (int i=0;i<a->size;i++) {
                float th=t->data[i]; a->grad[i]+=t->grad[i]*(1.0f-th*th);
            }
        }
        break;
    case OP_SCALE_DIV:
        if (a->requires_grad) {
            t_ensure_grad(a);
            float inv=1.0f/t->scalar;
            for (int i=0;i<a->size;i++) a->grad[i]+=t->grad[i]*inv;
        }
        break;
    }

    backward(a);
    if (b) backward(b);
}

static void clear_visited(Tensor* t) {
    if (!t || !t->_visited) return;
    t->_visited = 0;
    clear_visited(t->src[0]);
    clear_visited(t->src[1]);
}

/* ==================================================================
 * FFI Functions
 * ================================================================== */

/* Creation */
static RizPluginValue fn_zeros(RizPluginValue* a,int n){return wrap(t_new((int)a[0].as.integer,(int)a[1].as.integer,0));}
static RizPluginValue fn_ones(RizPluginValue* a,int n){
    Tensor*t=t_new((int)a[0].as.integer,(int)a[1].as.integer,0);
    for(int i=0;i<t->size;i++)t->data[i]=1;return wrap(t);
}
static RizPluginValue fn_rand(RizPluginValue* a,int n){
    Tensor*t=t_new((int)a[0].as.integer,(int)a[1].as.integer,0);
    for(int i=0;i<t->size;i++)t->data[i]=(float)rand()/(float)RAND_MAX;return wrap(t);
}
static RizPluginValue fn_param(RizPluginValue* a,int n){
    Tensor*t=t_new((int)a[0].as.integer,(int)a[1].as.integer,1);
    float sc=sqrtf(2.0f/(float)(t->rows+t->cols));
    for(int i=0;i<t->size;i++)t->data[i]=((float)rand()/(float)RAND_MAX-0.5f)*2.0f*sc;
    return wrap(t);
}

/* Data access */
static RizPluginValue fn_set(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);int r=(int)a[1].as.integer,c=(int)a[2].as.integer;
    float v=(float)(a[3].type==VAL_FLOAT?a[3].as.floating:(double)a[3].as.integer);
    if(t&&r>=0&&r<t->rows&&c>=0&&c<t->cols)t->data[r*t->cols+c]=v;
    return G.make_none();
}
static RizPluginValue fn_get(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);int r=(int)a[1].as.integer,c=(int)a[2].as.integer;
    if(!t||r<0||r>=t->rows||c<0||c>=t->cols)return G.make_float(0);
    return G.make_float((double)t->data[r*t->cols+c]);
}

/* Arithmetic */
static RizPluginValue fn_add(RizPluginValue* a,int n){
    Tensor*A=as_t(a[0]),*B=as_t(a[1]);if(!A||!B)return G.make_none();return wrap(fwd_add(A,B));
}
static RizPluginValue fn_sub(RizPluginValue* a,int n){
    Tensor*A=as_t(a[0]),*B=as_t(a[1]);if(!A||!B)return G.make_none();return wrap(fwd_sub(A,B));
}
static RizPluginValue fn_mul(RizPluginValue* a,int n){
    Tensor*A=as_t(a[0]),*B=as_t(a[1]);if(!A||!B)return G.make_none();return wrap(fwd_mul(A,B));
}
static RizPluginValue fn_matmul(RizPluginValue* a,int n){
    Tensor*A=as_t(a[0]),*B=as_t(a[1]);
    if(!A||!B||A->cols!=B->rows){fprintf(stderr,"[tensor] matmul shape error\n");return G.make_none();}
    return wrap(fwd_matmul(A,B));
}

/* Activations */
static RizPluginValue fn_relu(RizPluginValue* a,int n){Tensor*A=as_t(a[0]);if(!A)return G.make_none();return wrap(fwd_relu(A));}
static RizPluginValue fn_sigmoid(RizPluginValue* a,int n){Tensor*A=as_t(a[0]);if(!A)return G.make_none();return wrap(fwd_sigmoid(A));}

/* Broadcast */
static RizPluginValue fn_bias_add(RizPluginValue* a,int n){
    Tensor*M=as_t(a[0]),*B=as_t(a[1]);if(!M||!B)return G.make_none();return wrap(fwd_bias_add(M,B));
}

/* Reduction (returns 1x1 Tensor to keep autograd alive) */
static RizPluginValue fn_sum_t(RizPluginValue* a,int n){
    Tensor*A=as_t(a[0]);if(!A)return G.make_none();return wrap(fwd_sum(A));
}

/* Scalar extraction from 1x1 tensor */
static RizPluginValue fn_item(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);if(!t)return G.make_float(0);return G.make_float((double)t->data[0]);
}

/* Autograd */
static RizPluginValue fn_backward(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);if(!t)return G.make_none();
    t_ensure_grad(t);
    for(int i=0;i<t->size;i++)t->grad[i]=1.0f;
    backward(t);
    clear_visited(t);
    return G.make_none();
}

static RizPluginValue fn_zero_grad(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);
    if(t&&t->grad)memset(t->grad,0,sizeof(float)*t->size);
    t->op=OP_NONE; t->src[0]=NULL; t->src[1]=NULL;
    return G.make_none();
}

/* Optimizer */
static RizPluginValue fn_sgd(RizPluginValue* a,int n){
    Tensor*p=as_t(a[0]);
    double lr=a[1].type==VAL_FLOAT?a[1].as.floating:(double)a[1].as.integer;
    if(!p||!p->grad)return G.make_none();
    for(int i=0;i<p->size;i++)p->data[i]-=(float)lr*p->grad[i];
    return G.make_none();
}

/* Inspection */
static RizPluginValue fn_shape(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);if(!t)return G.make_none();
    RizPluginValue l=G.make_list();
    G.list_append(l.as.list,G.make_int(t->rows));
    G.list_append(l.as.list,G.make_int(t->cols));return l;
}
static RizPluginValue fn_print(RizPluginValue* a,int n){
    Tensor*t=as_t(a[0]);if(!t){printf("Tensor(null)\n");return G.make_none();}
    printf("Tensor(%dx%d) [\n",t->rows,t->cols);
    int mr=t->rows<10?t->rows:10;
    for(int i=0;i<mr;i++){
        printf("  [");int mc=t->cols<10?t->cols:10;
        for(int j=0;j<mc;j++){if(j>0)printf(", ");printf("%8.4f",t->data[i*t->cols+j]);}
        if(t->cols>10)printf(", ...");printf("]\n");
    }
    if(t->rows>10)printf("  ... (%d more rows)\n",t->rows-10);
    printf("]\n");return G.make_none();
}

/* Data generators (create training data in C for speed) */
static RizPluginValue fn_make_sine(RizPluginValue* a, int n) {
    int samples = (int)a[0].as.integer;
    /* Returns a list: [X_tensor, Y_tensor] */
    Tensor* X = t_new(samples, 1, 0);
    Tensor* Y = t_new(samples, 1, 0);
    for (int i = 0; i < samples; i++) {
        float x = -3.14159f + 6.28318f * (float)i / (float)(samples - 1);
        X->data[i] = x;
        Y->data[i] = sinf(x);
    }
    RizPluginValue list = G.make_list();
    G.list_append(list.as.list, wrap(X));
    G.list_append(list.as.list, wrap(Y));
    return list;
}

/* tanh activation with autograd */
static Tensor* fwd_tanh(Tensor* a) {
    Tensor* c = t_new(a->rows, a->cols, a->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = tanhf(a->data[i]);
    c->op = 9; /* OP_TANH */
    c->src[0] = a;
    return c;
}
static RizPluginValue fn_tanh(RizPluginValue* a, int n) {
    Tensor* A = as_t(a[0]); if (!A) return G.make_none(); return wrap(fwd_tanh(A));
}

/* scale_div: divide all elements by a scalar (for mean loss) */
static RizPluginValue fn_scale_div(RizPluginValue* a, int n) {
    Tensor* A = as_t(a[0]);
    double d = a[1].type == VAL_FLOAT ? a[1].as.floating : (double)a[1].as.integer;
    if (!A || d == 0) return G.make_none();
    Tensor* c = t_new(A->rows, A->cols, A->requires_grad);
    for (int i = 0; i < c->size; i++) c->data[i] = A->data[i] / (float)d;
    c->op = 10; /* OP_SCALE_DIV */
    c->src[0] = A; c->scalar = (float)d; /* store divisor for backward */
    return wrap(c);
}

/* ---- Entry point ---- */

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;
    srand((unsigned)time(NULL));

    api->register_fn(api->interp, "tensor_zeros",    fn_zeros,    2);
    api->register_fn(api->interp, "tensor_ones",     fn_ones,     2);
    api->register_fn(api->interp, "tensor_rand",     fn_rand,     2);
    api->register_fn(api->interp, "tensor_param",    fn_param,    2);
    api->register_fn(api->interp, "tensor_set",      fn_set,      4);
    api->register_fn(api->interp, "tensor_get",      fn_get,      3);
    api->register_fn(api->interp, "tensor_add",      fn_add,      2);
    api->register_fn(api->interp, "tensor_sub",      fn_sub,      2);
    api->register_fn(api->interp, "tensor_mul",      fn_mul,      2);
    api->register_fn(api->interp, "tensor_matmul",   fn_matmul,   2);
    api->register_fn(api->interp, "tensor_relu",     fn_relu,     1);
    api->register_fn(api->interp, "tensor_sigmoid",  fn_sigmoid,  1);
    api->register_fn(api->interp, "tensor_tanh",     fn_tanh,     1);
    api->register_fn(api->interp, "tensor_bias_add", fn_bias_add, 2);
    api->register_fn(api->interp, "tensor_sum",      fn_sum_t,    1);
    api->register_fn(api->interp, "tensor_item",     fn_item,     1);
    api->register_fn(api->interp, "tensor_backward", fn_backward, 1);
    api->register_fn(api->interp, "tensor_zero_grad",fn_zero_grad,1);
    api->register_fn(api->interp, "tensor_sgd",      fn_sgd,      2);
    api->register_fn(api->interp, "tensor_shape",    fn_shape,    1);
    api->register_fn(api->interp, "tensor_print",    fn_print,    1);
    api->register_fn(api->interp, "tensor_make_sine",fn_make_sine,1);
    api->register_fn(api->interp, "tensor_scale_div",fn_scale_div,2);
}

