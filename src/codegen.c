/*
 * Riz Programming Language
 * codegen.c — AST-to-C transpiler (AOT compilation)
 *
 * Walks the AST and emits equivalent C code that uses aot_runtime.h.
 */

#include "codegen.h"
#include <stdio.h>
#include <string.h>

/* ─── Code Generator State ────────────────────────────── */
typedef struct {
    FILE* out;
    int   indent;
    int   tmp_counter;
    bool  had_error;
} CodeGen;

static CodeGen G;

/* ─── Static Type Tracker ────────────────────────────── */
typedef enum { AOT_DYN, AOT_INT, AOT_FLOAT, AOT_STRUCT } AOTType;
typedef struct {
    char name[64];
    AOTType type;
    char sname[64];
    int depth;
} AOTSymbol;

#define MAX_AOT_SYM 1024
static AOTSymbol sym_table[MAX_AOT_SYM];
static int sym_count = 0;
static int sym_depth = 0;
static AOTType last_expr_type = AOT_DYN;
static char last_expr_sname[64] = {0};

static void aot_push_scope(void) { sym_depth++; }
static void aot_pop_scope(void) { 
    while (sym_count > 0 && sym_table[sym_count-1].depth == sym_depth) sym_count--;
    sym_depth--; 
}
static void aot_insert_sym(const char* name, AOTType type, const char* sname) {
    if (sym_count >= MAX_AOT_SYM) return;
    strncpy(sym_table[sym_count].name, name, 63); sym_table[sym_count].name[63] = 0;
    sym_table[sym_count].type = type;
    if(sname) { strncpy(sym_table[sym_count].sname, sname, 63); sym_table[sym_count].sname[63] = 0; }
    else sym_table[sym_count].sname[0] = 0;
    sym_table[sym_count].depth = sym_depth;
    sym_count++;
}
static AOTSymbol* aot_lookup(const char* name) {
    for (int i = sym_count - 1; i >= 0; i--) {
        if (strcmp(sym_table[i].name, name) == 0) return &sym_table[i];
    }
    return NULL;
}

/* ─── Helpers ─────────────────────────────────────────── */

static char* get_temp_buf(void) {
    static char pools[32][512]; /* Increased pool size for complex expressions */
    static int pool_idx = 0;
    char* buf = pools[pool_idx];
    pool_idx = (pool_idx + 1) % 32;
    return buf;
}

static void ind(void) {
    for (int i = 0; i < G.indent; i++) fprintf(G.out, "    ");
}

static int new_tmp(void) {
    return G.tmp_counter++;
}

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

static const char* aot_box(const char* val, AOTType type) {
    if (type == AOT_INT) {
        char* b = get_temp_buf();
        snprintf(b, 511, "riz_int(%s)", val);
        return b;
    }
    if (type == AOT_FLOAT) {
        char* b = get_temp_buf();
        snprintf(b, 511, "riz_float(%s)", val);
        return b;
    }
    return val;
}

static const char* emit_expr_box(ASTNode* node) {
    const char* val = emit_expr(node);
    char safe_val[256]; strncpy(safe_val, val, 255); safe_val[255] = '\0';
    return aot_box(safe_val, last_expr_type);
}

static const char* emit_expr(ASTNode* node) {
    if (!node) { last_expr_type = AOT_DYN; return "riz_none()"; }

    switch (node->type) {
        case NODE_INT_LIT: {
            last_expr_type = AOT_INT;
            char* b = get_temp_buf();
            snprintf(b, 511, "%lldLL", (long long)node->as.int_lit.value);
            return b;
        }
        case NODE_FLOAT_LIT: {
            last_expr_type = AOT_FLOAT;
            char* b = get_temp_buf();
            snprintf(b, 511, "%.17g", node->as.float_lit.value);
            return b;
        }
        case NODE_STRING_LIT: {
            int t = new_tmp();
            ind(); fprintf(G.out, "RizValue _t%d = riz_string(", t);
            emit_c_string(node->as.string_lit.value);
            fprintf(G.out, ");\n");
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_BOOL_LIT:
            last_expr_type = AOT_DYN;
            return node->as.bool_lit.value ? "riz_bool(true)" : "riz_bool(false)";
        case NODE_NONE_LIT:
            last_expr_type = AOT_DYN;
            return "riz_none()";

        case NODE_LIST_LIT: {
            int count = node->as.list_lit.count;
            int t = new_tmp();
            ind(); fprintf(G.out, "RizValue _t%d = riz_list_new();\n", t);
            for (int i = 0; i < count; i++) {
                const char* arg = emit_expr_box(node->as.list_lit.items[i]);
                char arg_safe[256]; strncpy(arg_safe, arg, 255); arg_safe[255] = '\0';
                ind(); fprintf(G.out, "riz_list_append(_t%d.as.list, %s);\n", t, arg_safe);
            }
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_DICT_LIT: {
            int count = node->as.dict_lit.count;
            int t = new_tmp();
            ind(); fprintf(G.out, "RizValue _t%d = riz_dict_new();\n", t);
            for (int i = 0; i < count; i++) {
                const char* key = emit_expr_box(node->as.dict_lit.keys[i]);
                char key_safe[256]; strncpy(key_safe, key, 255); key_safe[255] = '\0';
                const char* val = emit_expr_box(node->as.dict_lit.values[i]);
                ind(); fprintf(G.out, "riz_dict_set(_t%d.as.dict, %s, %s);\n", t, key_safe, val);
            }
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_IDENTIFIER: {
            AOTSymbol* sym = aot_lookup(node->as.identifier.name);
            if (sym) {
                last_expr_type = sym->type;
                if(sym->type == AOT_STRUCT) strncpy(last_expr_sname, sym->sname, 63);
            } else {
                last_expr_type = AOT_DYN;
            }
            char* b = get_temp_buf();
            snprintf(b, 511, "%s", node->as.identifier.name);
            return b;
        }
        case NODE_UNARY: {
            const char* operand = emit_expr(node->as.unary.operand);
            AOTType o_type = last_expr_type;
            char op_buf[256]; strncpy(op_buf, operand, 255); op_buf[255] = '\0';
            int t = new_tmp();
            ind();
            if (o_type == AOT_INT) {
                if (node->as.unary.op == TOK_MINUS) fprintf(G.out, "long long _t%d = -%s;\n", t, op_buf);
                else if (node->as.unary.op == TOK_NOT) fprintf(G.out, "long long _t%d = !%s;\n", t, op_buf);
                else fprintf(G.out, "long long _t%d = %s;\n", t, op_buf);
                last_expr_type = AOT_INT;
            } else {
                const char* boxed = aot_box(op_buf, o_type);
                if (node->as.unary.op == TOK_MINUS) fprintf(G.out, "RizValue _t%d = aot_neg(%s);\n", t, boxed);
                else if (node->as.unary.op == TOK_NOT) fprintf(G.out, "RizValue _t%d = riz_bool(!riz_value_is_truthy(%s));\n", t, boxed);
                else fprintf(G.out, "RizValue _t%d = %s;\n", t, boxed);
                last_expr_type = AOT_DYN;
            }
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_BINARY: {
            const char* l_raw = emit_expr(node->as.binary.left);
            AOTType l_type = last_expr_type;
            char l_buf[256]; strncpy(l_buf, l_raw, 255); l_buf[255] = '\0';
            const char* r_raw = emit_expr(node->as.binary.right);
            AOTType r_type = last_expr_type;
            char r_buf[256]; strncpy(r_buf, r_raw, 255); r_buf[255] = '\0';
            int t = new_tmp();
            ind();
            if (l_type == AOT_INT && r_type == AOT_INT) {
                switch (node->as.binary.op) {
                    case TOK_PLUS:      fprintf(G.out, "long long _t%d = %s + %s;\n", t, l_buf, r_buf); break;
                    case TOK_MINUS:     fprintf(G.out, "long long _t%d = %s - %s;\n", t, l_buf, r_buf); break;
                    case TOK_STAR:      fprintf(G.out, "long long _t%d = %s * %s;\n", t, l_buf, r_buf); break;
                    case TOK_SLASH:     fprintf(G.out, "long long _t%d = %s / %s;\n", t, l_buf, r_buf); break;
                    case TOK_PERCENT:   fprintf(G.out, "long long _t%d = %s %% %s;\n", t, l_buf, r_buf); break;
                    case TOK_EQ:        fprintf(G.out, "long long _t%d = (%s == %s);\n", t, l_buf, r_buf); break;
                    case TOK_NEQ:       fprintf(G.out, "long long _t%d = (%s != %s);\n", t, l_buf, r_buf); break;
                    case TOK_LT:        fprintf(G.out, "long long _t%d = (%s < %s);\n", t, l_buf, r_buf); break;
                    case TOK_LTE:       fprintf(G.out, "long long _t%d = (%s <= %s);\n", t, l_buf, r_buf); break;
                    case TOK_GT:        fprintf(G.out, "long long _t%d = (%s > %s);\n", t, l_buf, r_buf); break;
                    case TOK_GTE:       fprintf(G.out, "long long _t%d = (%s >= %s);\n", t, l_buf, r_buf); break;
                    default:            fprintf(G.out, "long long _t%d = 0;\n", t); break;
                }
                last_expr_type = AOT_INT;
            } else {
                const char* bl = aot_box(l_buf, l_type);
                char bl_s[256]; strncpy(bl_s, bl, 255); bl_s[255] = '\0';
                const char* br = aot_box(r_buf, r_type);
                char br_s[256]; strncpy(br_s, br, 255); br_s[255] = '\0';
                switch (node->as.binary.op) {
                    case TOK_PLUS:      fprintf(G.out, "RizValue _t%d = aot_add(%s, %s);\n", t, bl_s, br_s); break;
                    case TOK_MINUS:     fprintf(G.out, "RizValue _t%d = aot_sub(%s, %s);\n", t, bl_s, br_s); break;
                    case TOK_STAR:      fprintf(G.out, "RizValue _t%d = aot_mul(%s, %s);\n", t, bl_s, br_s); break;
                    case TOK_SLASH:     fprintf(G.out, "RizValue _t%d = aot_div(%s, %s);\n", t, bl_s, br_s); break;
                    case TOK_PERCENT:   fprintf(G.out, "RizValue _t%d = aot_mod(%s, %s);\n", t, bl_s, br_s); break;
                    case TOK_EQ:        fprintf(G.out, "RizValue _t%d = riz_bool(riz_value_equal(%s, %s));\n", t, bl_s, br_s); break;
                    case TOK_NEQ:       fprintf(G.out, "RizValue _t%d = riz_bool(!riz_value_equal(%s, %s));\n", t, bl_s, br_s); break;
                    case TOK_LT:        fprintf(G.out, "RizValue _t%d = riz_bool(aot_num(%s) < aot_num(%s));\n", t, bl_s, br_s); break;
                    case TOK_LTE:       fprintf(G.out, "RizValue _t%d = riz_bool(aot_num(%s) <= aot_num(%s));\n", t, bl_s, br_s); break;
                    case TOK_GT:        fprintf(G.out, "RizValue _t%d = riz_bool(aot_num(%s) > aot_num(%s));\n", t, bl_s, br_s); break;
                    case TOK_GTE:       fprintf(G.out, "RizValue _t%d = riz_bool(aot_num(%s) >= aot_num(%s));\n", t, bl_s, br_s); break;
                    default:            fprintf(G.out, "RizValue _t%d = riz_none();\n", t); break;
                }
                last_expr_type = AOT_DYN;
            }
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_CALL: {
            char arg_names[32][128];
            int argc = node->as.call.arg_count;
            if (argc > 32) argc = 32;
            for (int i = 0; i < argc; i++) {
                const char* n = emit_expr_box(node->as.call.args[i]);
                strncpy(arg_names[i], n, 127); arg_names[i][127] = '\0';
            }
            if (node->as.call.callee->type == NODE_IDENTIFIER) {
                const char* name = node->as.call.callee->as.identifier.name;
                if (strcmp(name, "print") == 0) {
                    ind(); fprintf(G.out, "aot_print(%d", argc);
                    for (int i = 0; i < argc; i++) fprintf(G.out, ", %s", arg_names[i]);
                    fprintf(G.out, ");\n");
                    last_expr_type = AOT_DYN;
                    return "riz_none()";
                }
                if (strcmp(name, "input") == 0) {
                    int t = new_tmp();
                    ind(); fprintf(G.out, "RizValue _t%d = aot_input(%d", t, argc);
                    for (int i = 0; i < argc; i++) fprintf(G.out, ", %s", arg_names[i]);
                    fprintf(G.out, ");\n");
                    last_expr_type = AOT_DYN;
                    char* b = get_temp_buf();
                    snprintf(b, 511, "_t%d", t);
                    return b;
                }
                /* Handle built-in conversions */
                if (strcmp(name, "int") == 0 && argc == 1) {
                    int t = new_tmp();
                    ind(); fprintf(G.out, "RizValue _t%d = riz_int(aot_to_int(%s));\n", t, arg_names[0]);
                    last_expr_type = AOT_DYN;
                    char* b = get_temp_buf(); snprintf(b, 511, "_t%d", t); return b;
                }
                if (strcmp(name, "float") == 0 && argc == 1) {
                    int t = new_tmp();
                    ind(); fprintf(G.out, "RizValue _t%d = riz_float(aot_to_float(%s));\n", t, arg_names[0]);
                    last_expr_type = AOT_DYN;
                    char* b = get_temp_buf(); snprintf(b, 511, "_t%d", t); return b;
                }

                int t = new_tmp();
                ind(); fprintf(G.out, "RizValue _args%d[] = {", t);
                for (int i = 0; i < argc; i++) fprintf(G.out, "%s%s", (i > 0) ? ", " : "", arg_names[i]);
                fprintf(G.out, "%s};\n", argc==0 ? "riz_none()":"");
                ind(); fprintf(G.out, "RizValue _t%d = aot_call_plugin(\"%s\", %d, _args%d);\n", t, name, argc, t);
                last_expr_type = AOT_DYN;
                char* b = get_temp_buf();
                snprintf(b, 511, "_t%d", t);
                return b;
            }
            last_expr_type = AOT_DYN;
            return "riz_none()";
        }
        case NODE_ASSIGN: {
            const char* val = emit_expr(node->as.assign.value);
            AOTType v_type = last_expr_type;
            char v_buf[256]; strncpy(v_buf, val, 255); v_buf[255] = '\0';
            AOTSymbol* sym = aot_lookup(node->as.assign.name);
            if (sym && sym->type != AOT_DYN) {
                if (sym->type == AOT_INT) {
                    ind(); fprintf(G.out, "%s = aot_to_int(%s);\n", node->as.assign.name, aot_box(v_buf, v_type));
                } else if (sym->type == AOT_FLOAT) {
                    ind(); fprintf(G.out, "%s = aot_to_float(%s);\n", node->as.assign.name, aot_box(v_buf, v_type));
                } else {
                    ind(); fprintf(G.out, "%s = %s;\n", node->as.assign.name, v_buf);
                }
            } else {
                ind(); fprintf(G.out, "%s = %s;\n", node->as.assign.name, aot_box(v_buf, v_type));
            }
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "%s", node->as.assign.name);
            return b;
        }
        case NODE_MEMBER_ASSIGN: {
            const char* obj = emit_expr(node->as.member_assign.object);
            AOTType o_type = last_expr_type;
            char o_sname[64]; strncpy(o_sname, last_expr_sname, 63); o_sname[63] = '\0';
            char obj_buf[256]; strncpy(obj_buf, obj, 255); obj_buf[255] = '\0';
            const char* val = emit_expr_box(node->as.member_assign.value);
            char val_buf[256]; strncpy(val_buf, val, 255); val_buf[255] = '\0';
            if (o_type == AOT_STRUCT) {
                ind(); fprintf(G.out, "((%s*)%s)->%s = %s;\n", o_sname, obj_buf, node->as.member_assign.member, val_buf);
            } else {
                ind(); fprintf(G.out, "aot_member_set(%s, \"%s\", %s);\n", aot_box(obj_buf, o_type), node->as.member_assign.member, val_buf);
            }
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "%s", val_buf);
            return b;
        }
        case NODE_INDEX: {
            const char* obj = emit_expr_box(node->as.index_expr.object);
            char o_buf[256]; strncpy(o_buf, obj, 255); o_buf[255] = '\0';
            const char* idx = emit_expr_box(node->as.index_expr.index);
            int t = new_tmp();
            ind(); fprintf(G.out, "RizValue _t%d = aot_index(%s, %s);\n", t, o_buf, idx);
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_MEMBER: {
            const char* obj = emit_expr(node->as.member.object);
            AOTType o_type = last_expr_type;
            char o_sn[64]; strncpy(o_sn, last_expr_sname, 63); o_sn[63] = '\0';
            char o_buf[256]; strncpy(o_buf, obj, 255); o_buf[255] = '\0';
            int t = new_tmp();
            if (o_type == AOT_STRUCT) {
                ind(); fprintf(G.out, "RizValue _t%d = ((%s*)%s)->%s;\n", t, o_sn, o_buf, node->as.member.member);
            } else {
                ind(); fprintf(G.out, "RizValue _t%d = aot_member_get(%s, \"%s\");\n", t, aot_box(o_buf, o_type), node->as.member.member);
            }
            last_expr_type = AOT_DYN;
            char* b = get_temp_buf();
            snprintf(b, 511, "_t%d", t);
            return b;
        }
        case NODE_PIPE: {
            const char* left = emit_expr_box(node->as.pipe.left);
            last_expr_type = AOT_DYN;
            return left;
        }
        case NODE_MATCH_EXPR:
        case NODE_LAMBDA:
            last_expr_type = AOT_DYN; return "riz_none()";
        default:
            last_expr_type = AOT_DYN; return "riz_none()";
    }
}

static void emit_stmt(ASTNode* node) {
    if (!node) return;
    if (node->type != NODE_PROGRAM && node->type != NODE_BLOCK && node->type != NODE_FN_DECL) {
        ind(); fprintf(G.out, "aot_current_line = %d;\n", node->line);
    }
    switch (node->type) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) emit_stmt(node->as.program.declarations[i]);
            break;
        case NODE_EXPR_STMT:
            emit_expr(node->as.expr_stmt.expr);
            break;
        case NODE_LET_DECL: {
            const char* val = emit_expr(node->as.let_decl.initializer);
            AOTType v_type = last_expr_type;
            char v_buf[256]; strncpy(v_buf, val, 255); v_buf[255] = '\0';
            AOTType decl_t = AOT_DYN;
            if (node->as.let_decl.type_annotation) {
                if (strcmp(node->as.let_decl.type_annotation, "int") == 0) decl_t = AOT_INT;
                else if (strcmp(node->as.let_decl.type_annotation, "float") == 0) decl_t = AOT_FLOAT;
                else decl_t = AOT_STRUCT;
            }
            if (decl_t == AOT_INT) {
                aot_insert_sym(node->as.let_decl.name, AOT_INT, NULL);
                ind(); fprintf(G.out, "long long %s = aot_to_int(%s);\n", node->as.let_decl.name, aot_box(v_buf, v_type));
            } else if (decl_t == AOT_FLOAT) {
                aot_insert_sym(node->as.let_decl.name, AOT_FLOAT, NULL);
                ind(); fprintf(G.out, "double %s = aot_to_float(%s);\n", node->as.let_decl.name, aot_box(v_buf, v_type));
            } else if (decl_t == AOT_STRUCT) {
                aot_insert_sym(node->as.let_decl.name, AOT_STRUCT, node->as.let_decl.type_annotation);
                ind(); fprintf(G.out, "void* %s = (void*)aot_to_int(%s);\n", node->as.let_decl.name, aot_box(v_buf, v_type));
            } else {
                aot_insert_sym(node->as.let_decl.name, AOT_DYN, NULL);
                ind(); fprintf(G.out, "RizValue %s = %s;\n", node->as.let_decl.name, aot_box(v_buf, v_type));
            }
            break;
        }
        case NODE_BLOCK:
            ind(); fprintf(G.out, "{\n"); G.indent++; aot_push_scope();
            for (int i = 0; i < node->as.block.count; i++) emit_stmt(node->as.block.statements[i]);
            aot_pop_scope(); G.indent--; ind(); fprintf(G.out, "}\n");
            break;
        case NODE_IF_STMT: {
            const char* cond = emit_expr_box(node->as.if_stmt.condition);
            char c_buf[256]; strncpy(c_buf, cond, 255); c_buf[255] = '\0';
            ind(); fprintf(G.out, "if (riz_value_is_truthy(%s)) ", c_buf);
            emit_stmt(node->as.if_stmt.then_branch);
            if (node->as.if_stmt.else_branch) { ind(); fprintf(G.out, "else "); emit_stmt(node->as.if_stmt.else_branch); }
            break;
        }
        case NODE_WHILE_STMT: {
            ind(); fprintf(G.out, "while (1) {\n"); G.indent++;
            const char* cond = emit_expr_box(node->as.while_stmt.condition);
            char c_buf[256]; strncpy(c_buf, cond, 255); c_buf[255] = '\0';
            ind(); fprintf(G.out, "if (!riz_value_is_truthy(%s)) break;\n", c_buf);
            if (node->as.while_stmt.body->type == NODE_BLOCK) {
                for (int i = 0; i < node->as.while_stmt.body->as.block.count; i++)
                    emit_stmt(node->as.while_stmt.body->as.block.statements[i]);
            } else emit_stmt(node->as.while_stmt.body);
            G.indent--; ind(); fprintf(G.out, "}\n");
            break;
        }
        case NODE_FOR_STMT: {
            const char* iter = emit_expr_box(node->as.for_stmt.iterable);
            char iter_buf[256]; strncpy(iter_buf, iter, 255); iter_buf[255] = '\0';
            int t = new_tmp();
            ind(); fprintf(G.out, "RizValue _iter%d = %s;\n", t, iter_buf);
            ind(); fprintf(G.out, "if (_iter%d.type == VAL_LIST) {\n", t);
            G.indent++;
            ind(); fprintf(G.out, "for (int _i%d = 0; _i%d < _iter%d.as.list->count; _i%d++) {\n", t, t, t, t);
            G.indent++;
            ind(); fprintf(G.out, "RizValue %s = _iter%d.as.list->items[_i%d];\n", node->as.for_stmt.var_name, t, t);
            aot_insert_sym(node->as.for_stmt.var_name, AOT_DYN, NULL);
            if (node->as.for_stmt.body->type == NODE_BLOCK) {
                for (int i = 0; i < node->as.for_stmt.body->as.block.count; i++)
                    emit_stmt(node->as.for_stmt.body->as.block.statements[i]);
            } else emit_stmt(node->as.for_stmt.body);
            G.indent--; ind(); fprintf(G.out, "}\n");
            G.indent--; ind(); fprintf(G.out, "}\n");
            break;
        }
        case NODE_RETURN_STMT: {
            const char* val = node->as.return_stmt.value ? emit_expr_box(node->as.return_stmt.value) : "riz_none()";
            char v_buf[256]; strncpy(v_buf, val, 255); v_buf[255] = '\0';
            ind(); fprintf(G.out, "return %s;\n", v_buf);
            break;
        }
        case NODE_BREAK_STMT: ind(); fprintf(G.out, "break;\n"); break;
        case NODE_CONTINUE_STMT: ind(); fprintf(G.out, "continue;\n"); break;
        case NODE_IMPORT:
        case NODE_IMPORT_NATIVE:
        case NODE_STRUCT_DECL:
            ind(); fprintf(G.out, "/* [AOT skip] unsupported top-level node %d */\n", node->type);
            break;
        default: break;
    }
}

/* ─── AOT Function Passes ──────────────────────────────── */

static void emit_fn_prototypes(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++) emit_fn_prototypes(node->as.program.declarations[i]);
    } else if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.count; i++) emit_fn_prototypes(node->as.block.statements[i]);
    } else if (node->type == NODE_FN_DECL) {
        fprintf(G.out, "RizValue userfn_%s(RizValue* args, int argc);\n", node->as.fn_decl.name);
    }
}

static void emit_fn_bodies(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++) emit_fn_bodies(node->as.program.declarations[i]);
    } else if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.count; i++) emit_fn_bodies(node->as.block.statements[i]);
    } else if (node->type == NODE_FN_DECL) {
        fprintf(G.out, "RizValue userfn_%s(RizValue* args, int argc) {\n", node->as.fn_decl.name);
        G.indent = 1;
        ind(); fprintf(G.out, "if (argc != %d) { fprintf(stderr, \"[AOT] Arity mismatch in %s\\n\"); return riz_none(); }\n", node->as.fn_decl.param_count, node->as.fn_decl.name);
        for (int i = 0; i < node->as.fn_decl.param_count; i++) {
            ind(); fprintf(G.out, "RizValue %s = args[%d];\n", node->as.fn_decl.params[i], i);
        }
        if (node->as.fn_decl.body) {
            if (node->as.fn_decl.body->type == NODE_BLOCK) {
                int count = node->as.fn_decl.body->as.block.count;
                for (int i = 0; i < count; i++) emit_stmt(node->as.fn_decl.body->as.block.statements[i]);
            } else emit_stmt(node->as.fn_decl.body);
        }
        ind(); fprintf(G.out, "return riz_none();\n");
        G.indent = 0; fprintf(G.out, "}\n\n");
    }
}

static void emit_fn_registrations(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++) emit_fn_registrations(node->as.program.declarations[i]);
    } else if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.count; i++) emit_fn_registrations(node->as.block.statements[i]);
    } else if (node->type == NODE_FN_DECL) {
        ind(); fprintf(G.out, "aot_register_user_fn(\"%s\", userfn_%s, %d);\n", node->as.fn_decl.name, node->as.fn_decl.name, node->as.fn_decl.param_count);
    }
}

/* ─── Public API ──────────────────────────────────────── */

bool codegen_emit(ASTNode* program, const char* output_path, const char* runtime_path) {
    G.out = fopen(output_path, "w");
    if (!G.out) return false;
    G.indent = 0; G.tmp_counter = 0; G.had_error = false;
    fprintf(G.out, "/* Generated by Riz AOT Compiler */\n#include \"%s\"\n\n", runtime_path);
    emit_fn_prototypes(program);
    fprintf(G.out, "\n");
    emit_fn_bodies(program);
    fprintf(G.out, "int main(void) {\n");
    G.indent = 1;
    ind(); fprintf(G.out, "riz_enable_ansi();\n");
    ind(); fprintf(G.out, "char src_path[512];\n");
    ind(); fprintf(G.out, "strncpy(src_path, \"");
    for (const char* c = output_path; *c; c++) {
        if (*c == '\\') fprintf(G.out, "\\\\");
        else fprintf(G.out, "%c", *c);
    }
    fprintf(G.out, "\", 511);\n");
    ind(); fprintf(G.out, "char* suffix = strstr(src_path, \"_aot.c\");\n");
    ind(); fprintf(G.out, "if (suffix) strcpy(suffix, \".riz\");\n");
    ind(); fprintf(G.out, "aot_source_path = src_path;\n");
    ind(); fprintf(G.out, "aot_setup_builtins();\n");
    emit_fn_registrations(program);
    emit_stmt(program);
    ind(); fprintf(G.out, "return 0;\n}\n");
    fclose(G.out);
    return !G.had_error;
}
