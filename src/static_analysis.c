/*
 * Riz — static analysis: control flow, duplicates, type hints, lexical name resolution
 */

#include "static_analysis.h"
#include "diagnostic.h"
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "riz_import.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;
    int   depth;
} SABind;

typedef struct {
    char** struct_names;
    int     struct_count;
    int     struct_cap;

    SABind* binds;
    int     bind_n;
    int     bind_cap;
    int     scope_depth;

    char** imported_names;
    int     imported_n;
    int     imported_cap;

    char** import_seen;
    int     import_seen_n;
    int     import_seen_cap;

    int     loop_depth;
    int     fn_depth;
    int     errors;

    bool    has_import_native; /* includes import_python (same AST node) */
} SACtx;

static const char* const SA_BUILTINS[] = {
    "abs", "all", "any", "append", "assert", "bool", "ceil", "chr", "clamp", "debug",
    "enumerate", "exit", "extend", "filter", "float", "floor", "format", "has_key",
    "input", "int", "keys", "len", "map", "max", "min", "ord", "panic", "pop",
    "parallel_sum", "print", "py", "range", "read_file", "reversed", "round", "sign",
    "sorted", "str", "sum", "time", "type", "values", "write_file", "zip", "cpu_count",
};

static bool sa_is_builtin(const char* name) {
    for (size_t i = 0; i < sizeof(SA_BUILTINS) / sizeof(SA_BUILTINS[0]); i++) {
        if (strcmp(SA_BUILTINS[i], name) == 0) return true;
    }
    return false;
}

static bool sa_plugin_name_guess(SACtx* C, const char* name) {
    if (!C->has_import_native) return false;
    return strncmp(name, "tensor_", 7) == 0 || strncmp(name, "llama_", 6) == 0
        || strncmp(name, "math_", 5) == 0;
}

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

static bool sa_imported_name(SACtx* C, const char* name) {
    for (int i = 0; i < C->imported_n; i++) {
        if (strcmp(C->imported_names[i], name) == 0) return true;
    }
    return false;
}

static bool sa_lookup(SACtx* C, const char* name) {
    if (strcmp(name, "_") == 0) return true;
    /* User / fn bindings shadow imports; imports shadow builtins (matches runtime after import). */
    for (int i = C->bind_n - 1; i >= 0; i--) {
        if (strcmp(C->binds[i].name, name) == 0) return true;
    }
    if (sa_imported_name(C, name)) return true;
    if (sa_struct_exists(C, name)) return true;
    if (sa_is_builtin(name)) return true;
    return false;
}

static void sa_check_id(SACtx* C, int line, const char* name) {
    if (strcmp(name, "_") == 0) return;
    if (sa_lookup(C, name)) return;
    if (sa_plugin_name_guess(C, name)) return;
    if (C->has_import_native) {
        sa_warn(C, line,
                "Undefined name '%s' (not a builtin, local, struct, or known import; may be registered by a native plugin)",
                name);
        return;
    }
    sa_error(C, line, "Undefined name '%s'", name);
}

static void sa_bind(SACtx* C, const char* name) {
    if (C->bind_n >= C->bind_cap) {
        int nc = C->bind_cap < 16 ? 16 : C->bind_cap * 2;
        C->binds = RIZ_GROW_ARRAY(SABind, C->binds, C->bind_cap, nc);
        C->bind_cap = nc;
    }
    C->binds[C->bind_n].name = riz_strdup(name);
    C->binds[C->bind_n].depth = C->scope_depth;
    C->bind_n++;
}

static void sa_push_scope(SACtx* C) { C->scope_depth++; }

static void sa_pop_scope(SACtx* C) {
    while (C->bind_n > 0 && C->binds[C->bind_n - 1].depth == C->scope_depth) {
        free(C->binds[C->bind_n - 1].name);
        C->bind_n--;
    }
    if (C->scope_depth > 0) C->scope_depth--;
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

static void sa_copy_dirname(const char* path, char* dir, size_t cap) {
    if (!path || !dir || cap < 2) {
        if (cap) { dir[0] = '.'; dir[1] = '\0'; }
        return;
    }
    strncpy(dir, path, cap - 1);
    dir[cap - 1] = '\0';
    char* s = strrchr(dir, '\\');
    if (!s) s = strrchr(dir, '/');
    if (s) *s = '\0';
    else {
        dir[0] = '.';
        dir[1] = '\0';
    }
}

static char* sa_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool sa_seen_import(SACtx* C, const char* path) {
    for (int i = 0; i < C->import_seen_n; i++) {
        if (strcmp(C->import_seen[i], path) == 0) return true;
    }
    return false;
}

static void sa_mark_seen_import(SACtx* C, const char* path) {
    if (C->import_seen_n >= C->import_seen_cap) {
        int nc = C->import_seen_cap < 8 ? 8 : C->import_seen_cap * 2;
        C->import_seen = RIZ_GROW_ARRAY(char*, C->import_seen, C->import_seen_cap, nc);
        C->import_seen_cap = nc;
    }
    C->import_seen[C->import_seen_n++] = riz_strdup(path);
}

static void sa_add_import_name(SACtx* C, const char* name) {
    if (sa_imported_name(C, name)) return;
    if (C->imported_n >= C->imported_cap) {
        int nc = C->imported_cap < 16 ? 16 : C->imported_cap * 2;
        C->imported_names = RIZ_GROW_ARRAY(char*, C->imported_names, C->imported_cap, nc);
        C->imported_cap = nc;
    }
    C->imported_names[C->imported_n++] = riz_strdup(name);
}

static void sa_collect_export_names_from_program(SACtx* C, ASTNode* prog) {
    if (!prog || prog->type != NODE_PROGRAM) return;
    for (int i = 0; i < prog->as.program.count; i++) {
        ASTNode* d = prog->as.program.declarations[i];
        switch (d->type) {
            case NODE_FN_DECL:
                sa_add_import_name(C, d->as.fn_decl.name);
                break;
            case NODE_LET_DECL:
                sa_add_import_name(C, d->as.let_decl.name);
                break;
            case NODE_STRUCT_DECL:
                sa_add_import_name(C, d->as.struct_decl.name);
                break;
            default:
                break;
        }
    }
}

static void sa_resolve_import_paths(const char* host_path, const char* import_path, char* out, size_t cap) {
    if (riz_import_resolve(out, cap, import_path)) return;
    char dir[1024];
    sa_copy_dirname(host_path, dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\%s", dir, import_path);
#else
    snprintf(out, cap, "%s/%s", dir, import_path);
#endif
}

static void sa_process_import_tree(SACtx* C, const char* host_path, const char* import_path, int depth) {
    if (depth > 24) return;
    char resolved[1024];
    sa_resolve_import_paths(host_path, import_path, resolved, sizeof resolved);
    FILE* test = fopen(resolved, "rb");
    if (!test) return;
    fclose(test);
    if (sa_seen_import(C, resolved)) return;
    sa_mark_seen_import(C, resolved);

    char* src = sa_read_file(resolved);
    if (!src) return;
    Lexer lex;
    lexer_init(&lex, src);
    Parser P;
    parser_init(&P, &lex);
    ASTNode* prog = parser_parse(&P);
    free(src);
    if (P.had_error || !prog) {
        ast_free(prog);
        return;
    }
    for (int i = 0; i < prog->as.program.count; i++) {
        ASTNode* d = prog->as.program.declarations[i];
        if (d->type == NODE_IMPORT) {
            sa_process_import_tree(C, resolved, d->as.import_stmt.path, depth + 1);
        }
    }
    sa_collect_export_names_from_program(C, prog);
    ast_free(prog);
}

static void sa_gather_root_imports(SACtx* C, const char* host_path, ASTNode* program) {
    if (!program || program->type != NODE_PROGRAM) return;
    for (int i = 0; i < program->as.program.count; i++) {
        ASTNode* d = program->as.program.declarations[i];
        if (d->type == NODE_IMPORT) {
            sa_process_import_tree(C, host_path, d->as.import_stmt.path, 0);
        } else if (d->type == NODE_IMPORT_NATIVE) {
            C->has_import_native = true;
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

static void sa_scan_import_flags(SACtx* C, ASTNode* program) {
    if (!program || program->type != NODE_PROGRAM) return;
    for (int i = 0; i < program->as.program.count; i++) {
        ASTNode* d = program->as.program.declarations[i];
        if (d->type == NODE_IMPORT_NATIVE) C->has_import_native = true;
    }
}

/* Bind all top-level fn names before any body so mutual recursion type-checks. */
static void sa_presolve_top_level_fn_names(SACtx* C, ASTNode* program) {
    if (!program || program->type != NODE_PROGRAM) return;
    for (int i = 0; i < program->as.program.count; i++) {
        ASTNode* d = program->as.program.declarations[i];
        if (d->type != NODE_FN_DECL) continue;
        const char* nm = d->as.fn_decl.name;
        bool dup = false;
        for (int j = C->bind_n - 1; j >= 0 && C->binds[j].depth == 0; j--) {
            if (strcmp(C->binds[j].name, nm) == 0) {
                sa_error(C, d->line, "Duplicate top-level function '%s'", nm);
                dup = true;
                break;
            }
        }
        if (!dup) sa_bind(C, nm);
    }
}

static void sa_walk_expr(SACtx* C, ASTNode* n);
static void sa_walk_stmt(SACtx* C, ASTNode* n);

static void sa_walk_block(SACtx* C, ASTNode* block) {
    if (!block || block->type != NODE_BLOCK) return;
    sa_push_scope(C);
    for (int i = 0; i < block->as.block.count; i++)
        sa_walk_stmt(C, block->as.block.statements[i]);
    sa_pop_scope(C);
}

static void sa_walk_block_flat(SACtx* C, ASTNode* block) {
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
    sa_push_scope(C);
    for (int i = 0; i < fn->as.fn_decl.param_count; i++)
        sa_bind(C, fn->as.fn_decl.params[i]);
    sa_walk_stmt(C, fn->as.fn_decl.body);
    sa_pop_scope(C);
    C->fn_depth--;
}

static void sa_walk_match_arm_body(SACtx* C, ASTNode* body) {
    if (!body) return;
    if (body->type == NODE_BLOCK) sa_walk_block(C, body);
    else sa_walk_expr(C, body);
}

static void sa_walk_match_expr(SACtx* C, ASTNode* n) {
    sa_walk_expr(C, n->as.match_expr.subject);
    for (int i = 0; i < n->as.match_expr.arm_count; i++) {
        RizMatchArm* a = &n->as.match_expr.arms[i];
        ASTNode* pat = a->pattern;
        if (pat->type == NODE_IDENTIFIER && strcmp(pat->as.identifier.name, "_") != 0) {
            sa_push_scope(C);
            sa_bind(C, pat->as.identifier.name);
            if (a->guard) sa_walk_expr(C, a->guard);
            sa_walk_match_arm_body(C, a->body);
            sa_pop_scope(C);
        } else {
            sa_walk_expr(C, pat);
            if (a->guard) sa_walk_expr(C, a->guard);
            sa_walk_match_arm_body(C, a->body);
        }
    }
}

static void sa_walk_expr(SACtx* C, ASTNode* n) {
    if (!n) return;
    switch (n->type) {
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_BOOL_LIT:
        case NODE_NONE_LIT:
            break;
        case NODE_IDENTIFIER:
            sa_check_id(C, n->line, n->as.identifier.name);
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
            if (!sa_lookup(C, n->as.assign.name))
                sa_error(C, n->line, "Assignment to undefined name '%s'", n->as.assign.name);
            sa_walk_expr(C, n->as.assign.value);
            break;
        case NODE_COMPOUND_ASSIGN:
            if (!sa_lookup(C, n->as.compound_assign.name))
                sa_error(C, n->line, "Compound assignment on undefined name '%s'", n->as.compound_assign.name);
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
                sa_push_scope(C);
                for (int i = 0; i < n->as.lambda.param_count; i++)
                    sa_bind(C, n->as.lambda.params[i]);
                if (n->as.lambda.body->type == NODE_BLOCK) sa_walk_block(C, n->as.lambda.body);
                else sa_walk_expr(C, n->as.lambda.body);
                sa_pop_scope(C);
                C->fn_depth--;
            }
            break;
        case NODE_MATCH_EXPR:
            sa_walk_match_expr(C, n);
            break;
        case NODE_TERNARY:
            sa_walk_expr(C, n->as.ternary.true_expr);
            sa_walk_expr(C, n->as.ternary.condition);
            sa_walk_expr(C, n->as.ternary.false_expr);
            break;
        case NODE_LIST_COMP:
            sa_walk_expr(C, n->as.list_comp.iterable);
            sa_push_scope(C);
            sa_bind(C, n->as.list_comp.var_name);
            if (n->as.list_comp.condition) sa_walk_expr(C, n->as.list_comp.condition);
            sa_walk_expr(C, n->as.list_comp.expr);
            sa_pop_scope(C);
            break;
        case NODE_SLICE:
            sa_walk_expr(C, n->as.slice.object);
            if (n->as.slice.start) sa_walk_expr(C, n->as.slice.start);
            if (n->as.slice.end) sa_walk_expr(C, n->as.slice.end);
            if (n->as.slice.step) sa_walk_expr(C, n->as.slice.step);
            break;
        default:
            break;
    }
}

static void sa_walk_for_stmt(SACtx* C, ASTNode* n) {
    sa_walk_expr(C, n->as.for_stmt.iterable);
    C->loop_depth++;
    sa_push_scope(C);
    sa_bind(C, n->as.for_stmt.var_name);
    if (n->as.for_stmt.body->type == NODE_BLOCK) sa_walk_block_flat(C, n->as.for_stmt.body);
    else sa_walk_stmt(C, n->as.for_stmt.body);
    sa_pop_scope(C);
    C->loop_depth--;
}

static void sa_walk_try_stmt(SACtx* C, ASTNode* n) {
    sa_walk_stmt(C, n->as.try_stmt.try_block);
    sa_push_scope(C);
    sa_bind(C, n->as.try_stmt.catch_var);
    sa_walk_stmt(C, n->as.try_stmt.catch_block);
    sa_pop_scope(C);
}

static void sa_walk_toplevel_decl(SACtx* C, ASTNode* n) {
    if (!n) return;
    switch (n->type) {
        case NODE_FN_DECL:
            sa_walk_fn(C, n);
            break;
        case NODE_LET_DECL:
            sa_validate_type_ann(C, n->line, n->as.let_decl.type_annotation);
            sa_walk_expr(C, n->as.let_decl.initializer);
            sa_bind(C, n->as.let_decl.name);
            break;
        case NODE_EXPR_STMT:
            sa_walk_expr(C, n->as.expr_stmt.expr);
            break;
        case NODE_IMPORT:
        case NODE_IMPORT_NATIVE:
        case NODE_STRUCT_DECL:
            break;
        case NODE_IMPL_DECL:
            for (int i = 0; i < n->as.impl_decl.method_count; i++) sa_walk_fn(C, n->as.impl_decl.methods[i]);
            break;
        case NODE_IMPL_TRAIT_DECL:
            for (int i = 0; i < n->as.impl_trait_decl.method_count; i++) sa_walk_fn(C, n->as.impl_trait_decl.methods[i]);
            break;
        case NODE_TRAIT_DECL:
            for (int i = 0; i < n->as.trait_decl.method_count; i++) sa_walk_fn(C, n->as.trait_decl.methods[i]);
            break;
        default:
            sa_walk_stmt(C, n);
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
            sa_bind(C, n->as.let_decl.name);
            break;
        case NODE_FN_DECL:
            sa_bind(C, n->as.fn_decl.name);
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
            sa_walk_for_stmt(C, n);
            if (n->as.for_stmt.else_branch) sa_walk_stmt(C, n->as.for_stmt.else_branch);
            break;
        case NODE_BLOCK:
            sa_walk_block(C, n);
            break;
        case NODE_TRY_STMT:
            sa_walk_try_stmt(C, n);
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
        case NODE_STRUCT_DECL:
            break;
        case NODE_IMPL_DECL:
            for (int i = 0; i < n->as.impl_decl.method_count; i++) sa_walk_fn(C, n->as.impl_decl.methods[i]);
            break;
        case NODE_IMPL_TRAIT_DECL:
            for (int i = 0; i < n->as.impl_trait_decl.method_count; i++) sa_walk_fn(C, n->as.impl_trait_decl.methods[i]);
            break;
        case NODE_TRAIT_DECL:
            for (int i = 0; i < n->as.trait_decl.method_count; i++) sa_walk_fn(C, n->as.trait_decl.methods[i]);
            break;
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

    sa_scan_import_flags(&ctx, program);
    sa_collect_program(&ctx, program);
    const char* host = riz_diag_source_path ? riz_diag_source_path : "";
    sa_gather_root_imports(&ctx, host, program);

    ctx.scope_depth = 0;
    sa_presolve_top_level_fn_names(&ctx, program);
    for (int i = 0; i < program->as.program.count; i++)
        sa_walk_toplevel_decl(&ctx, program->as.program.declarations[i]);

    riz_diag_source_kind = prev_kind;

    for (int i = 0; i < ctx.struct_count; i++) free(ctx.struct_names[i]);
    free(ctx.struct_names);
    for (int i = 0; i < ctx.bind_n; i++) free(ctx.binds[i].name);
    free(ctx.binds);
    for (int i = 0; i < ctx.imported_n; i++) free(ctx.imported_names[i]);
    free(ctx.imported_names);
    for (int i = 0; i < ctx.import_seen_n; i++) free(ctx.import_seen[i]);
    free(ctx.import_seen);

    return ctx.errors == 0;
}
