/*
 * Riz Tensor Plugin -- PyTorch Backend (Phase 6.5)
 *
 * Uses libtorch (PyTorch C++ API) as the backend engine.
 * This provides state-of-the-art GPU acceleration (CUDA) and a
 * robust Autograd engine.
 */

#include "riz_plugin.h"
#include <torch/torch.h>
#include <iostream>
#include <memory>
#include <vector>
#include <string>

extern "C" {

static RizPluginAPI G;

#define RIZ_TRY try {
#define RIZ_CATCH(fn_name) } catch (const std::exception& e) { \
    if (G.panic) { \
        std::string full_msg = e.what(); \
        size_t newline = full_msg.find('\n'); \
        std::string short_msg = (newline != std::string::npos) ? full_msg.substr(0, newline) : full_msg; \
        std::string msg = std::string("PyTorch Error in '") + fn_name + "':\n    " + short_msg; \
        G.panic(G.interp, msg.c_str()); \
    } \
    return G.make_none(); \
}

// Wrapper for torch::Tensor
struct TorchTensor {
    torch::Tensor t;
};

static TorchTensor* as_tt(RizPluginValue v) {
    return (TorchTensor*)G.get_native_ptr(v);
}

static void tt_dtor(void* p) {
    TorchTensor* tt = (TorchTensor*)p;
    if (tt) {
        delete tt;
    }
}

static RizPluginValue wrap(torch::Tensor t) {
    TorchTensor* tt = new TorchTensor{t};
    return G.make_native_ptr(tt, "Tensor", tt_dtor);
}

static torch::Device get_device() {
    if (torch::cuda::is_available()) {
        return torch::Device(torch::kCUDA);
    }
    return torch::Device(torch::kCPU);
}

static RizPluginValue fn_is_cuda(RizPluginValue* a, int n) {
    return G.make_bool(torch::cuda::is_available());
}

static RizPluginValue fn_device_name(RizPluginValue* a, int n) {
    if (torch::cuda::is_available()) {
        return G.make_string("cuda");
    }
    return G.make_string("cpu");
}

// -------------------------------------------------------------
// Creation
// -------------------------------------------------------------

static RizPluginValue fn_zeros(RizPluginValue* a, int n) {
    int r = (int)a[0].as.integer, c = (int)a[1].as.integer;
    return wrap(torch::zeros({r, c}, torch::kFloat32).to(get_device()));
}

static RizPluginValue fn_ones(RizPluginValue* a, int n) {
    int r = (int)a[0].as.integer, c = (int)a[1].as.integer;
    return wrap(torch::ones({r, c}, torch::kFloat32).to(get_device()));
}

static RizPluginValue fn_rand(RizPluginValue* a, int n) {
    int r = (int)a[0].as.integer, c = (int)a[1].as.integer;
    return wrap(torch::rand({r, c}, torch::kFloat32).to(get_device()));
}

static RizPluginValue fn_param(RizPluginValue* a, int n) {
    int r = (int)a[0].as.integer, c = (int)a[1].as.integer;
    float scale = std::sqrt(2.0f / (float)(r + c));
    auto t = (torch::rand({r, c}, torch::kFloat32) - 0.5f) * 2.0f * scale;
    t = t.to(get_device());
    t.set_requires_grad(true);
    return wrap(t);
}

// -------------------------------------------------------------
// Data Access
// -------------------------------------------------------------

static RizPluginValue fn_set(RizPluginValue* a, int n) {
    RIZ_TRY
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) return G.make_none();
    int r = (int)a[1].as.integer, c = (int)a[2].as.integer;
    float v = (float)(a[3].type == VAL_FLOAT ? a[3].as.floating : (double)a[3].as.integer);
    
    // We must do this without autograd complaining
    torch::NoGradGuard no_grad;
    tt->t[r][c] = v;
    return G.make_none();
    RIZ_CATCH("tensor_set")
}

static RizPluginValue fn_get(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) return G.make_float(0);
    int r = (int)a[1].as.integer, c = (int)a[2].as.integer;
    float v = tt->t[r][c].item<float>();
    return G.make_float(v);
}

// -------------------------------------------------------------
// Arithmetic
// -------------------------------------------------------------

static RizPluginValue fn_add(RizPluginValue* a, int n) {
    RIZ_TRY return wrap(as_tt(a[0])->t + as_tt(a[1])->t); RIZ_CATCH("tensor_add")
}

static RizPluginValue fn_sub(RizPluginValue* a, int n) {
    RIZ_TRY return wrap(as_tt(a[0])->t - as_tt(a[1])->t); RIZ_CATCH("tensor_sub")
}

static RizPluginValue fn_mul(RizPluginValue* a, int n) {
    RIZ_TRY return wrap(as_tt(a[0])->t * as_tt(a[1])->t); RIZ_CATCH("tensor_mul")
}

static RizPluginValue fn_matmul(RizPluginValue* a, int n) {
    RIZ_TRY return wrap(torch::matmul(as_tt(a[0])->t, as_tt(a[1])->t)); RIZ_CATCH("tensor_matmul")
}

// -------------------------------------------------------------
// Activations
// -------------------------------------------------------------

static RizPluginValue fn_relu(RizPluginValue* a, int n) {
    return wrap(torch::relu(as_tt(a[0])->t));
}

static RizPluginValue fn_sigmoid(RizPluginValue* a, int n) {
    return wrap(torch::sigmoid(as_tt(a[0])->t));
}

static RizPluginValue fn_tanh(RizPluginValue* a, int n) {
    return wrap(torch::tanh(as_tt(a[0])->t));
}

// -------------------------------------------------------------
// Broadcasting & Reduction
// -------------------------------------------------------------

static RizPluginValue fn_bias_add(RizPluginValue* a, int n) {
    return wrap(as_tt(a[0])->t + as_tt(a[1])->t);
}

static RizPluginValue fn_sum_t(RizPluginValue* a, int n) {
    return wrap(torch::sum(as_tt(a[0])->t));
}

static RizPluginValue fn_scale_div(RizPluginValue* a, int n) {
    double d = a[1].type == VAL_FLOAT ? a[1].as.floating : (double)a[1].as.integer;
    return wrap(as_tt(a[0])->t / (float)d);
}

static RizPluginValue fn_item(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) return G.make_float(0);
    return G.make_float(tt->t.item<float>());
}

// -------------------------------------------------------------
// Autograd & Optimizer
// -------------------------------------------------------------

static RizPluginValue fn_backward(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) return G.make_none();
    tt->t.backward();
    return G.make_none();
}

static RizPluginValue fn_zero_grad(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (tt && tt->t.grad().defined()) {
        tt->t.grad().zero_();
    }
    return G.make_none();
}

static RizPluginValue fn_sgd(RizPluginValue* a, int n) {
    TorchTensor* p = as_tt(a[0]);
    double lr = a[1].type == VAL_FLOAT ? a[1].as.floating : (double)a[1].as.integer;
    if (p && p->t.grad().defined()) {
        torch::NoGradGuard no_grad;
        p->t -= p->t.grad() * (float)lr;
    }
    return G.make_none();
}

// -------------------------------------------------------------
// Data generation
// -------------------------------------------------------------

static RizPluginValue fn_make_sine(RizPluginValue* a, int n) {
    int samples = (int)a[0].as.integer;
    auto x = torch::linspace(-3.14159f, 3.14159f, samples).reshape({samples, 1});
    auto y = torch::sin(x);
    x = x.to(get_device());
    y = y.to(get_device());
    
    RizPluginValue list = G.make_list();
    G.list_append(list, wrap(x));
    G.list_append(list, wrap(y));
    return list;
}

// -------------------------------------------------------------
// Inspection
// -------------------------------------------------------------

static RizPluginValue fn_shape(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) return G.make_none();
    RizPluginValue l = G.make_list();
    for (int i = 0; i < tt->t.dim(); i++) {
        G.list_append(l, G.make_int(tt->t.size(i)));
    }
    return l;
}

static RizPluginValue fn_print(RizPluginValue* a, int n) {
    TorchTensor* tt = as_tt(a[0]);
    if (!tt) { std::cout << "Tensor(null)\n"; return G.make_none(); }
    std::cout << tt->t << std::endl;
    return G.make_none();
}

// -------------------------------------------------------------
// Entry Point
// -------------------------------------------------------------

#ifdef _WIN32
#include <windows.h>
#endif

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;
    
#ifdef _WIN32
    // Force load CUDA backend DLLs to ensure initialization
    LoadLibraryA("c10_cuda.dll");
    LoadLibraryA("torch_cuda.dll");
#endif

    printf("[PyTorch Plugin] Initializing... (CUDA count: %d)\n", (int)torch::cuda::device_count());
    auto device = get_device();
    if (device.is_cuda()) {
        printf("[PyTorch Plugin] Initialized. Using CUDA (%s).\n", device.str().c_str());
    } else {
        printf("[PyTorch Plugin] Initialized. Using CPU.\n");
    }

    api->register_fn(api->interp, "tensor_is_cuda",   fn_is_cuda,   0);
    api->register_fn(api->interp, "tensor_device",    fn_device_name,0);
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
    api->register_fn(api->interp, "tensor_scale_div",fn_scale_div,2);
    api->register_fn(api->interp, "tensor_item",     fn_item,     1);
    api->register_fn(api->interp, "tensor_backward", fn_backward, 1);
    api->register_fn(api->interp, "tensor_zero_grad",fn_zero_grad,1);
    api->register_fn(api->interp, "tensor_sgd",      fn_sgd,      2);
    api->register_fn(api->interp, "tensor_shape",    fn_shape,    1);
    api->register_fn(api->interp, "tensor_print",    fn_print,    1);
    api->register_fn(api->interp, "tensor_make_sine",fn_make_sine,1);
}

} // extern "C"
