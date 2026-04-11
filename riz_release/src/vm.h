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

/* ─── Configuration ───────────────────────────────────── */
#define RIZ_REG_MAX     256
#define RIZ_FRAMES_MAX  64

/* ─── Call Frame ──────────────────────────────────────── */
typedef struct {
    Chunk*    chunk;
    RizInstr* ip;           /* instruction pointer */
    RizValue* regs;         /* base of register window for this frame */
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
