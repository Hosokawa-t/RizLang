/*
 * Riz Programming Language
 * codegen.c — AST-to-C transpiler (AOT compilation)
 *
 * Walks the AST and emits equivalent C code that uses riz_runtime.h.
 * The generated .c file can be compiled with any C compiler to produce
 * a standalone native binary — no interpreter needed.
 *
 * Usage: riz --aot input.riz -o output.exe
 *        (internally: Riz→C→GCC→native)
 */

#include "codegen.h"
#include <stdio.h>
#include <string.h>

/* ─── Code Generator State ────────────────────────────── */
typedef struct {
    FILE* out;
    int   indent;
    int   tmp_counter;  /* unique temp variable counter */
    bool  had_error;
} CodeGen;

static CodeGen G;

/* ─── Helpers ─────────────────────────────────────────── */

static void ind(void) {
    for (int i = 0; i < G.indent; i++) fprintf(G.out, "    ");
}

static int new_tmp(void) {
    return G.tmp_counter++;
}

/* Escape a string for C output */
static void emit_c_string(const char* s) {
    fputc('"', G.out);
    for (; *s; s++) {
        switch (*s) {
            case '\n': fprintf(G.out, "\\n"); break;
            case '\r': fprintf(G.out, "\\r"); break;
            case '\t': fprintf(G.out, "\\t"); break;
            case '\\': fprintf(G.out, "\\\\"); break;
            case '"':  fprintf(G.out, "\\\""); break;
            default:   fputc(*s, G.out); break;
        }
    }
    fputc('"', G.out);
}

/* ─── Forward declarations ────────────────────────────── */
static const char* emit_expr(ASTNode* node);
static void emit_stmt(ASTNode* node);

/* ─── Expression emission ─────────────────────────────── */
/*
 * emit_expr(node) → returns a C variable name (e.g. "_t3")
 * holding the result of the expression.
 */
static const char* emit_expr(ASTNode* node) {
    static char buf[64];
    if (!node) return "rv_none()";

    switch (node->type) {
        case NODE_INT_LIT:
            snprintf(buf, sizeof(buf), "rv_int(%lldLL)", (long long)node->as.int_lit.value);
            return buf;

        case NODE_FLOAT_LIT:
            snprintf(buf, sizeof(buf), "rv_float(%.17g)", node->as.float_lit.value);
            return buf;

        case NODE_STRING_LIT: {
            int t = new_tmp();
            ind(); fprintf(G.out, "RVal _t%d = rv_str(", t);
            emit_c_string(node->as.string_lit.value);
            fprintf(G.out, ");\n");
            snprintf(buf, sizeof(buf), "_t%d", t);
            return buf;
        }

        case NODE_BOOL_LIT:
            return node->as.bool_lit.value ? "rv_bool(true)" : "rv_bool(false)";

        case NODE_NONE_LIT:
            return "rv_none()";

        case NODE_IDENTIFIER:
            snprintf(buf, sizeof(buf), "%s", node->as.identifier.name);
            return buf;

        case NODE_UNARY: {
            const char* operand = emit_expr(node->as.unary.operand);
            int t = new_tmp();
            ind();
            if (node->as.unary.op == TOK_MINUS)
                fprintf(G.out, "RVal _t%d = rv_neg(%s);\n", t, operand);
            else if (node->as.unary.op == TOK_NOT)
                fprintf(G.out, "RVal _t%d = rv_bool(!rv_truthy(%s));\n", t, operand);
            else
                fprintf(G.out, "RVal _t%d = %s;\n", t, operand);
            snprintf(buf, sizeof(buf), "_t%d", t);
            return buf;
        }

        case NODE_BINARY: {
            const char* left_name  = emit_expr(node->as.binary.left);
            /* Copy to local to avoid clobbering static buf */
            char left_buf[64];
            strncpy(left_buf, left_name, 63); left_buf[63] = '\0';
            const char* right_name = emit_expr(node->as.binary.right);
            char right_buf[64];
            strncpy(right_buf, right_name, 63); right_buf[63] = '\0';

            int t = new_tmp();
            ind();
            switch (node->as.binary.op) {
                case TOK_PLUS:      fprintf(G.out, "RVal _t%d = rv_add(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_MINUS:     fprintf(G.out, "RVal _t%d = rv_sub(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_STAR:      fprintf(G.out, "RVal _t%d = rv_mul(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_SLASH:     fprintf(G.out, "RVal _t%d = rv_div(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_PERCENT:   fprintf(G.out, "RVal _t%d = rv_mod(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_FLOOR_DIV: fprintf(G.out, "RVal _t%d = rv_idiv(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_POWER:     fprintf(G.out, "RVal _t%d = rv_pow(%s, %s);\n", t, left_buf, right_buf); break;
                case TOK_EQ:        fprintf(G.out, "RVal _t%d = rv_bool(rv_eq(%s, %s));\n", t, left_buf, right_buf); break;
                case TOK_NEQ:       fprintf(G.out, "RVal _t%d = rv_bool(!rv_eq(%s, %s));\n", t, left_buf, right_buf); break;
                case TOK_LT:        fprintf(G.out, "RVal _t%d = rv_bool(rv_num(%s) < rv_num(%s));\n", t, left_buf, right_buf); break;
                case TOK_LTE:       fprintf(G.out, "RVal _t%d = rv_bool(rv_num(%s) <= rv_num(%s));\n", t, left_buf, right_buf); break;
                case TOK_GT:        fprintf(G.out, "RVal _t%d = rv_bool(rv_num(%s) > rv_num(%s));\n", t, left_buf, right_buf); break;
                case TOK_GTE:       fprintf(G.out, "RVal _t%d = rv_bool(rv_num(%s) >= rv_num(%s));\n", t, left_buf, right_buf); break;
                case TOK_AND:       fprintf(G.out, "RVal _t%d = rv_bool(rv_truthy(%s) && rv_truthy(%s));\n", t, left_buf, right_buf); break;
                case TOK_OR:        fprintf(G.out, "RVal _t%d = rv_bool(rv_truthy(%s) || rv_truthy(%s));\n", t, left_buf, right_buf); break;
                default:            fprintf(G.out, "RVal _t%d = rv_none(); /* unsupported op */\n", t); break;
            }
            snprintf(buf, sizeof(buf), "_t%d", t);
            return buf;
        }

        case NODE_CALL: {
            /* print() is special */
            if (node->as.call.callee->type == NODE_IDENTIFIER &&
                strcmp(node->as.call.callee->as.identifier.name, "print") == 0) {
                /* Emit each arg, then call rv_print */
                char arg_names[32][64];
                int argc = node->as.call.arg_count;
                for (int i = 0; i < argc; i++) {
                    const char* n = emit_expr(node->as.call.args[i]);
                    strncpy(arg_names[i], n, 63); arg_names[i][63] = '\0';
                }
                ind(); fprintf(G.out, "rv_print(%d", argc);
                for (int i = 0; i < argc; i++) fprintf(G.out, ", %s", arg_names[i]);
                fprintf(G.out, ");\n");
                return "rv_none()";
            }
            /* Generic function call — not yet supported in AOT */
            fprintf(stderr, "[codegen] Function calls (non-print) not yet supported in AOT\n");
            G.had_error = true;
            return "rv_none()";
        }

        case NODE_ASSIGN: {
            const char* val = emit_expr(node->as.assign.value);
            char val_buf[64];
            strncpy(val_buf, val, 63); val_buf[63] = '\0';
            ind(); fprintf(G.out, "%s = %s;\n", node->as.assign.name, val_buf);
            snprintf(buf, sizeof(buf), "%s", node->as.assign.name);
            return buf;
        }

        case NODE_COMPOUND_ASSIGN: {
            const char* val = emit_expr(node->as.compound_assign.value);
            char val_buf[64];
            strncpy(val_buf, val, 63); val_buf[63] = '\0';
            int t = new_tmp();
            ind();
            switch (node->as.compound_assign.op) {
                case TOK_PLUS_ASSIGN:  fprintf(G.out, "RVal _t%d = rv_add(%s, %s);\n", t, node->as.compound_assign.name, val_buf); break;
                case TOK_MINUS_ASSIGN: fprintf(G.out, "RVal _t%d = rv_sub(%s, %s);\n", t, node->as.compound_assign.name, val_buf); break;
                case TOK_STAR_ASSIGN:  fprintf(G.out, "RVal _t%d = rv_mul(%s, %s);\n", t, node->as.compound_assign.name, val_buf); break;
                case TOK_SLASH_ASSIGN: fprintf(G.out, "RVal _t%d = rv_div(%s, %s);\n", t, node->as.compound_assign.name, val_buf); break;
                default: fprintf(G.out, "RVal _t%d = rv_none();\n", t); break;
            }
            ind(); fprintf(G.out, "%s = _t%d;\n", node->as.compound_assign.name, t);
            snprintf(buf, sizeof(buf), "%s", node->as.compound_assign.name);
            return buf;
        }

        default:
            fprintf(stderr, "[codegen] Unhandled expr node %d\n", node->type);
            G.had_error = true;
            return "rv_none()";
    }
}

/* ─── Statement emission ──────────────────────────────── */

static void emit_stmt(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                emit_stmt(node->as.program.declarations[i]);
            }
            break;

        case NODE_EXPR_STMT:
            emit_expr(node->as.expr_stmt.expr);
            break;

        case NODE_LET_DECL: {
            const char* val = emit_expr(node->as.let_decl.initializer);
            char val_buf[64];
            strncpy(val_buf, val, 63); val_buf[63] = '\0';
            ind(); fprintf(G.out, "RVal %s = %s;\n", node->as.let_decl.name, val_buf);
            break;
        }

        case NODE_ASSIGN: {
            const char* val = emit_expr(node->as.assign.value);
            char val_buf[64];
            strncpy(val_buf, val, 63); val_buf[63] = '\0';
            ind(); fprintf(G.out, "%s = %s;\n", node->as.assign.name, val_buf);
            break;
        }

        case NODE_BLOCK:
            ind(); fprintf(G.out, "{\n");
            G.indent++;
            for (int i = 0; i < node->as.block.count; i++) {
                emit_stmt(node->as.block.statements[i]);
            }
            G.indent--;
            ind(); fprintf(G.out, "}\n");
            break;

        case NODE_IF_STMT: {
            const char* cond = emit_expr(node->as.if_stmt.condition);
            char cond_buf[64];
            strncpy(cond_buf, cond, 63); cond_buf[63] = '\0';
            ind(); fprintf(G.out, "if (rv_truthy(%s)) ", cond_buf);
            if (node->as.if_stmt.then_branch->type == NODE_BLOCK) {
                emit_stmt(node->as.if_stmt.then_branch);
            } else {
                fprintf(G.out, "{\n"); G.indent++;
                emit_stmt(node->as.if_stmt.then_branch);
                G.indent--; ind(); fprintf(G.out, "}\n");
            }
            if (node->as.if_stmt.else_branch) {
                ind(); fprintf(G.out, "else ");
                if (node->as.if_stmt.else_branch->type == NODE_IF_STMT) {
                    emit_stmt(node->as.if_stmt.else_branch);
                } else {
                    emit_stmt(node->as.if_stmt.else_branch);
                }
            }
            break;
        }

        case NODE_WHILE_STMT: {
            ind(); fprintf(G.out, "while (1) {\n");
            G.indent++;
            const char* cond = emit_expr(node->as.while_stmt.condition);
            char cond_buf[64];
            strncpy(cond_buf, cond, 63); cond_buf[63] = '\0';
            ind(); fprintf(G.out, "if (!rv_truthy(%s)) break;\n", cond_buf);
            if (node->as.while_stmt.body->type == NODE_BLOCK) {
                for (int i = 0; i < node->as.while_stmt.body->as.block.count; i++)
                    emit_stmt(node->as.while_stmt.body->as.block.statements[i]);
            } else {
                emit_stmt(node->as.while_stmt.body);
            }
            G.indent--;
            ind(); fprintf(G.out, "}\n");
            break;
        }

        case NODE_FOR_STMT: {
            const char* iter = emit_expr(node->as.for_stmt.iterable);
            char iter_buf[64];
            strncpy(iter_buf, iter, 63); iter_buf[63] = '\0';
            /* For now, assume iterable is a range-like list — emit a simple for loop */
            ind(); fprintf(G.out, "/* for loop — iterable not fully supported in AOT */\n");
            ind(); fprintf(G.out, "(void)%s;\n", iter_buf);
            break;
        }

        case NODE_BREAK_STMT:
            ind(); fprintf(G.out, "break;\n");
            break;

        case NODE_CONTINUE_STMT:
            ind(); fprintf(G.out, "continue;\n");
            break;

        case NODE_RETURN_STMT: {
            if (node->as.return_stmt.value) {
                const char* v = emit_expr(node->as.return_stmt.value);
                ind(); fprintf(G.out, "return %s;\n", v);
            } else {
                ind(); fprintf(G.out, "return rv_none();\n");
            }
            break;
        }

        default:
            ind(); fprintf(G.out, "/* [codegen] skipped node type %d */\n", node->type);
            break;
    }
}

/* ─── Public API ──────────────────────────────────────── */

bool codegen_emit(ASTNode* program, const char* output_path, const char* runtime_path) {
    G.out = fopen(output_path, "w");
    if (!G.out) {
        fprintf(stderr, "Cannot open output file '%s'\n", output_path);
        return false;
    }
    G.indent = 1;
    G.tmp_counter = 0;
    G.had_error = false;

    /* ── Emit file header ── */
    fprintf(G.out, "/*\n * Generated by Riz AOT Compiler\n * Source → C → Native Binary\n */\n\n");
    fprintf(G.out, "#include \"%s\"\n\n", runtime_path);
    fprintf(G.out, "int main(void) {\n");

    /* ── Emit body ── */
    emit_stmt(program);

    /* ── Emit footer ── */
    fprintf(G.out, "    return 0;\n");
    fprintf(G.out, "}\n");

    fclose(G.out);
    return !G.had_error;
}
