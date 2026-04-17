/*
 * Riz Programming Language
 * vm.h — Register-based virtual machine (Phase 5.1)
 *
 * Uses 256 registers per call frame instead of a value stack.
 * Dispatch via computed goto (GCC/Clang) for optimal branch prediction.
 */

#ifndef RIZ_VM_H
#define RIZ_VM_H

#include "chunk.h"
#include "value.h"
#include "environment.h"
#include <setjmp.h>

/* ─── Configuration ───────────────────────────────────── */
#define RIZ_REG_MAX     256
#define RIZ_FRAMES_MAX  64

/* ─── Call Frame ──────────────────────────────────────── */
typedef struct {
    Chunk*      chunk;
    RizInstr*   ip;
    RizValue*   regs;
    int         reg_base;
    int         window_size;

    uint8_t     caller_result_reg;
    RizInstr*   caller_resume_ip;
    Chunk*      caller_chunk;
    RizValue*   caller_regs;
    /* If non-NULL, chunk is freed when this frame returns (OP_IMPORT submodule). */
    Chunk*      owned_import_chunk;
} CallFrame;

/* ─── Virtual Machine ─────────────────────────────────── */
typedef struct {
    /* Register file: large flat array, frames index into it */
    RizValue    registers[RIZ_FRAMES_MAX * RIZ_REG_MAX];
    int         reg_top;    /* next free register slot */

    CallFrame   frames[RIZ_FRAMES_MAX];
    int         frame_count;

    Environment* globals;
    bool         had_error;

    /* Resolved paths already loaded via OP_IMPORT (VM import graph). */
    char**      imported_paths;
    int         imported_count;

    /* Native plugins loaded via OP_IMPORT_NATIVE (FreeLibrary/dlclose on vm_free). */
    void**      native_libs;
    int         native_lib_count;
    jmp_buf     panic_jmp;
} RizVM;

/* ─── Result ──────────────────────────────────────────── */
typedef enum {
    VM_OK,
    VM_COMPILE_ERROR,
    VM_RUNTIME_ERROR,
} VMResult;

/* ─── API ─────────────────────────────────────────────── */
void     vm_init(RizVM* vm);
void     vm_free(RizVM* vm);
VMResult vm_execute(RizVM* vm, Chunk* chunk);

#endif /* RIZ_VM_H */
