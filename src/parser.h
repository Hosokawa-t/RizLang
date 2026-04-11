/*
 * Riz Programming Language
 * parser.h — Recursive descent parser interface
 */

#ifndef RIZ_PARSER_H
#define RIZ_PARSER_H

#include "lexer.h"
#include "ast.h"

/* ─── Parser State ────────────────────────────────────── */
typedef struct {
    Lexer*  lexer;
    Token   current;
    Token   previous;
    bool    had_error;
    bool    panic_mode;
} Parser;

/* ─── API ─────────────────────────────────────────────── */

/* Initialize the parser with a lexer */
void parser_init(Parser* parser, Lexer* lexer);

/* Parse an entire program → returns NODE_PROGRAM */
ASTNode* parser_parse(Parser* parser);

/* Parse a single expression (useful for REPL) */
ASTNode* parser_parse_expression(Parser* parser);

#endif /* RIZ_PARSER_H */
