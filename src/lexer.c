/*
 * Riz Programming Language
 * lexer.c — Tokenizer implementation
 */

#include "lexer.h"

/* ═══════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════ */

static bool is_at_end(Lexer* L) {
    return *L->current == '\0';
}

static char advance(Lexer* L) {
    L->current++;
    return L->current[-1];
}

static char peek(Lexer* L) {
    return *L->current;
}

static char peek_next(Lexer* L) {
    if (is_at_end(L)) return '\0';
    return L->current[1];
}

static bool match_char(Lexer* L, char expected) {
    if (is_at_end(L)) return false;
    if (*L->current != expected) return false;
    L->current++;
    return true;
}

static Token make_token(Lexer* L, TokenType type) {
    Token t;
    t.type = type;
    t.start = L->start;
    t.length = (int)(L->current - L->start);
    t.line = L->line;
    return t;
}

static Token error_token(Lexer* L, const char* message) {
    Token t;
    t.type = TOK_ERROR;
    t.start = message;
    t.length = (int)strlen(message);
    t.line = L->line;
    return t;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* ═══════════════════════════════════════════════════════
 *  Skip whitespace and comments
 * ═══════════════════════════════════════════════════════ */

static void skip_whitespace(Lexer* L) {
    for (;;) {
        char c = peek(L);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(L);
                break;
            case '\n':
                L->line++;
                advance(L);
                break;
            case '#':
                /* Single-line comment: skip until end of line */
                while (peek(L) != '\n' && !is_at_end(L)) advance(L);
                break;
            default:
                return;
        }
    }
}

/* ═══════════════════════════════════════════════════════
 *  Keywords
 * ═══════════════════════════════════════════════════════ */

typedef struct {
    const char* keyword;
    int         length;
    TokenType   type;
} Keyword;

static const Keyword keywords[] = {
    { "let",      3,  TOK_LET },
    { "mut",      3,  TOK_MUT },
    { "fn",       2,  TOK_FN },
    { "return",   6,  TOK_RETURN },
    { "if",       2,  TOK_IF },
    { "else",     4,  TOK_ELSE },
    { "while",    5,  TOK_WHILE },
    { "for",      3,  TOK_FOR },
    { "in",       2,  TOK_IN },
    { "true",     4,  TOK_TRUE },
    { "false",    5,  TOK_FALSE },
    { "none",     4,  TOK_NONE },
    { "and",      3,  TOK_AND },
    { "or",       2,  TOK_OR },
    { "not",      3,  TOK_NOT },
    { "match",    5,  TOK_MATCH },
    { "struct",   6,  TOK_STRUCT },
    { "impl",     4,  TOK_IMPL },
    { "trait",    5,  TOK_TRAIT },
    { "async",    5,  TOK_ASYNC },
    { "await",    5,  TOK_AWAIT },
    { "import",        6,  TOK_IMPORT },
    { "import_native", 13,  TOK_IMPORT_NATIVE },
    { "break",          5,  TOK_BREAK },
    { "continue", 8,  TOK_CONTINUE },
    { "try",      3,  TOK_TRY },
    { "catch",    5,  TOK_CATCH },
    { "throw",    5,  TOK_THROW },
    { NULL,       0,  TOK_ERROR },
};

static TokenType check_keyword(const char* start, int length) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (length == keywords[i].length &&
            memcmp(start, keywords[i].keyword, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENTIFIER;
}

/* ═══════════════════════════════════════════════════════
 *  Scan specific token types
 * ═══════════════════════════════════════════════════════ */

static Token scan_string(Lexer* L) {
    while (peek(L) != '"' && !is_at_end(L)) {
        if (peek(L) == '\n') L->line++;
        if (peek(L) == '\\') advance(L);  /* skip escape char */
        advance(L);
    }
    if (is_at_end(L)) return error_token(L, "Unterminated string");
    advance(L);  /* closing " */
    return make_token(L, TOK_STRING);
}

static Token scan_number(Lexer* L) {
    while (is_digit(peek(L))) advance(L);

    /* Check for float */
    if (peek(L) == '.' && is_digit(peek_next(L))) {
        advance(L);  /* consume '.' */
        while (is_digit(peek(L))) advance(L);

        /* Scientific notation */
        if (peek(L) == 'e' || peek(L) == 'E') {
            advance(L);
            if (peek(L) == '+' || peek(L) == '-') advance(L);
            while (is_digit(peek(L))) advance(L);
        }
        return make_token(L, TOK_FLOAT);
    }

    /* Scientific notation for integers → becomes float */
    if (peek(L) == 'e' || peek(L) == 'E') {
        advance(L);
        if (peek(L) == '+' || peek(L) == '-') advance(L);
        while (is_digit(peek(L))) advance(L);
        return make_token(L, TOK_FLOAT);
    }

    return make_token(L, TOK_INT);
}

static Token scan_identifier(Lexer* L) {
    while (is_alpha(peek(L)) || is_digit(peek(L))) advance(L);
    int length = (int)(L->current - L->start);
    TokenType type = check_keyword(L->start, length);
    return make_token(L, type);
}

/* ═══════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════ */

void lexer_init(Lexer* lexer, const char* source) {
    lexer->source = source;
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

Token lexer_next_token(Lexer* L) {
    skip_whitespace(L);
    L->start = L->current;

    if (is_at_end(L)) return make_token(L, TOK_EOF);

    char c = advance(L);

    /* Identifiers & keywords */
    if (is_alpha(c)) return scan_identifier(L);

    /* Numbers */
    if (is_digit(c)) return scan_number(L);

    switch (c) {
        /* Single-character tokens */
        case '(': return make_token(L, TOK_LPAREN);
        case ')': return make_token(L, TOK_RPAREN);
        case '{': return make_token(L, TOK_LBRACE);
        case '}': return make_token(L, TOK_RBRACE);
        case '[': return make_token(L, TOK_LBRACKET);
        case ']': return make_token(L, TOK_RBRACKET);
        case ',': return make_token(L, TOK_COMMA);
        case '.': return make_token(L, TOK_DOT);
        case ':': return make_token(L, TOK_COLON);
        case '%': return make_token(L, TOK_PERCENT);

        /* One or two character tokens */
        case '+':
            if (match_char(L, '=')) return make_token(L, TOK_PLUS_ASSIGN);
            return make_token(L, TOK_PLUS);

        case '-':
            if (match_char(L, '>')) return make_token(L, TOK_ARROW);
            if (match_char(L, '=')) return make_token(L, TOK_MINUS_ASSIGN);
            return make_token(L, TOK_MINUS);

        case '*':
            if (match_char(L, '*')) return make_token(L, TOK_POWER);
            if (match_char(L, '=')) return make_token(L, TOK_STAR_ASSIGN);
            return make_token(L, TOK_STAR);

        case '/':
            if (match_char(L, '/')) return make_token(L, TOK_FLOOR_DIV);
            if (match_char(L, '=')) return make_token(L, TOK_SLASH_ASSIGN);
            return make_token(L, TOK_SLASH);

        case '=':
            if (match_char(L, '=')) return make_token(L, TOK_EQ);
            if (match_char(L, '>')) return make_token(L, TOK_FAT_ARROW);
            return make_token(L, TOK_ASSIGN);

        case '!':
            if (match_char(L, '=')) return make_token(L, TOK_NEQ);
            return make_token(L, TOK_BANG);

        case '<':
            if (match_char(L, '=')) return make_token(L, TOK_LTE);
            return make_token(L, TOK_LT);

        case '>':
            if (match_char(L, '=')) return make_token(L, TOK_GTE);
            return make_token(L, TOK_GT);

        case '|':
            if (match_char(L, '>')) return make_token(L, TOK_PIPE);
            return error_token(L, "Unexpected character '|'. Did you mean '|>'?");

        /* Strings */
        case '"': return scan_string(L);
    }

    return error_token(L, "Unexpected character");
}

/* ═══════════════════════════════════════════════════════
 *  Debug: token type names
 * ═══════════════════════════════════════════════════════ */

const char* token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT:           return "INT";
        case TOK_FLOAT:         return "FLOAT";
        case TOK_STRING:        return "STRING";
        case TOK_IDENTIFIER:    return "IDENTIFIER";
        case TOK_LPAREN:        return "(";
        case TOK_RPAREN:        return ")";
        case TOK_LBRACE:        return "{";
        case TOK_RBRACE:        return "}";
        case TOK_LBRACKET:      return "[";
        case TOK_RBRACKET:      return "]";
        case TOK_COMMA:         return ",";
        case TOK_DOT:           return ".";
        case TOK_COLON:         return ":";
        case TOK_PLUS:          return "+";
        case TOK_MINUS:         return "-";
        case TOK_STAR:          return "*";
        case TOK_SLASH:         return "/";
        case TOK_PERCENT:       return "%";
        case TOK_ASSIGN:        return "=";
        case TOK_EQ:            return "==";
        case TOK_BANG:          return "!";
        case TOK_NEQ:           return "!=";
        case TOK_LT:            return "<";
        case TOK_LTE:           return "<=";
        case TOK_GT:            return ">";
        case TOK_GTE:           return ">=";
        case TOK_ARROW:         return "->";
        case TOK_FAT_ARROW:     return "=>";
        case TOK_PIPE:          return "|>";
        case TOK_POWER:         return "**";
        case TOK_FLOOR_DIV:     return "//";
        case TOK_PLUS_ASSIGN:   return "+=";
        case TOK_MINUS_ASSIGN:  return "-=";
        case TOK_STAR_ASSIGN:   return "*=";
        case TOK_SLASH_ASSIGN:  return "/=";
        case TOK_LET:           return "let";
        case TOK_MUT:           return "mut";
        case TOK_FN:            return "fn";
        case TOK_RETURN:        return "return";
        case TOK_IF:            return "if";
        case TOK_ELSE:          return "else";
        case TOK_WHILE:         return "while";
        case TOK_FOR:           return "for";
        case TOK_IN:            return "in";
        case TOK_TRUE:          return "true";
        case TOK_FALSE:         return "false";
        case TOK_NONE:          return "none";
        case TOK_AND:           return "and";
        case TOK_OR:            return "or";
        case TOK_NOT:           return "not";
        case TOK_MATCH:         return "match";
        case TOK_STRUCT:        return "struct";
        case TOK_IMPL:          return "impl";
        case TOK_TRAIT:         return "trait";
        case TOK_ASYNC:         return "async";
        case TOK_AWAIT:         return "await";
        case TOK_IMPORT:        return "import";
        case TOK_IMPORT_NATIVE: return "import_native";
        case TOK_BREAK:         return "break";
        case TOK_CONTINUE:      return "continue";
        case TOK_TRY:           return "try";
        case TOK_CATCH:         return "catch";
        case TOK_THROW:         return "throw";
        case TOK_NEWLINE:       return "NEWLINE";
        case TOK_EOF:           return "EOF";
        case TOK_ERROR:         return "ERROR";
    }
    return "?";
}
