/*
 * Riz Programming Language
 * compiler.h — AST-to-Bytecode compiler interface (Phase 5)
 */

#ifndef RIZ_COMPILER_H
#define RIZ_COMPILER_H

#include "ast.h"
#include "chunk.h"

/* Compile an AST program into a bytecode chunk.
 * Returns true on success, false on compile error. */
bool compiler_compile(ASTNode* program, Chunk* chunk);

#endif /* RIZ_COMPILER_H */
