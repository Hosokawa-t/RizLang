/*
 * Riz Programming Language
 * compiler.c — AST-to-Register-Bytecode compiler (Phase 5.1)
 *
 * Translates the AST into register-based 32-bit instructions.
 *
 * Register allocation strategy (Lua-style):
 *   - Each local variable occupies a fixed register slot.
 *   - Temporaries are allocated above locals via `alloc_reg()`.
 *   - After an expression is compiled into register R, the register
 *     can be freed by calling `free_reg()` if it's a temporary.
 *   - `compile_expr(node)` returns the register holding the result.
 */

#include "compiler.h"
#include <stdio.h>
#include <string.h>

/* ─── Compiler State ──────────────────────────────────── */
typedef struct {
    Chunk* chunk;
    bool   had_error;

    /* Register allocator */
    int    free_reg;       /* next available register */
    int    max_reg;        /* high water mark */

    /* Local variable table */
    struct {
        char name[64];
        int  reg;          /* register slot for this local */
        int  depth;        /* scope depth */
    } locals[RIZ_MAX_LOCALS];
    int local_count;
    int scope_depth;
} Compiler;

static Compiler C;

/* ─── Register allocator ──────────────────────────────── */

static int alloc_reg(void) {
    int r = C.free_reg++;
    if (C.free_reg > C.max_reg) C.max_reg = C.free_reg;
    if (r >= 255) {
        fprintf(stderr, "[compiler] Register overflow\n");
        C.had_error = true;
    }
    return r;
}

static void free_reg(int r) {
    /* Only free if it's the most recently allocated register */
    if (r == C.free_reg - 1) C.free_reg--;
}

static void free_regs_to(int target) {
    if (C.free_reg > target) C.free_reg = target;
}

/* ─── Emit helpers ────────────────────────────────────── */

static int emit(RizInstr instr, int line) {
    return chunk_emit(C.chunk, instr, line);
}

static int add_const(RizValue value) {
    return chunk_add_constant(C.chunk, value);
}

static int add_string_const(const char* s) {
    return chunk_add_constant(C.chunk, riz_string(s));
}

/* Emit a jump placeholder, return its position for later patching */
static int emit_jmp(int line) {
    return emit(RIZ_AsBx(OP_JMP, 0, 0), line);
}

/* Patch a jump instruction at `pos` to jump to the current position */
static void patch_jmp(int pos) {
    int offset = C.chunk->count - pos - 1;
    C.chunk->code[pos] = RIZ_AsBx(OP_JMP, 0, offset);
}

/* ─── Scope management ────────────────────────────────── */

static void begin_scope(void) { C.scope_depth++; }

static void end_scope(void) {
    C.scope_depth--;
    while (C.local_count > 0 && C.locals[C.local_count - 1].depth > C.scope_depth) {
        C.local_count--;
        C.free_reg = C.locals[C.local_count].reg; /* reclaim register */
    }
}

static int resolve_local(const char* name) {
    for (int i = C.local_count - 1; i >= 0; i--) {
        if (strcmp(C.locals[i].name, name) == 0)
            return C.locals[i].reg;
    }
    return -1;
}

static int add_local(const char* name) {
    int reg = alloc_reg();
    strncpy(C.locals[C.local_count].name, name, 63);
    C.locals[C.local_count].name[63] = '\0';
    C.locals[C.local_count].reg = reg;
    C.locals[C.local_count].depth = C.scope_depth;
    C.local_count++;
    return reg;
}

/* ─── Forward decl ────────────────────────────────────── */
static int  compile_expr(ASTNode* node);
static void compile_into(ASTNode* node, int dest);
static void compile_stmt(ASTNode* node);

/* ─── Expression compilation ──────────────────────────── */
/*
 * compile_expr(node) → int reg
 *   Compiles the expression and returns the register it lives in.
 *   The register may be a temporary (can be freed) or a local (don't free).
 */
static int compile_expr(ASTNode* node) {
    if (!node) return 0;
    int line = node->line;

    switch (node->type) {
        case NODE_INT_LIT: {
            int r = alloc_reg();
            int k = add_const(riz_int(node->as.int_lit.value));
            emit(RIZ_ABx(OP_LOADK, r, k), line);
            return r;
        }
        case NODE_FLOAT_LIT: {
            int r = alloc_reg();
            int k = add_const(riz_float(node->as.float_lit.value));
            emit(RIZ_ABx(OP_LOADK, r, k), line);
            return r;
        }
        case NODE_STRING_LIT: {
            int r = alloc_reg();
            int k = add_const(riz_string(node->as.string_lit.value));
            emit(RIZ_ABx(OP_LOADK, r, k), line);
            return r;
        }
        case NODE_BOOL_LIT: {
            int r = alloc_reg();
            emit(RIZ_ABC(OP_LOADBOOL, r, node->as.bool_lit.value ? 1 : 0, 0), line);
            return r;
        }
        case NODE_NONE_LIT: {
            int r = alloc_reg();
            emit(RIZ_ABC(OP_LOADNIL, r, 0, 0), line);
            return r;
        }

        case NODE_IDENTIFIER: {
            const char* name = node->as.identifier.name;
            int slot = resolve_local(name);
            if (slot >= 0) {
                return slot; /* directly reference the local's register */
            } else {
                int r = alloc_reg();
                int k = add_string_const(name);
                emit(RIZ_ABx(OP_GETGLOBAL, r, k), line);
                return r;
            }
        }

        case NODE_UNARY: {
            int src = compile_expr(node->as.unary.operand);
            int r = alloc_reg();
            switch (node->as.unary.op) {
                case TOK_MINUS: emit(RIZ_ABC(OP_NEG, r, src, 0), line); break;
                case TOK_NOT:   emit(RIZ_ABC(OP_NOT, r, src, 0), line); break;
                default: break;
            }
            free_reg(src);
            return r;
        }

        case NODE_BINARY: {
            /* Compile left and right, then emit op */
            int lhs = compile_expr(node->as.binary.left);
            int rhs = compile_expr(node->as.binary.right);
            int dest = alloc_reg();

            switch (node->as.binary.op) {
                case TOK_PLUS:      emit(RIZ_ABC(OP_ADD,  dest, lhs, rhs), line); break;
                case TOK_MINUS:     emit(RIZ_ABC(OP_SUB,  dest, lhs, rhs), line); break;
                case TOK_STAR:      emit(RIZ_ABC(OP_MUL,  dest, lhs, rhs), line); break;
                case TOK_SLASH:     emit(RIZ_ABC(OP_DIV,  dest, lhs, rhs), line); break;
                case TOK_PERCENT:   emit(RIZ_ABC(OP_MOD,  dest, lhs, rhs), line); break;
                case TOK_FLOOR_DIV: emit(RIZ_ABC(OP_IDIV, dest, lhs, rhs), line); break;
                case TOK_POWER:     emit(RIZ_ABC(OP_POW,  dest, lhs, rhs), line); break;

                /* Comparison: emit test + jmp pattern */
                case TOK_EQ: {
                    emit(RIZ_ABC(OP_EQ, 1, lhs, rhs), line);
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line); /* false */
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line); /* true */
                    patch_jmp(done);
                    break;
                }
                case TOK_NEQ: {
                    emit(RIZ_ABC(OP_EQ, 0, lhs, rhs), line);
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
                    patch_jmp(done);
                    break;
                }
                case TOK_LT: {
                    emit(RIZ_ABC(OP_LT, 1, lhs, rhs), line);
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
                    patch_jmp(done);
                    break;
                }
                case TOK_LTE: {
                    emit(RIZ_ABC(OP_LE, 1, lhs, rhs), line);
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
                    patch_jmp(done);
                    break;
                }
                case TOK_GT: {
                    emit(RIZ_ABC(OP_LT, 1, rhs, lhs), line); /* swap! */
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
                    patch_jmp(done);
                    break;
                }
                case TOK_GTE: {
                    emit(RIZ_ABC(OP_LE, 1, rhs, lhs), line); /* swap! */
                    int skip = emit_jmp(line);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
                    int done = emit_jmp(line);
                    patch_jmp(skip);
                    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
                    patch_jmp(done);
                    break;
                }
                default: break;
            }
            free_reg(rhs);
            free_reg(lhs);
            return dest;
        }

        case NODE_CALL: {
            /* Check for print (special opcode) */
            if (node->as.call.callee->type == NODE_IDENTIFIER &&
                strcmp(node->as.call.callee->as.identifier.name, "print") == 0) {
                int base = C.free_reg;
                for (int i = 0; i < node->as.call.arg_count; i++) {
                    compile_into(node->as.call.args[i], base + i);
                    C.free_reg = base + i + 1;
                }
                emit(RIZ_ABC(OP_PRINT, base, node->as.call.arg_count, 0), line);
                free_regs_to(base);
                int r = alloc_reg();
                emit(RIZ_ABC(OP_LOADNIL, r, 0, 0), line);
                return r;
            }

            /* General function call: R[base] = callee, R[base+1..] = args */
            int base = C.free_reg;
            compile_into(node->as.call.callee, base);
            C.free_reg = base + 1;
            for (int i = 0; i < node->as.call.arg_count; i++) {
                compile_into(node->as.call.args[i], base + 1 + i);
                C.free_reg = base + 2 + i;
            }
            emit(RIZ_ABC(OP_CALL, base, node->as.call.arg_count + 1, 2), line);
            C.free_reg = base + 1; /* result in R[base] */
            return base;
        }

        case NODE_ASSIGN: {
            const char* name = node->as.assign.name;
            int slot = resolve_local(name);
            if (slot >= 0) {
                compile_into(node->as.assign.value, slot);
                return slot;
            } else {
                int r = compile_expr(node->as.assign.value);
                int k = add_string_const(name);
                emit(RIZ_ABx(OP_SETGLOBAL, r, k), line);
                return r;
            }
        }

        case NODE_COMPOUND_ASSIGN: {
            /* x += val => x = x + val */
            const char* name = node->as.compound_assign.name;
            int slot = resolve_local(name);
            int cur;
            if (slot >= 0) {
                cur = slot;
            } else {
                cur = alloc_reg();
                int k = add_string_const(name);
                emit(RIZ_ABx(OP_GETGLOBAL, cur, k), line);
            }
            int rhs = compile_expr(node->as.compound_assign.value);
            int dest = (slot >= 0) ? slot : alloc_reg();
            switch (node->as.compound_assign.op) {
                case TOK_PLUS_ASSIGN:  emit(RIZ_ABC(OP_ADD, dest, cur, rhs), line); break;
                case TOK_MINUS_ASSIGN: emit(RIZ_ABC(OP_SUB, dest, cur, rhs), line); break;
                case TOK_STAR_ASSIGN:  emit(RIZ_ABC(OP_MUL, dest, cur, rhs), line); break;
                case TOK_SLASH_ASSIGN: emit(RIZ_ABC(OP_DIV, dest, cur, rhs), line); break;
                default: break;
            }
            free_reg(rhs);
            if (slot < 0) {
                int k = add_string_const(name);
                emit(RIZ_ABx(OP_SETGLOBAL, dest, k), line);
                free_reg(cur);
            }
            return dest;
        }

        default:
            fprintf(stderr, "[compiler] Unhandled expr node %d at line %d\n", node->type, line);
            C.had_error = true;
            return 0;
    }
}

/* Compile expression, ensuring result is in `dest` */
static void compile_into(ASTNode* node, int dest) {
    int r = compile_expr(node);
    if (r != dest) {
        emit(RIZ_ABC(OP_MOVE, dest, r, 0), node->line);
    }
    free_reg(r);
}

/* ─── Statement compilation ───────────────────────────── */

static void compile_stmt(ASTNode* node) {
    if (!node) return;
    int line = node->line;

    switch (node->type) {
        case NODE_EXPR_STMT: {
            int r = compile_expr(node->as.expr_stmt.expr);
            free_reg(r);
            break;
        }

        case NODE_LET_DECL: {
            if (C.scope_depth > 0) {
                /* Local: allocate a register and keep it */
                int reg = add_local(node->as.let_decl.name);
                compile_into(node->as.let_decl.initializer, reg);
            } else {
                /* Global */
                int r = compile_expr(node->as.let_decl.initializer);
                int k = add_string_const(node->as.let_decl.name);
                emit(RIZ_ABx(OP_SETGLOBAL, r, k), line);
                free_reg(r);
            }
            break;
        }

        case NODE_ASSIGN: {
            const char* name = node->as.assign.name;
            int slot = resolve_local(name);
            if (slot >= 0) {
                compile_into(node->as.assign.value, slot);
            } else {
                int r = compile_expr(node->as.assign.value);
                int k = add_string_const(name);
                emit(RIZ_ABx(OP_SETGLOBAL, r, k), line);
                free_reg(r);
            }
            break;
        }

        case NODE_BLOCK:
            begin_scope();
            for (int i = 0; i < node->as.block.count; i++) {
                compile_stmt(node->as.block.statements[i]);
            }
            end_scope();
            break;

        case NODE_IF_STMT: {
            /* Compile condition */
            int cond_r = compile_expr(node->as.if_stmt.condition);
            emit(RIZ_ABC(OP_TEST, cond_r, 0, 0), line); /* test: skip if false */
            int then_jmp = emit_jmp(line);               /* skip over then-branch */
            free_reg(cond_r);

            /* Then branch */
            compile_stmt(node->as.if_stmt.then_branch);

            if (node->as.if_stmt.else_branch) {
                int else_jmp = emit_jmp(line);
                patch_jmp(then_jmp);
                compile_stmt(node->as.if_stmt.else_branch);
                patch_jmp(else_jmp);
            } else {
                patch_jmp(then_jmp);
            }
            break;
        }

        case NODE_WHILE_STMT: {
            int loop_start = C.chunk->count;

            int cond_r = compile_expr(node->as.while_stmt.condition);
            emit(RIZ_ABC(OP_TEST, cond_r, 0, 0), line);
            int exit_jmp = emit_jmp(line);
            free_reg(cond_r);

            /* Loop body */
            compile_stmt(node->as.while_stmt.body);

            /* Jump back to condition */
            int offset = loop_start - C.chunk->count - 1;
            emit(RIZ_AsBx(OP_JMP, 0, offset), line);

            patch_jmp(exit_jmp);
            break;
        }

        case NODE_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                compile_stmt(node->as.program.declarations[i]);
            }
            break;

        default:
            fprintf(stderr, "[compiler] Unhandled stmt node %d at line %d\n", node->type, line);
            C.had_error = true;
            break;
    }
}

/* ─── Public API ──────────────────────────────────────── */

bool compiler_compile(ASTNode* program, Chunk* chunk) {
    C.chunk = chunk;
    C.had_error = false;
    C.free_reg = 0;
    C.max_reg = 0;
    C.local_count = 0;
    C.scope_depth = 0;

    compile_stmt(program);
    emit(RIZ_ABC(OP_HALT, 0, 0, 0), 0);

    return !C.had_error;
}
