/*
 * Riz — static analysis pass (control flow, duplicates, type hints)
 */

#include "static_analysis.h"
#include "diagnostic.h"
#include "common.h"
#include <string.h>

typedef struct {
    char** struct_names;
    int     struct_count;
    int     struct_cap;
    int     loop_depth;
    int     fn_depth;
    int     errors;
} SACtx;

static void sa_error(SACtx* C, int line, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    C->errors++;
    riz_error(line, "%s", buf);
}

static void sa_warn(SACtx* C, int line, const char* fmt, ...) {
    (void)C;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    riz_warn(line, "%s", buf);
}

static bool sa_struct_exists(SACtx* C, const char* name) {
    for (int i = 0; i < C->struct_count; i++) {
        if (strcmp(C->struct_names[i], name) == 0) return true;
    }
    return false;
}

static bool sa_is_primitive_type(const char* t) {
    return strcmp(t, "int") == 0 || strcmp(t, "float") == 0 || strcmp(t, "bool") == 0
        || strcmp(t, "str") == 0 || strcmp(t, "none") == 0;
}

static void sa_validate_type_ann(SACtx* C, int line, const char* ann) {
    if (!ann || !ann[0]) return;
    if (sa_is_primitive_type(ann)) return;
    if (sa_struct_exists(C, ann)) return;
    sa_warn(C, line,
            "Unknown type '%s' in annotation (not a built-in and no struct with that name in this file; OK for native/AOT placeholders)",
            ann);
}

static void sa_check_dup_params(SACtx* C, int line, char** params, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(params[i], params[j]) == 0) {
                sa_error(C, line, "Duplicate parameter name '%s'", params[i]);
                return;
            }
        }
    }
}

static void sa_collect_program(SACtx* C, ASTNode* program) {
    if (!program || program->type != NODE_PROGRAM) return;
    for (int i = 0; i < program->as.program.count; i++) {
        ASTNode* d = program->as.program.declarations[i];
        if (d->type != NODE_STRUCT_DECL) continue;
        const char* sname = d->as.struct_decl.name;
        if (sa_struct_exists(C, sname)) {
            sa_error(C, d->line, "Duplicate struct '%s'", sname);
            continue;
        }
        int fc = d->as.struct_decl.field_count;
        char** fnames = d->as.struct_decl.field_names;
        for (int a = 0; a < fc; a++) {
            for (int b = a + 1; b < fc; b++) {
                if (strcmp(fnames[a], fnames[b]) == 0) {
                    sa_error(C, d->line, "Duplicate field '%s' in struct '%s'", fnames[a], sname);
                }
            }
        }
        if (C->struct_count >= C->struct_cap) {
            int nc = C->struct_cap < 8 ? 8 : C->struct_cap * 2;
            C->struct_names = RIZ_GROW_ARRAY(char*, C->struct_names, C->struct_cap, nc);
            C->struct_cap = nc;
        }
        C->struct_names[C->struct_count++] = riz_strdup(sname);
    }
}

static void sa_walk_expr(SACtx* C, ASTNode* n);
static void sa_walk_stmt(SACtx* C, ASTNode* n);

static void sa_walk_block(SACtx* C, ASTNode* block) {
    if (!block || block->type != NODE_BLOCK) return;
    for (int i = 0; i < block->as.block.count; i++)
        sa_walk_stmt(C, block->as.block.statements[i]);
}

static void sa_walk_fn(SACtx* C, ASTNode* fn) {
    if (!fn || fn->type != NODE_FN_DECL) return;
    sa_check_dup_params(C, fn->line, fn->as.fn_decl.params, fn->as.fn_decl.param_count);
    sa_validate_type_ann(C, fn->line, fn->as.fn_decl.return_type);
    if (!fn->as.fn_decl.body) return;
    C->fn_depth++;
    sa_walk_stmt(C, fn->as.fn_decl.body);
    C->fn_depth--;
}

static void sa_walk_match_arm_body(SACtx* C, ASTNode* body) {
    if (!body) return;
    if (body->type == NODE_BLOCK) sa_walk_block(C, body);
    else sa_walk_expr(C, body);
}

static void sa_walk_expr(SACtx* C, ASTNode* n) {
    if (!n) return;
    switch (n->type) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_BOOL_LIT:
        case NODE_NONE_LIT:
        case NODE_IDENTIFIER:
            break;
        case NODE_LIST_LIT:
            for (int i = 0; i < n->as.list_lit.count; i++) sa_walk_expr(C, n->as.list_lit.items[i]);
            break;
        case NODE_DICT_LIT:
            for (int i = 0; i < n->as.dict_lit.count; i++) {
                sa_walk_expr(C, n->as.dict_lit.keys[i]);
                sa_walk_expr(C, n->as.dict_lit.values[i]);
            }
            break;
        case NODE_UNARY:
            sa_walk_expr(C, n->as.unary.operand);
            break;
        case NODE_BINARY:
            sa_walk_expr(C, n->as.binary.left);
            sa_walk_expr(C, n->as.binary.right);
            break;
        case NODE_CALL:
            sa_walk_expr(C, n->as.call.callee);
            for (int i = 0; i < n->as.call.arg_count; i++) sa_walk_expr(C, n->as.call.args[i]);
            break;
        case NODE_INDEX:
            sa_walk_expr(C, n->as.index_expr.object);
            sa_walk_expr(C, n->as.index_expr.index);
            break;
        case NODE_ASSIGN:
            sa_walk_expr(C, n->as.assign.value);
            break;
        case NODE_COMPOUND_ASSIGN:
            sa_walk_expr(C, n->as.compound_assign.value);
            break;
        case NODE_MEMBER:
            sa_walk_expr(C, n->as.member.object);
            break;
        case NODE_PIPE:
            sa_walk_expr(C, n->as.pipe.left);
            sa_walk_expr(C, n->as.pipe.right);
            break;
        case NODE_LAMBDA:
            sa_check_dup_params(C, n->line, n->as.lambda.params, n->as.lambda.param_count);
            if (n->as.lambda.body) {
                C->fn_depth++;
                if (n->as.lambda.body->type == NODE_BLOCK) sa_walk_block(C, n->as.lambda.body);
                else sa_walk_expr(C, n->as.lambda.body);
                C->fn_depth--;
            }
            break;
        case NODE_MATCH_EXPR:
            sa_walk_expr(C, n->as.match_expr.subject);
            for (int i = 0; i < n->as.match_expr.arm_count; i++) {
                RizMatchArm* a = &n->as.match_expr.arms[i];
                sa_walk_expr(C, a->pattern);
                if (a->guard) sa_walk_expr(C, a->guard);
                sa_walk_match_arm_body(C, a->body);
            }
            break;
        default:
            break;
    }
}

static void sa_walk_stmt(SACtx* C, ASTNode* n) {
    if (!n) return;
    switch (n->type) {
        case NODE_EXPR_STMT:
            sa_walk_expr(C, n->as.expr_stmt.expr);
            break;
        case NODE_LET_DECL:
            sa_validate_type_ann(C, n->line, n->as.let_decl.type_annotation);
            sa_walk_expr(C, n->as.let_decl.initializer);
            break;
        case NODE_FN_DECL:
            sa_walk_fn(C, n);
            break;
        case NODE_RETURN_STMT:
            if (C->fn_depth <= 0) sa_error(C, n->line, "'return' outside of a function");
            if (n->as.return_stmt.value) sa_walk_expr(C, n->as.return_stmt.value);
            break;
        case NODE_BREAK_STMT:
            if (C->loop_depth <= 0) sa_error(C, n->line, "'break' outside of a loop");
            break;
        case NODE_CONTINUE_STMT:
            if (C->loop_depth <= 0) sa_error(C, n->line, "'continue' outside of a loop");
            break;
        case NODE_IF_STMT:
            sa_walk_expr(C, n->as.if_stmt.condition);
            sa_walk_stmt(C, n->as.if_stmt.then_branch);
            sa_walk_stmt(C, n->as.if_stmt.else_branch);
            break;
        case NODE_WHILE_STMT:
            sa_walk_expr(C, n->as.while_stmt.condition);
            C->loop_depth++;
            sa_walk_stmt(C, n->as.while_stmt.body);
            C->loop_depth--;
            break;
        case NODE_FOR_STMT:
            sa_walk_expr(C, n->as.for_stmt.iterable);
            C->loop_depth++;
            sa_walk_stmt(C, n->as.for_stmt.body);
            C->loop_depth--;
            break;
        case NODE_BLOCK:
            sa_walk_block(C, n);
            break;
        case NODE_TRY_STMT:
            sa_walk_stmt(C, n->as.try_stmt.try_block);
            sa_walk_stmt(C, n->as.try_stmt.catch_block);
            break;
        case NODE_THROW_STMT:
            sa_walk_expr(C, n->as.throw_stmt.value);
            break;
        case NODE_MEMBER_ASSIGN:
            sa_walk_expr(C, n->as.member_assign.object);
            sa_walk_expr(C, n->as.member_assign.value);
            break;
        case NODE_INDEX_ASSIGN:
            sa_walk_expr(C, n->as.index_assign.object);
            sa_walk_expr(C, n->as.index_assign.index);
            sa_walk_expr(C, n->as.index_assign.value);
            break;
        case NODE_IMPORT:
        case NODE_IMPORT_NATIVE:
            break;
        case NODE_STRUCT_DECL:
            break;
        case NODE_IMPL_DECL: {
            int m = n->as.impl_decl.method_count;
            for (int i = 0; i < m; i++) sa_walk_fn(C, n->as.impl_decl.methods[i]);
            break;
        }
        case NODE_IMPL_TRAIT_DECL: {
            int m = n->as.impl_trait_decl.method_count;
            for (int i = 0; i < m; i++) sa_walk_fn(C, n->as.impl_trait_decl.methods[i]);
            break;
        }
        case NODE_TRAIT_DECL: {
            int m = n->as.trait_decl.method_count;
            for (int i = 0; i < m; i++) sa_walk_fn(C, n->as.trait_decl.methods[i]);
            break;
        }
        case NODE_PROGRAM:
            for (int i = 0; i < n->as.program.count; i++) sa_walk_stmt(C, n->as.program.declarations[i]);
            break;
        default:
            sa_walk_expr(C, n);
            break;
    }
}

bool riz_static_analysis_ok(ASTNode* program) {
    SACtx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (!program || program->type != NODE_PROGRAM) return true;

    const char* prev_kind = riz_diag_source_kind;
    riz_diag_source_kind = "riz-static";

    sa_collect_program(&ctx, program);
    for (int i = 0; i < program->as.program.count; i++)
        sa_walk_stmt(&ctx, program->as.program.declarations[i]);

    riz_diag_source_kind = prev_kind;

    for (int i = 0; i < ctx.struct_count; i++) free(ctx.struct_names[i]);
    free(ctx.struct_names);

    return ctx.errors == 0;
}
