/*
 * Riz Programming Language
 * chunk.c — Bytecode chunk implementation (32-bit instructions)
 */

#include "chunk.h"

void chunk_init(Chunk* chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->lines = NULL;
    chunk->constants = NULL;
    chunk->const_count = 0;
    chunk->const_cap = 0;
    chunk->stack_slots = 0;
}

void chunk_free(Chunk* chunk) {
    for (int i = 0; i < chunk->const_count; i++) {
        riz_value_free(&chunk->constants[i]);
    }
    free(chunk->constants);
    free(chunk->code);
    free(chunk->lines);
    chunk_init(chunk);
}

int chunk_emit(Chunk* chunk, RizInstr instr, int line) {
    if (chunk->count >= chunk->capacity) {
        int nc = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code  = RIZ_GROW_ARRAY(RizInstr, chunk->code,  chunk->capacity, nc);
        chunk->lines = RIZ_GROW_ARRAY(int,      chunk->lines, chunk->capacity, nc);
        chunk->capacity = nc;
    }
    chunk->code[chunk->count]  = instr;
    chunk->lines[chunk->count] = line;
    return chunk->count++;
}

int chunk_add_constant(Chunk* chunk, RizValue value) {
    if (chunk->const_count >= chunk->const_cap) {
        int nc = chunk->const_cap < 8 ? 8 : chunk->const_cap * 2;
        chunk->constants = RIZ_GROW_ARRAY(RizValue, chunk->constants, chunk->const_cap, nc);
        chunk->const_cap = nc;
    }
    chunk->constants[chunk->const_count] = value;
    return chunk->const_count++;
}
