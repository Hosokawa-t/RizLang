/*
 * Riz Programming Language
 * ast.h — Abstract Syntax Tree node definitions
 */

#ifndef RIZ_AST_H
#define RIZ_AST_H

#include "common.h"
#include "lexer.h"

/* Forward declaration */
typedef struct ASTNode ASTNode;

/* ─── AST Node Types ──────────────────────────────────── */
typedef enum {
    /* Literals */
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STRING_LIT,
    NODE_BOOL_LIT,
    NODE_NONE_LIT,
    NODE_LIST_LIT,
    NODE_DICT_LIT,

    /* Expressions */
    NODE_IDENTIFIER,
    NODE_UNARY,
    NODE_BINARY,
    NODE_CALL,
    NODE_INDEX,
    NODE_ASSIGN,
    NODE_COMPOUND_ASSIGN,
    NODE_MEMBER,
    NODE_PIPE,
    NODE_LAMBDA,
    NODE_MATCH_EXPR,

    /* Statements */
    NODE_EXPR_STMT,
    NODE_LET_DECL,
    NODE_FN_DECL,
    NODE_RETURN_STMT,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_BLOCK,
    NODE_IMPORT,
    NODE_IMPORT_NATIVE,
    NODE_STRUCT_DECL,
    NODE_IMPL_DECL,
    NODE_TRAIT_DECL,
    NODE_IMPL_TRAIT_DECL,
    NODE_TRY_STMT,
    NODE_THROW_STMT,
    NODE_MEMBER_ASSIGN,
    NODE_INDEX_ASSIGN,

    /* Top-level */
    NODE_PROGRAM,
} NodeType;

/* ─── Match Arm ───────────────────────────────────────── */
typedef struct {
    ASTNode* pattern;
    ASTNode* guard;     /* NULL or conditional expression */
    ASTNode* body;      /* expression or block */
} RizMatchArm;

/* ─── AST Node ────────────────────────────────────────── */
struct ASTNode {
    NodeType type;
    int      line;

    union {
        /* NODE_INT_LIT */
        struct { int64_t value; } int_lit;

        /* NODE_FLOAT_LIT */
        struct { double value; } float_lit;

        /* NODE_STRING_LIT */
        struct { char* value; } string_lit;

        /* NODE_BOOL_LIT */
        struct { bool value; } bool_lit;

        /* NODE_LIST_LIT */
        struct {
            ASTNode** items;
            int       count;
        } list_lit;

        /* NODE_DICT_LIT */
        struct {
            ASTNode** keys;
            ASTNode** values;
            int       count;
        } dict_lit;

        /* NODE_IDENTIFIER */
        struct { char* name; } identifier;

        /* NODE_UNARY */
        struct {
            TokenType op;
            ASTNode*  operand;
        } unary;

        /* NODE_BINARY */
        struct {
            TokenType op;
            ASTNode*  left;
            ASTNode*  right;
        } binary;

        /* NODE_CALL */
        struct {
            ASTNode*  callee;
            ASTNode** args;
            int       arg_count;
        } call;

        /* NODE_INDEX */
        struct {
            ASTNode* object;
            ASTNode* index;
        } index_expr;

        /* NODE_ASSIGN */
        struct {
            char*    name;
            ASTNode* value;
        } assign;

        /* NODE_COMPOUND_ASSIGN */
        struct {
            char*     name;
            TokenType op;        /* TOK_PLUS, TOK_MINUS, etc */
            ASTNode*  value;
        } compound_assign;

        /* NODE_MEMBER */
        struct {
            ASTNode* object;
            char*    member;
        } member;

        /* NODE_PIPE */
        struct {
            ASTNode* left;
            ASTNode* right;      /* will be a call node */
        } pipe;

        /* NODE_LAMBDA */
        struct {
            char**   params;
            int      param_count;
            ASTNode* body;
        } lambda;

        /* NODE_MATCH_EXPR */
        struct {
            ASTNode*     subject;
            RizMatchArm* arms;
            int          arm_count;
        } match_expr;

        /* NODE_EXPR_STMT */
        struct {
            ASTNode* expr;
        } expr_stmt;

        /* NODE_LET_DECL */
        struct {
            char*    name;
            char*    type_annotation;
            ASTNode* initializer;
            bool     is_mutable;
        } let_decl;

        /* NODE_FN_DECL */
        struct {
            char*    name;
            char**   params;
            int      param_count;
            char*    return_type;
            ASTNode* body;
            ASTNode** param_defaults; /* NULL = no defaults; entries: NULL=required, non-NULL=default expr */
        } fn_decl;

        /* NODE_RETURN_STMT */
        struct {
            ASTNode* value;
        } return_stmt;

        /* NODE_IF_STMT */
        struct {
            ASTNode* condition;
            ASTNode* then_branch;
            ASTNode* else_branch;  /* NULL or if_stmt or block */
        } if_stmt;

        /* NODE_WHILE_STMT */
        struct {
            ASTNode* condition;
            ASTNode* body;
        } while_stmt;

        /* NODE_FOR_STMT */
        struct {
            char*    var_name;
            ASTNode* iterable;
            ASTNode* body;
        } for_stmt;

        /* NODE_BLOCK */
        struct {
            ASTNode** statements;
            int       count;
        } block;

        /* NODE_IMPORT */
        struct {
            char* path;
        } import_stmt;

        /* NODE_IMPORT_NATIVE */
        struct {
            char* path;
        } import_native;

        /* NODE_STRUCT_DECL */
        struct {
            char*  name;
            char** field_names;
            int    field_count;
        } struct_decl;

        /* NODE_IMPL_DECL */
        struct {
            char*     struct_name;
            ASTNode** methods;
            int       method_count;
        } impl_decl;

        /* NODE_TRAIT_DECL */
        struct {
            char*     name;
            ASTNode** methods; /* method signatures */
            int       method_count;
        } trait_decl;

        /* NODE_IMPL_TRAIT_DECL */
        struct {
            char*     trait_name;
            char*     struct_name;
            ASTNode** methods;
            int       method_count;
        } impl_trait_decl;

        /* NODE_TRY_STMT */
        struct {
            ASTNode* try_block;
            char*    catch_var;
            ASTNode* catch_block;
        } try_stmt;

        /* NODE_THROW_STMT */
        struct {
            ASTNode* value;
        } throw_stmt;

        /* NODE_MEMBER_ASSIGN */
        struct {
            ASTNode* object;
            char*    member;
            ASTNode* value;
        } member_assign;

        /* NODE_INDEX_ASSIGN */
        struct {
            ASTNode* object;
            ASTNode* index;
            ASTNode* value;
        } index_assign;

        /* NODE_PROGRAM */
        struct {
            ASTNode** declarations;
            int       count;
        } program;
    } as;
};

/* ─── Constructors ────────────────────────────────────── */
ASTNode* ast_int_lit(int64_t value, int line);
ASTNode* ast_float_lit(double value, int line);
ASTNode* ast_string_lit(const char* value, int line);
ASTNode* ast_bool_lit(bool value, int line);
ASTNode* ast_none_lit(int line);
ASTNode* ast_list_lit(ASTNode** items, int count, int line);
ASTNode* ast_dict_lit(ASTNode** keys, ASTNode** values, int count, int line);
ASTNode* ast_identifier(const char* name, int line);
ASTNode* ast_unary(TokenType op, ASTNode* operand, int line);
ASTNode* ast_binary(TokenType op, ASTNode* left, ASTNode* right, int line);
ASTNode* ast_call(ASTNode* callee, ASTNode** args, int arg_count, int line);
ASTNode* ast_index(ASTNode* object, ASTNode* index, int line);
ASTNode* ast_assign(const char* name, ASTNode* value, int line);
ASTNode* ast_compound_assign(const char* name, TokenType op, ASTNode* value, int line);
ASTNode* ast_member(ASTNode* object, const char* member, int line);
ASTNode* ast_pipe(ASTNode* left, ASTNode* right, int line);
ASTNode* ast_lambda(char** params, int param_count, ASTNode* body, int line);
ASTNode* ast_match_expr(ASTNode* subject, RizMatchArm* arms, int arm_count, int line);
ASTNode* ast_expr_stmt(ASTNode* expr, int line);
ASTNode* ast_let_decl(const char* name, const char* type_ann, ASTNode* init, bool mutable, int line);
ASTNode* ast_fn_decl(const char* name, char** params, int param_count, ASTNode** defaults, const char* ret_type, ASTNode* body, int line);
ASTNode* ast_return_stmt(ASTNode* value, int line);
ASTNode* ast_if_stmt(ASTNode* cond, ASTNode* then_b, ASTNode* else_b, int line);
ASTNode* ast_while_stmt(ASTNode* cond, ASTNode* body, int line);
ASTNode* ast_for_stmt(const char* var_name, ASTNode* iterable, ASTNode* body, int line);
ASTNode* ast_break_stmt(int line);
ASTNode* ast_continue_stmt(int line);
ASTNode* ast_block(ASTNode** stmts, int count, int line);
ASTNode* ast_import(const char* path, int line);
ASTNode* ast_import_native(const char* path, int line);
ASTNode* ast_struct_decl(const char* name, char** fields, int field_count, int line);
ASTNode* ast_impl_decl(const char* name, ASTNode** methods, int method_count, int line);
ASTNode* ast_trait_decl(const char* name, ASTNode** methods, int method_count, int line);
ASTNode* ast_impl_trait_decl(const char* trait_name, const char* struct_name, ASTNode** methods, int method_count, int line);
ASTNode* ast_try_stmt(ASTNode* try_block, const char* catch_var, ASTNode* catch_block, int line);
ASTNode* ast_throw_stmt(ASTNode* value, int line);
ASTNode* ast_member_assign(ASTNode* object, char* member, ASTNode* value, int line);
ASTNode* ast_index_assign(ASTNode* object, ASTNode* index, ASTNode* value, int line);
ASTNode* ast_program(ASTNode** decls, int count);

/* ─── Destructor ──────────────────────────────────────── */
void ast_free(ASTNode* node);

#endif /* RIZ_AST_H */
