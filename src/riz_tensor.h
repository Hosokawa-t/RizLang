/*
 * Riz Programming Language
 * riz_tensor.h -- Universal Tensor API for Riz plugins
 *
 * Both the GGML and PyTorch backends expose the same function names
 * to the Riz interpreter through this common interface.
 *
 * Each backend implements:
 *   tensor_zeros(rows, cols)      -> Tensor
 *   tensor_ones(rows, cols)       -> Tensor
 *   tensor_rand(rows, cols)       -> Tensor
 *   tensor_add(a, b)             -> Tensor
 *   tensor_mul(a, b)             -> Tensor (element-wise)
 *   tensor_matmul(a, b)          -> Tensor
 *   tensor_sum(a)                -> float
 *   tensor_print(a)              -> none
 *   tensor_shape(a)              -> list [rows, cols]
 *   tensor_free(a)               -> none
 *
 * To switch backends, simply change which DLL is loaded:
 *   import_native "plugin_ggml.dll"    # lightweight C backend
 *   import_native "plugin_torch.dll"   # full PyTorch backend
 */

#ifndef RIZ_TENSOR_H
#define RIZ_TENSOR_H

/* Shared Tensor structure used by the dummy and GGML backends.
 * The PyTorch backend wraps torch::Tensor directly. */
typedef struct {
    float* data;
    int    rows;
    int    cols;
    int    size;   /* rows * cols */
} RizTensor;

#endif /* RIZ_TENSOR_H */
