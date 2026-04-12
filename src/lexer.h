/*
 * Riz Programming Language
 * lexer.h — Lexical analysis (tokenizer) interface
 */

#ifndef RIZ_LEXER_H
#define RIZ_LEXER_H

#include "common.h"

/* ─── Token Types ─────────────────────────────────────── */
typedef enum {
    /* Literals */
    TOK_INT, TOK_FLOAT, TOK_STRING, TOK_IDENTIFIER,

    /* Single-character tokens */
    TOK_LPAREN, TOK_RPAREN,       /* ( ) */
    TOK_LBRACE, TOK_RBRACE,       /* { } */
    TOK_LBRACKET, TOK_RBRACKET,   /* [ ] */
    TOK_COMMA,                     /* , */
    TOK_DOT,                       /* . */
    TOK_COLON,                     /* : */
    TOK_PLUS,                      /* + */
    TOK_MINUS,                     /* - */
    TOK_STAR,                      /* * */
    TOK_SLASH,                     /* / */
    TOK_PERCENT,                   /* % */

    /* One or two character tokens */
    TOK_ASSIGN,                    /* = */
    TOK_EQ,                        /* == */
    TOK_BANG,                      /* ! */
    TOK_NEQ,                       /* != */
    TOK_LT,                        /* < */
    TOK_LTE,                       /* <= */
    TOK_GT,                        /* > */
    TOK_GTE,                       /* >= */
    TOK_ARROW,                     /* -> */
    TOK_FAT_ARROW,                 /* => */
    TOK_PIPE,                      /* |> */
    TOK_POWER,                     /* ** */
    TOK_FLOOR_DIV,                 /* // (integer division) */
    TOK_PLUS_ASSIGN,               /* += */
    TOK_MINUS_ASSIGN,              /* -= */
    TOK_STAR_ASSIGN,               /* *= */
    TOK_SLASH_ASSIGN,              /* /= */

    /* Keywords */
    TOK_LET, TOK_MUT,
    TOK_FN, TOK_RETURN,
    TOK_IF, TOK_ELSE,
    TOK_WHILE, TOK_FOR, TOK_IN,
    TOK_TRUE, TOK_FALSE, TOK_NONE,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_MATCH,
    TOK_STRUCT, TOK_IMPL, TOK_TRAIT,
    TOK_ASYNC, TOK_AWAIT,
    TOK_IMPORT_PYTHON, TOK_IMPORT, TOK_IMPORT_NATIVE, TOK_BREAK, TOK_CONTINUE,
    TOK_TRY, TOK_CATCH, TOK_THROW,

    /* Special */
    TOK_NEWLINE,
    TOK_EOF,
    TOK_ERROR,
} TokenType;

/* ─── Token ───────────────────────────────────────────── */
typedef struct {
    TokenType   type;
    const char* start;      /* pointer into source string */
    int         length;
    int         line;
    int         column;     /* 0-based UTF-8 byte offset in line (machine diagnostics) */
} Token;

/* ─── Lexer State ─────────────────────────────────────── */
typedef struct {
    const char* source;
    const char* start;
    const char* current;
    const char* line_start; /* start of current line in source */
    int         line;
} Lexer;

/* ─── API ─────────────────────────────────────────────── */
void  lexer_init(Lexer* lexer, const char* source);
Token lexer_next_token(Lexer* lexer);

/* Utility: get the string name for a token type */
const char* token_type_name(TokenType type);

#endif /* RIZ_LEXER_H */
