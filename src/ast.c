/*
 * Riz Programming Language
 * ast.c — AST node constructors and destructor (Phase 3)
 */

#include "ast.h"

static ASTNode* new_node(NodeType type, int line) {
    ASTNode* node = RIZ_ALLOC(ASTNode);
    node->type = type;
    node->line = line;
    return node;
}

/* ═══ Literals ═══ */

ASTNode* ast_int_lit(int64_t value, int line) { ASTNode* n=new_node(NODE_INT_LIT,line); n->as.int_lit.value=value; return n; }
ASTNode* ast_float_lit(double value, int line) { ASTNode* n=new_node(NODE_FLOAT_LIT,line); n->as.float_lit.value=value; return n; }
ASTNode* ast_string_lit(const char* value, int line) { ASTNode* n=new_node(NODE_STRING_LIT,line); n->as.string_lit.value=riz_strdup(value); return n; }
ASTNode* ast_bool_lit(bool value, int line) { ASTNode* n=new_node(NODE_BOOL_LIT,line); n->as.bool_lit.value=value; return n; }
ASTNode* ast_none_lit(int line) { return new_node(NODE_NONE_LIT, line); }
ASTNode* ast_list_lit(ASTNode** items, int count, int line) { ASTNode* n=new_node(NODE_LIST_LIT,line); n->as.list_lit.items=items; n->as.list_lit.count=count; return n; }
ASTNode* ast_dict_lit(ASTNode** keys, ASTNode** values, int count, int line) {
    ASTNode* n=new_node(NODE_DICT_LIT,line); n->as.dict_lit.keys=keys; n->as.dict_lit.values=values; n->as.dict_lit.count=count; return n;
}

/* ═══ Expressions ═══ */

ASTNode* ast_identifier(const char* name, int line) { ASTNode* n=new_node(NODE_IDENTIFIER,line); n->as.identifier.name=riz_strdup(name); return n; }
ASTNode* ast_unary(TokenType op, ASTNode* operand, int line) { ASTNode* n=new_node(NODE_UNARY,line); n->as.unary.op=op; n->as.unary.operand=operand; return n; }
ASTNode* ast_binary(TokenType op, ASTNode* left, ASTNode* right, int line) { ASTNode* n=new_node(NODE_BINARY,line); n->as.binary.op=op; n->as.binary.left=left; n->as.binary.right=right; return n; }
ASTNode* ast_call(ASTNode* callee, ASTNode** args, int arg_count, int line) { ASTNode* n=new_node(NODE_CALL,line); n->as.call.callee=callee; n->as.call.args=args; n->as.call.arg_count=arg_count; return n; }
ASTNode* ast_index(ASTNode* object, ASTNode* index, int line) { ASTNode* n=new_node(NODE_INDEX,line); n->as.index_expr.object=object; n->as.index_expr.index=index; return n; }
ASTNode* ast_assign(const char* name, ASTNode* value, int line) { ASTNode* n=new_node(NODE_ASSIGN,line); n->as.assign.name=riz_strdup(name); n->as.assign.value=value; return n; }
ASTNode* ast_compound_assign(const char* name, TokenType op, ASTNode* value, int line) { ASTNode* n=new_node(NODE_COMPOUND_ASSIGN,line); n->as.compound_assign.name=riz_strdup(name); n->as.compound_assign.op=op; n->as.compound_assign.value=value; return n; }
ASTNode* ast_member(ASTNode* object, const char* member, int line) { ASTNode* n=new_node(NODE_MEMBER,line); n->as.member.object=object; n->as.member.member=riz_strdup(member); return n; }
ASTNode* ast_pipe(ASTNode* left, ASTNode* right, int line) { ASTNode* n=new_node(NODE_PIPE,line); n->as.pipe.left=left; n->as.pipe.right=right; return n; }
ASTNode* ast_lambda(char** params, int param_count, ASTNode* body, int line) { ASTNode* n=new_node(NODE_LAMBDA,line); n->as.lambda.params=params; n->as.lambda.param_count=param_count; n->as.lambda.body=body; return n; }
ASTNode* ast_match_expr(ASTNode* subject, RizMatchArm* arms, int arm_count, int line) { ASTNode* n=new_node(NODE_MATCH_EXPR,line); n->as.match_expr.subject=subject; n->as.match_expr.arms=arms; n->as.match_expr.arm_count=arm_count; return n; }

/* ═══ Statements ═══ */

ASTNode* ast_expr_stmt(ASTNode* expr, int line) { ASTNode* n=new_node(NODE_EXPR_STMT,line); n->as.expr_stmt.expr=expr; return n; }
ASTNode* ast_let_decl(const char* name, const char* type_ann, ASTNode* init, bool mutable, int line) {
    ASTNode* n=new_node(NODE_LET_DECL,line); n->as.let_decl.name=riz_strdup(name);
    n->as.let_decl.type_annotation=type_ann?riz_strdup(type_ann):NULL; n->as.let_decl.initializer=init; n->as.let_decl.is_mutable=mutable; return n;
}
ASTNode* ast_fn_decl(const char* name, char** params, int param_count, ASTNode** defaults, const char* ret_type, ASTNode* body, int line) {
    ASTNode* n=new_node(NODE_FN_DECL,line); n->as.fn_decl.name=riz_strdup(name); n->as.fn_decl.params=params;
    n->as.fn_decl.param_count=param_count; n->as.fn_decl.return_type=ret_type?riz_strdup(ret_type):NULL;
    n->as.fn_decl.body=body; n->as.fn_decl.param_defaults=defaults; return n;
}
ASTNode* ast_return_stmt(ASTNode* value, int line) { ASTNode* n=new_node(NODE_RETURN_STMT,line); n->as.return_stmt.value=value; return n; }
ASTNode* ast_if_stmt(ASTNode* cond, ASTNode* then_b, ASTNode* else_b, int line) { ASTNode* n=new_node(NODE_IF_STMT,line); n->as.if_stmt.condition=cond; n->as.if_stmt.then_branch=then_b; n->as.if_stmt.else_branch=else_b; return n; }
ASTNode* ast_while_stmt(ASTNode* cond, ASTNode* body, int line) { ASTNode* n=new_node(NODE_WHILE_STMT,line); n->as.while_stmt.condition=cond; n->as.while_stmt.body=body; return n; }
ASTNode* ast_for_stmt(const char* var_name, ASTNode* iterable, ASTNode* body, int line) { ASTNode* n=new_node(NODE_FOR_STMT,line); n->as.for_stmt.var_name=riz_strdup(var_name); n->as.for_stmt.iterable=iterable; n->as.for_stmt.body=body; return n; }
ASTNode* ast_break_stmt(int line) { return new_node(NODE_BREAK_STMT, line); }
ASTNode* ast_continue_stmt(int line) { return new_node(NODE_CONTINUE_STMT, line); }
ASTNode* ast_block(ASTNode** stmts, int count, int line) { ASTNode* n=new_node(NODE_BLOCK,line); n->as.block.statements=stmts; n->as.block.count=count; return n; }
ASTNode* ast_import(const char* path, int line) { ASTNode* n=new_node(NODE_IMPORT,line); n->as.import_stmt.path=riz_strdup(path); return n; }
ASTNode* ast_import_native(const char* path, int line) { ASTNode* n=new_node(NODE_IMPORT_NATIVE,line); n->as.import_native.path=riz_strdup(path); return n; }
ASTNode* ast_struct_decl(const char* name, char** fields, int field_count, int line) {
    ASTNode* n=new_node(NODE_STRUCT_DECL,line); n->as.struct_decl.name=riz_strdup(name); n->as.struct_decl.field_names=fields; n->as.struct_decl.field_count=field_count; return n;
}
ASTNode* ast_impl_decl(const char* name, ASTNode** methods, int method_count, int line) {
    ASTNode* n=new_node(NODE_IMPL_DECL,line); n->as.impl_decl.struct_name=riz_strdup(name); n->as.impl_decl.methods=methods; n->as.impl_decl.method_count=method_count; return n;
}
ASTNode* ast_trait_decl(const char* name, ASTNode** methods, int method_count, int line) {
    ASTNode* n=new_node(NODE_TRAIT_DECL,line); n->as.trait_decl.name=riz_strdup(name); n->as.trait_decl.methods=methods; n->as.trait_decl.method_count=method_count; return n;
}
ASTNode* ast_impl_trait_decl(const char* trait_name, const char* struct_name, ASTNode** methods, int method_count, int line) {
    ASTNode* n=new_node(NODE_IMPL_TRAIT_DECL,line); n->as.impl_trait_decl.trait_name=riz_strdup(trait_name); n->as.impl_trait_decl.struct_name=riz_strdup(struct_name); n->as.impl_trait_decl.methods=methods; n->as.impl_trait_decl.method_count=method_count; return n;
}
ASTNode* ast_try_stmt(ASTNode* try_block, const char* catch_var, ASTNode* catch_block, int line) {
    ASTNode* n=new_node(NODE_TRY_STMT,line); n->as.try_stmt.try_block=try_block; n->as.try_stmt.catch_var=catch_var?riz_strdup(catch_var):NULL; n->as.try_stmt.catch_block=catch_block; return n;
}
ASTNode* ast_throw_stmt(ASTNode* value, int line) { ASTNode* n=new_node(NODE_THROW_STMT,line); n->as.throw_stmt.value=value; return n; }
ASTNode* ast_member_assign(ASTNode* object, char* member, ASTNode* value, int line) {
    ASTNode* n=new_node(NODE_MEMBER_ASSIGN,line); n->as.member_assign.object=object; n->as.member_assign.member=member; n->as.member_assign.value=value; return n;
}
ASTNode* ast_index_assign(ASTNode* object, ASTNode* index, ASTNode* value, int line) {
    ASTNode* n=new_node(NODE_INDEX_ASSIGN,line); n->as.index_assign.object=object; n->as.index_assign.index=index; n->as.index_assign.value=value; return n;
}
ASTNode* ast_program(ASTNode** decls, int count) { ASTNode* n=new_node(NODE_PROGRAM,0); n->as.program.declarations=decls; n->as.program.count=count; return n; }

/* ═══ Destructor ═══ */

void ast_free(ASTNode* node) {
    if (!node) return;
    switch (node->type) {
        case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_BOOL_LIT: case NODE_NONE_LIT: case NODE_BREAK_STMT: case NODE_CONTINUE_STMT: break;
        case NODE_STRING_LIT: free(node->as.string_lit.value); break;
        case NODE_LIST_LIT: for(int i=0;i<node->as.list_lit.count;i++)ast_free(node->as.list_lit.items[i]); free(node->as.list_lit.items); break;
        case NODE_DICT_LIT: for(int i=0;i<node->as.dict_lit.count;i++){ast_free(node->as.dict_lit.keys[i]);ast_free(node->as.dict_lit.values[i]);} free(node->as.dict_lit.keys);free(node->as.dict_lit.values); break;
        case NODE_IDENTIFIER: free(node->as.identifier.name); break;
        case NODE_UNARY: ast_free(node->as.unary.operand); break;
        case NODE_BINARY: ast_free(node->as.binary.left);ast_free(node->as.binary.right); break;
        case NODE_CALL: ast_free(node->as.call.callee); for(int i=0;i<node->as.call.arg_count;i++)ast_free(node->as.call.args[i]); free(node->as.call.args); break;
        case NODE_INDEX: ast_free(node->as.index_expr.object);ast_free(node->as.index_expr.index); break;
        case NODE_ASSIGN: free(node->as.assign.name);ast_free(node->as.assign.value); break;
        case NODE_COMPOUND_ASSIGN: free(node->as.compound_assign.name);ast_free(node->as.compound_assign.value); break;
        case NODE_MEMBER: ast_free(node->as.member.object);free(node->as.member.member); break;
        case NODE_PIPE: ast_free(node->as.pipe.left);ast_free(node->as.pipe.right); break;
        case NODE_LAMBDA: for(int i=0;i<node->as.lambda.param_count;i++)free(node->as.lambda.params[i]); free(node->as.lambda.params);ast_free(node->as.lambda.body); break;
        case NODE_MATCH_EXPR: ast_free(node->as.match_expr.subject); for(int i=0;i<node->as.match_expr.arm_count;i++){ast_free(node->as.match_expr.arms[i].pattern);ast_free(node->as.match_expr.arms[i].guard);ast_free(node->as.match_expr.arms[i].body);} free(node->as.match_expr.arms); break;
        case NODE_EXPR_STMT: ast_free(node->as.expr_stmt.expr); break;
        case NODE_LET_DECL: free(node->as.let_decl.name);free(node->as.let_decl.type_annotation);ast_free(node->as.let_decl.initializer); break;
        case NODE_FN_DECL: free(node->as.fn_decl.name); for(int i=0;i<node->as.fn_decl.param_count;i++)free(node->as.fn_decl.params[i]); free(node->as.fn_decl.params);free(node->as.fn_decl.return_type);ast_free(node->as.fn_decl.body); break;
        case NODE_RETURN_STMT: ast_free(node->as.return_stmt.value); break;
        case NODE_IF_STMT: ast_free(node->as.if_stmt.condition);ast_free(node->as.if_stmt.then_branch);ast_free(node->as.if_stmt.else_branch); break;
        case NODE_WHILE_STMT: ast_free(node->as.while_stmt.condition);ast_free(node->as.while_stmt.body); break;
        case NODE_FOR_STMT: free(node->as.for_stmt.var_name);ast_free(node->as.for_stmt.iterable);ast_free(node->as.for_stmt.body); break;
        case NODE_BLOCK: for(int i=0;i<node->as.block.count;i++)ast_free(node->as.block.statements[i]); free(node->as.block.statements); break;
        case NODE_IMPORT: free(node->as.import_stmt.path); break;
        case NODE_IMPORT_NATIVE: free(node->as.import_native.path); break;
        case NODE_STRUCT_DECL: free(node->as.struct_decl.name); for(int i=0;i<node->as.struct_decl.field_count;i++)free(node->as.struct_decl.field_names[i]); free(node->as.struct_decl.field_names); break;
        case NODE_IMPL_DECL: free(node->as.impl_decl.struct_name); for(int i=0;i<node->as.impl_decl.method_count;i++)ast_free(node->as.impl_decl.methods[i]); free(node->as.impl_decl.methods); break;
        case NODE_TRAIT_DECL: free(node->as.trait_decl.name); for(int i=0;i<node->as.trait_decl.method_count;i++)ast_free(node->as.trait_decl.methods[i]); free(node->as.trait_decl.methods); break;
        case NODE_IMPL_TRAIT_DECL: free(node->as.impl_trait_decl.trait_name); free(node->as.impl_trait_decl.struct_name); for(int i=0;i<node->as.impl_trait_decl.method_count;i++)ast_free(node->as.impl_trait_decl.methods[i]); free(node->as.impl_trait_decl.methods); break;
        case NODE_TRY_STMT: ast_free(node->as.try_stmt.try_block);free(node->as.try_stmt.catch_var);ast_free(node->as.try_stmt.catch_block); break;
        case NODE_THROW_STMT: ast_free(node->as.throw_stmt.value); break;
        case NODE_MEMBER_ASSIGN: ast_free(node->as.member_assign.object);free(node->as.member_assign.member);ast_free(node->as.member_assign.value); break;
        case NODE_INDEX_ASSIGN: ast_free(node->as.index_assign.object);ast_free(node->as.index_assign.index);ast_free(node->as.index_assign.value); break;
        case NODE_PROGRAM: for(int i=0;i<node->as.program.count;i++)ast_free(node->as.program.declarations[i]); free(node->as.program.declarations); break;
    }
    free(node);
}
