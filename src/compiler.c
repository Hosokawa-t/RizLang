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
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Compiler State ──────────────────────────────────── */

/* Loop break/continue patch list */
typedef struct {
    int* breaks;       /* positions of LOOP_BREAK instructions to patch */
    int  break_count;
    int  break_cap;
    int  continue_target; /* PC of loop increment / condition recheck */
} LoopContext;

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

    /* Loop control (break/continue) */
    LoopContext loops[64];
    int         loop_depth;
} Compiler;

static Compiler C;
static bool inside_fn = false;

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

/* Patch a LOOP_BREAK/LOOP_CONT instruction at `pos` */
static void patch_loop_jmp(int pos, uint8_t op) {
    int offset = C.chunk->count - pos - 1;
    C.chunk->code[pos] = RIZ_AsBx(op, 0, offset);
}

/* ─── Loop context management ─────────────────────────── */

static void push_loop(void) {
    if (C.loop_depth >= 64) {
        fprintf(stderr, "[compiler] Too many nested loops\n");
        C.had_error = true;
        return;
    }
    LoopContext* lc = &C.loops[C.loop_depth++];
    lc->breaks = NULL;
    lc->break_count = 0;
    lc->break_cap = 0;
    lc->continue_target = -1;
}

static void pop_loop(void) {
    if (C.loop_depth <= 0) return;
    LoopContext* lc = &C.loops[--C.loop_depth];
    /* Patch all break jumps to current position */
    for (int i = 0; i < lc->break_count; i++) {
        patch_loop_jmp(lc->breaks[i], OP_LOOP_BREAK);
    }
    free(lc->breaks);
    lc->breaks = NULL;
}

static void add_break_patch(int pos) {
    if (C.loop_depth <= 0) {
        fprintf(stderr, "[compiler] 'break' outside loop\n");
        C.had_error = true;
        return;
    }
    LoopContext* lc = &C.loops[C.loop_depth - 1];
    if (lc->break_count >= lc->break_cap) {
        int nc = lc->break_cap < 8 ? 8 : lc->break_cap * 2;
        lc->breaks = (int*)realloc(lc->breaks, sizeof(int) * nc);
        lc->break_cap = nc;
    }
    lc->breaks[lc->break_count++] = pos;
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
static Chunk* compile_fn_body(ASTNode* fn_decl, int* stack_slots_out, bool* err_out);
static void compile_top_level_fn(ASTNode* node);
static int compile_lt_regs(int lhs, int rhs, int line);
static int emit_len_call(int list_r, int line);

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

        case NODE_LIST_LIT: {
            int n = node->as.list_lit.count;
            if (n > 255) {
                fprintf(stderr, "[compiler] list literal too long for VM (max 255 elements) at line %d\n", line);
                C.had_error = true;
                return 0;
            }
            int base = C.free_reg;
            for (int i = 0; i < n; i++) {
                compile_into(node->as.list_lit.items[i], base + i);
                C.free_reg = base + i + 1;
            }
            int dest = alloc_reg();
            emit(RIZ_ABC(OP_BUILDLIST, dest, base, (uint8_t)n), line);
            return dest;
        }

        case NODE_DICT_LIT: {
            int n = node->as.dict_lit.count;
            if (n > 127) {
                fprintf(stderr, "[compiler] dict literal too large for VM (max 127 entries) at line %d\n", line);
                C.had_error = true;
                return 0;
            }
            int base = C.free_reg;
            for (int i = 0; i < n; i++) {
                /* Key: compile into even slot */
                compile_into(node->as.dict_lit.keys[i], base + i * 2);
                C.free_reg = base + i * 2 + 1;
                /* Value: compile into odd slot */
                compile_into(node->as.dict_lit.values[i], base + i * 2 + 1);
                C.free_reg = base + i * 2 + 2;
            }
            int dest = alloc_reg();
            emit(RIZ_ABC(OP_NEWDICT, dest, base, (uint8_t)n), line);
            return dest;
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
            /* Short-circuit and/or */
            if (node->as.binary.op == TOK_AND) {
                int lhs = compile_expr(node->as.binary.left);
                emit(RIZ_ABC(OP_TEST, lhs, 0, 0), line); /* if false, skip rhs */
                int skip = emit_jmp(line);
                int rhs = compile_expr(node->as.binary.right);
                if (rhs != lhs)
                    emit(RIZ_ABC(OP_MOVE, lhs, rhs, 0), line);
                free_reg(rhs);
                patch_jmp(skip);
                return lhs;
            }
            if (node->as.binary.op == TOK_OR) {
                int lhs = compile_expr(node->as.binary.left);
                emit(RIZ_ABC(OP_TEST, lhs, 0, 1), line); /* if true, skip rhs */
                int skip = emit_jmp(line);
                int rhs = compile_expr(node->as.binary.right);
                if (rhs != lhs)
                    emit(RIZ_ABC(OP_MOVE, lhs, rhs, 0), line);
                free_reg(rhs);
                patch_jmp(skip);
                return lhs;
            }

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

        case NODE_INDEX: {
            int obj_r = compile_expr(node->as.index_expr.object);
            int idx_r = compile_expr(node->as.index_expr.index);
            int dest = alloc_reg();
            emit(RIZ_ABC(OP_GETINDEX, dest, obj_r, idx_r), line);
            free_reg(idx_r);
            free_reg(obj_r);
            return dest;
        }

        case NODE_MEMBER: {
            int obj_r = compile_expr(node->as.member.object);
            int dest = alloc_reg();
            int k = add_string_const(node->as.member.member);
            if (k > 255) {
                fprintf(stderr, "[compiler] too many constants for GETMEMBER at line %d\n", line);
                C.had_error = true;
                return 0;
            }
            emit(RIZ_ABC(OP_GETMEMBER, dest, obj_r, (uint8_t)k), line);
            free_reg(obj_r);
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

/* R[a] < R[b] → bool in new register (same pattern as NODE_BINARY TOK_LT) */
static int compile_lt_regs(int lhs, int rhs, int line) {
    int dest = alloc_reg();
    emit(RIZ_ABC(OP_LT, 1, lhs, rhs), line);
    int skip = emit_jmp(line);
    emit(RIZ_ABC(OP_LOADBOOL, dest, 0, 0), line);
    int done = emit_jmp(line);
    patch_jmp(skip);
    emit(RIZ_ABC(OP_LOADBOOL, dest, 1, 0), line);
    patch_jmp(done);
    return dest;
}

/* len(list_r) → result register (overwrites len fn slot) */
static int emit_len_call(int list_r, int line) {
    int fnr = alloc_reg();
    emit(RIZ_ABx(OP_GETGLOBAL, fnr, add_string_const("len")), line);
    int arg = alloc_reg();
    emit(RIZ_ABC(OP_MOVE, arg, list_r, 0), line);
    if (arg != fnr + 1) {
        fprintf(stderr, "[compiler] internal: len() call needs consecutive arg reg\n");
        C.had_error = true;
        return fnr;
    }
    emit(RIZ_ABC(OP_CALL, fnr, 2, 2), line);
    return fnr;
}

/* ─── User-defined functions (VM bytecode closure) ────── */

static Chunk* compile_fn_body(ASTNode* fn_decl, int* stack_slots_out, bool* err_out) {
    Chunk* ch = (Chunk*)malloc(sizeof(Chunk));
    if (!ch) {
        *err_out = true;
        return NULL;
    }
    chunk_init(ch);

    Compiler saved = C;
    bool was_inside = inside_fn;
    C.chunk = ch;
    C.had_error = false;
    C.free_reg = 0;
    C.max_reg = 0;
    C.local_count = 0;
    C.scope_depth = 0;
    inside_fn = true;

    for (int i = 0; i < fn_decl->as.fn_decl.param_count; i++)
        add_local(fn_decl->as.fn_decl.params[i]);

    ASTNode* body = fn_decl->as.fn_decl.body;
    if (!body) {
        fprintf(stderr, "[compiler] function has no body\n");
        C.had_error = true;
    } else if (body->type == NODE_BLOCK) {
        for (int i = 0; i < body->as.block.count; i++)
            compile_stmt(body->as.block.statements[i]);
    } else {
        compile_stmt(body);
    }

    if (!C.had_error) {
        emit(RIZ_ABC(OP_LOADNIL, 0, 0, 0), fn_decl->line);
        emit(RIZ_ABC(OP_RETURN, 0, 2, 0), fn_decl->line);
    }

    int slots = C.max_reg;
    if (slots < fn_decl->as.fn_decl.param_count)
        slots = fn_decl->as.fn_decl.param_count;
    *stack_slots_out = slots;
    *err_out = C.had_error;

    inside_fn = was_inside;
    C = saved;
    return ch;
}

static void compile_top_level_fn(ASTNode* node) {
    ASTNode* fd = node;
    if (!fd->as.fn_decl.body) {
        fprintf(stderr, "[compiler] function '%s' has no body\n", fd->as.fn_decl.name);
        C.had_error = true;
        return;
    }
    if (fd->as.fn_decl.param_defaults) {
        for (int i = 0; i < fd->as.fn_decl.param_count; i++) {
            if (fd->as.fn_decl.param_defaults[i]) {
                fprintf(stderr, "[compiler] VM: default parameters are not supported yet\n");
                C.had_error = true;
                return;
            }
        }
    }

    int slots;
    bool fn_err;
    Chunk* sub = compile_fn_body(node, &slots, &fn_err);
    if (fn_err || !sub) {
        if (sub) {
            chunk_free(sub);
            free(sub);
        }
        C.had_error = true;
        return;
    }
    sub->stack_slots = slots;

    RizVMClosure* cl = (RizVMClosure*)malloc(sizeof(RizVMClosure));
    if (!cl) {
        chunk_free(sub);
        free(sub);
        C.had_error = true;
        return;
    }
    cl->chunk = sub;
    cl->param_count = fd->as.fn_decl.param_count;
    cl->stack_slots = slots;
    cl->name = riz_strdup(fd->as.fn_decl.name);

    RizValue v = riz_vm_closure_val(cl);
    int k = chunk_add_constant(C.chunk, v);
    int r = alloc_reg();
    emit(RIZ_ABx(OP_LOADK, r, k), node->line);
    int namek = add_string_const(fd->as.fn_decl.name);
    emit(RIZ_ABx(OP_SETGLOBAL, r, namek), node->line);
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
            push_loop();
            int loop_start = C.chunk->count;
            C.loops[C.loop_depth - 1].continue_target = loop_start;

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
            pop_loop();
            break;
        }

        case NODE_FOR_STMT: {
            begin_scope();
            push_loop();
            int list_r = compile_expr(node->as.for_stmt.iterable);
            int idx_r = alloc_reg();
            int k0 = add_const(riz_int(0));
            emit(RIZ_ABx(OP_LOADK, idx_r, k0), line);

            int len_r = emit_len_call(list_r, line);
            if (C.had_error) {
                pop_loop();
                end_scope();
                break;
            }

            int var_slot = add_local(node->as.for_stmt.var_name);

            int loop_start = C.chunk->count;

            int cond_r = compile_lt_regs(idx_r, len_r, line);
            emit(RIZ_ABC(OP_TEST, cond_r, 0, 0), line);
            int exit_jmp = emit_jmp(line);
            free_reg(cond_r);

            emit(RIZ_ABC(OP_GETINDEX, var_slot, list_r, idx_r), line);
            compile_stmt(node->as.for_stmt.body);

            /* Continue target: the increment */
            int incr_pos = C.chunk->count;
            C.loops[C.loop_depth - 1].continue_target = incr_pos;

            int k1 = add_const(riz_int(1));
            int one_r = alloc_reg();
            emit(RIZ_ABx(OP_LOADK, one_r, k1), line);
            emit(RIZ_ABC(OP_ADD, idx_r, idx_r, one_r), line);
            free_reg(one_r);

            int offset = loop_start - C.chunk->count - 1;
            emit(RIZ_AsBx(OP_JMP, 0, offset), line);

            patch_jmp(exit_jmp);
            pop_loop();
            end_scope();
            break;
        }

        case NODE_RETURN_STMT:
            if (!inside_fn) {
                fprintf(stderr, "[compiler] 'return' outside function (VM)\n");
                C.had_error = true;
                break;
            }
            {
                ASTNode* val = node->as.return_stmt.value;
                if (!val) {
                    emit(RIZ_ABC(OP_LOADNIL, 0, 0, 0), line);
                } else {
                    int r = compile_expr(val);
                    if (r != 0)
                        emit(RIZ_ABC(OP_MOVE, 0, r, 0), line);
                }
                emit(RIZ_ABC(OP_RETURN, 0, 2, 0), line);
            }
            break;

        case NODE_FN_DECL:
            compile_top_level_fn(node);
            break;

        case NODE_BREAK_STMT: {
            if (C.loop_depth <= 0) {
                fprintf(stderr, "[compiler] 'break' outside loop at line %d\n", line);
                C.had_error = true;
                break;
            }
            int pos = emit(RIZ_AsBx(OP_LOOP_BREAK, 0, 0), line);
            add_break_patch(pos);
            break;
        }

        case NODE_CONTINUE_STMT: {
            if (C.loop_depth <= 0) {
                fprintf(stderr, "[compiler] 'continue' outside loop at line %d\n", line);
                C.had_error = true;
                break;
            }
            int target = C.loops[C.loop_depth - 1].continue_target;
            if (target >= 0) {
                int offset = target - C.chunk->count - 1;
                emit(RIZ_AsBx(OP_LOOP_CONT, 0, offset), line);
            } else {
                /* continue_target not yet set (while loop) — jump to loop start */
                /* For while loops, continue_target is set at push_loop time */
                fprintf(stderr, "[compiler] 'continue' target not available at line %d\n", line);
                C.had_error = true;
            }
            break;
        }

        case NODE_INDEX_ASSIGN: {
            int obj_r = compile_expr(node->as.index_assign.object);
            int idx_r = compile_expr(node->as.index_assign.index);
            int val_r = compile_expr(node->as.index_assign.value);
            emit(RIZ_ABC(OP_SETINDEX, obj_r, idx_r, val_r), line);
            free_reg(val_r);
            free_reg(idx_r);
            free_reg(obj_r);
            break;
        }

        case NODE_MEMBER_ASSIGN: {
            int obj_r = compile_expr(node->as.member_assign.object);
            int val_r = compile_expr(node->as.member_assign.value);
            int k = add_string_const(node->as.member_assign.member);
            emit(RIZ_ABx(OP_SETMEMBER, obj_r, k), line);
            /* Encode value register in a following NOP-like instruction word */
            emit(RIZ_ABC(0, val_r, 0, 0), line);
            free_reg(val_r);
            free_reg(obj_r);
            break;
        }

        case NODE_IMPORT: {
            int k = add_string_const(node->as.import_stmt.path);
            emit(RIZ_ABx(OP_IMPORT, 0, k), line);
            break;
        }

        case NODE_IMPORT_NATIVE: {
            int k = add_string_const(node->as.import_native.path);
            emit(RIZ_ABx(OP_IMPORT_NATIVE, 0, k), line);
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

bool compiler_compile_ex(ASTNode* program, Chunk* chunk, bool module_return) {
    C.chunk = chunk;
    C.had_error = false;
    C.free_reg = 0;
    C.max_reg = 0;
    C.local_count = 0;
    C.scope_depth = 0;
    C.loop_depth = 0;
    inside_fn = false;

    compile_stmt(program);
    chunk->stack_slots = C.max_reg > 0 ? C.max_reg : RIZ_REG_MAX;
    if (chunk->stack_slots > RIZ_REG_MAX)
        chunk->stack_slots = RIZ_REG_MAX;
    if (module_return) {
        emit(RIZ_ABC(OP_LOADNIL, 0, 0, 0), 0);
        emit(RIZ_ABC(OP_RETURN, 0, 1, 0), 0);
    } else {
        emit(RIZ_ABC(OP_HALT, 0, 0, 0), 0);
    }

    return !C.had_error;
}

bool compiler_compile(ASTNode* program, Chunk* chunk) {
    return compiler_compile_ex(program, chunk, false);
}
