/*
 * Riz Programming Language
 * chunk.h — Register-based bytecode chunk (Phase 5.1)
 *
 * Instructions are 32-bit words with the following formats:
 *
 *   ABC:   | OP (8) | A (8) | B (8) | C (8) |
 *   ABx:   | OP (8) | A (8) |    Bx (16)    |
 *   AsBx:  | OP (8) | A (8) |   sBx (16)    |  (signed: value - 32768)
 */

#ifndef RIZ_CHUNK_H
#define RIZ_CHUNK_H

#include "common.h"
#include "value.h"

/* ─── 32-bit Instruction ──────────────────────────────── */
typedef uint32_t RizInstr;

/* ─── Instruction Encoding ────────────────────────────── */
#define RIZ_ABC(op,a,b,c)   ((uint32_t)(op) | ((uint32_t)(a)<<8) | ((uint32_t)(b)<<16) | ((uint32_t)(c)<<24))
#define RIZ_ABx(op,a,bx)    ((uint32_t)(op) | ((uint32_t)(a)<<8) | ((uint32_t)((bx)&0xFFFF)<<16))
#define RIZ_AsBx(op,a,sbx)  RIZ_ABx(op, a, (uint16_t)((sbx)+32768))

/* ─── Instruction Decoding ────────────────────────────── */
#define RIZ_OP(i)   ((uint8_t)((i) & 0xFF))
#define RIZ_A(i)    ((uint8_t)(((i) >> 8) & 0xFF))
#define RIZ_B(i)    ((uint8_t)(((i) >> 16) & 0xFF))
#define RIZ_C(i)    ((uint8_t)(((i) >> 24) & 0xFF))
#define RIZ_Bx(i)   ((uint16_t)(((i) >> 16) & 0xFFFF))
#define RIZ_sBx(i)  ((int32_t)RIZ_Bx(i) - 32768)

/* ─── Opcodes (Register Machine) ──────────────────────── */
typedef enum {
    /* Load */
    OP_LOADK,       /* A Bx     R[A] = K[Bx]                           */
    OP_LOADNIL,     /* A        R[A] = none                             */
    OP_LOADBOOL,    /* A B      R[A] = (bool)B                          */
    OP_MOVE,        /* A B      R[A] = R[B]                             */

    /* Arithmetic  (A = dest, B = lhs, C = rhs) */
    OP_ADD,         /* A B C    R[A] = R[B] + R[C]                      */
    OP_SUB,         /* A B C    R[A] = R[B] - R[C]                      */
    OP_MUL,         /* A B C    R[A] = R[B] * R[C]                      */
    OP_DIV,         /* A B C    R[A] = R[B] / R[C]                      */
    OP_MOD,         /* A B C    R[A] = R[B] % R[C]                      */
    OP_IDIV,        /* A B C    R[A] = R[B] // R[C]                     */
    OP_POW,         /* A B C    R[A] = R[B] ** R[C]                     */
    OP_NEG,         /* A B      R[A] = -R[B]                            */
    OP_NOT,         /* A B      R[A] = !R[B]                            */

    /* Comparison (tests, skip next instruction if condition != A) */
    OP_EQ,          /* A B C    if (R[B] == R[C]) != A then skip next   */
    OP_LT,          /* A B C    if (R[B] <  R[C]) != A then skip next   */
    OP_LE,          /* A B C    if (R[B] <= R[C]) != A then skip next   */

    /* Control flow */
    OP_JMP,         /* AsBx     PC += sBx                               */
    OP_TEST,        /* A C      if bool(R[A]) != C then skip next       */

    /* Globals */
    OP_GETGLOBAL,   /* A Bx     R[A] = globals[K[Bx]]                   */
    OP_SETGLOBAL,   /* A Bx     globals[K[Bx]] = R[A]                   */

    /* Functions */
    OP_CALL,        /* A B C    R[A]..R[A+C-2] = R[A](R[A+1]..R[A+B-1])*/
    OP_TAILCALL,    /* A B      return R[A](R[A+1]..R[A+B-1])           */
    OP_RETURN,      /* A B      return R[A]..R[A+B-2]                   */

    /* I/O */
    OP_PRINT,       /* A B      print R[A]..R[A+B-1]                    */

    /* Halt */
    OP_HALT,        /*          stop execution                          */

    OP_COUNT        /* sentinel — number of opcodes for dispatch table  */
} OpCode;

/* ─── Chunk ───────────────────────────────────────────── */
typedef struct {
    RizInstr* code;         /* 32-bit instruction stream */
    int       count;
    int       capacity;

    int*      lines;        /* source line per instruction */

    RizValue* constants;    /* constant pool */
    int       const_count;
    int       const_cap;
} Chunk;

/* ─── API ─────────────────────────────────────────────── */
void  chunk_init(Chunk* chunk);
void  chunk_free(Chunk* chunk);
int   chunk_emit(Chunk* chunk, RizInstr instr, int line);
int   chunk_add_constant(Chunk* chunk, RizValue value);

#endif /* RIZ_CHUNK_H */
