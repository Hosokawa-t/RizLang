/*
 * Riz Programming Language
 * parser.c — Recursive descent parser (Phase 3: +struct, impl, try/catch, defaults)
 */

#include "parser.h"
#include "diagnostic.h"

/* ═══ Helpers ═══ */

static void advance_parser(Parser* P) {
    P->previous = P->current;
    for (;;) {
        P->current = lexer_next_token(P->lexer);
        if (P->current.type != TOK_ERROR) break;
        {
            int ec = P->current.column + (P->current.length > 0 ? P->current.length : 1);
            riz_error_col(P->current.line, P->current.column, ec, "%.*s", P->current.length, P->current.start);
        }
        P->had_error = true;
    }
}
static bool check(Parser* P, TokenType type) { return P->current.type == type; }
static bool match(Parser* P, TokenType type) {
    if (!check(P, type)) return false;
    advance_parser(P); return true;
}
static Token consume(Parser* P, TokenType type, const char* msg) {
    if (P->current.type == type) { advance_parser(P); return P->previous; }
    riz_error_col(P->current.line, P->current.column, P->current.column + P->current.length,
                   "%s (got '%.*s')", msg, P->current.length, P->current.start);
    P->had_error = true;
    Token t = P->current; advance_parser(P); return t;
}
static char* token_text(Token t) { return riz_strndup(t.start, t.length); }

static char* parse_string_literal(Token t) {
    int offset = 1;  /* skip opening " */
    int trim_end = 2; /* skip opening " and closing " */
    /* Detect triple-quoted string: starts with """ */
    if (t.length >= 6 && t.start[0] == '"' && t.start[1] == '"' && t.start[2] == '"') {
        offset = 3; trim_end = 6;
    }
    const char* s = t.start + offset; int raw = t.length - trim_end;
    char* r = (char*)malloc(raw + 1); int j = 0;
    for (int i = 0; i < raw; i++) {
        if (s[i] == '\\' && i+1 < raw) {
            i++;
            switch(s[i]) {
                case 'n': r[j++]='\n'; break; case 't': r[j++]='\t'; break;
                case 'r': r[j++]='\r'; break; case '\\': r[j++]='\\'; break;
                case '"': r[j++]='"'; break;  case '0': r[j++]='\0'; break;
                default: r[j++]='\\'; r[j++]=s[i]; break;
            }
        } else r[j++] = s[i];
    }
    r[j] = '\0'; return r;
}

/* ═══ Forward declarations ═══ */

static ASTNode* parse_declaration(Parser* P);
static ASTNode* parse_statement(Parser* P);
static ASTNode* parse_expression(Parser* P);
static ASTNode* parse_assignment(Parser* P);
static ASTNode* parse_pipe(Parser* P);
static ASTNode* parse_or(Parser* P);
static ASTNode* parse_and(Parser* P);
static ASTNode* parse_equality(Parser* P);
static ASTNode* parse_comparison(Parser* P);
static ASTNode* parse_term(Parser* P);
static ASTNode* parse_factor(Parser* P);
static ASTNode* parse_power(Parser* P);
static ASTNode* parse_unary(Parser* P);
static ASTNode* parse_call(Parser* P);
static ASTNode* parse_primary(Parser* P);
static ASTNode* parse_block(Parser* P);
static ASTNode* parse_fn_decl(Parser* P);

/* ═══ Match expression ═══ */

static ASTNode* parse_match_expr(Parser* P) {
    int line = P->previous.line;
    ASTNode* subject = parse_expression(P);
    consume(P, TOK_LBRACE, "Expected '{' after match subject");
    int cap = 8, count = 0;
    RizMatchArm* arms = RIZ_ALLOC_ARRAY(RizMatchArm, cap);
    while (!check(P, TOK_RBRACE) && !check(P, TOK_EOF)) {
        if (count >= cap) { cap *= 2; arms = RIZ_GROW_ARRAY(RizMatchArm, arms, count, cap); }
        ASTNode* pattern = parse_primary(P);
        ASTNode* guard = NULL;
        if (match(P, TOK_IF)) guard = parse_expression(P);
        consume(P, TOK_FAT_ARROW, "Expected '=>' after match pattern");
        ASTNode* body;
        if (check(P, TOK_LBRACE)) body = parse_block(P);
        else body = parse_expression(P);
        arms[count].pattern = pattern;
        arms[count].guard = guard;
        arms[count].body = body;
        count++;
        match(P, TOK_COMMA);
    }
    consume(P, TOK_RBRACE, "Expected '}' after match arms");
    return ast_match_expr(subject, arms, count, line);
}

/* ═══ Dict literal ═══ */

static ASTNode* parse_dict_literal(Parser* P) {
    int line = P->previous.line;
    int cap = 8, count = 0;
    ASTNode** keys   = RIZ_ALLOC_ARRAY(ASTNode*, cap);
    ASTNode** values = RIZ_ALLOC_ARRAY(ASTNode*, cap);
    if (!check(P, TOK_RBRACE)) {
        do {
            if (count >= cap) {
                cap *= 2;
                keys   = RIZ_GROW_ARRAY(ASTNode*, keys,   count, cap);
                values = RIZ_GROW_ARRAY(ASTNode*, values, count, cap);
            }
            ASTNode* key;
            if (check(P, TOK_IDENTIFIER)) {
                Token id = P->current;
                advance_parser(P);
                if (check(P, TOK_COLON)) {
                    char* name = token_text(id);
                    key = ast_string_lit(name, id.line);
                    free(name);
                } else {
                    riz_error_col(id.line, id.column, id.column + id.length, "Expected ':' after key in dict literal");
                    P->had_error = true;
                    char* name = token_text(id);
                    key = ast_string_lit(name, id.line);
                    free(name);
                }
            } else {
                key = parse_expression(P);
            }
            consume(P, TOK_COLON, "Expected ':' in dict entry");
            ASTNode* value = parse_expression(P);
            keys[count] = key;
            values[count] = value;
            count++;
        } while (match(P, TOK_COMMA) && !check(P, TOK_RBRACE));
    }
    consume(P, TOK_RBRACE, "Expected '}' closing dict literal");
    return ast_dict_lit(keys, values, count, line);
}

/* ═══ F-string to format() AST helper ═══ */

static ASTNode* parse_fstring(Parser* P, Token fstr_tok) {
    /* f"Hello {expr} world {expr2}" -> format("Hello {} world {}", expr, expr2) */
    /* Token starts with f" and ends with ", so skip 2 chars from start, 1 from end */
    const char* s = fstr_tok.start + 2; int raw_len = fstr_tok.length - 3;
    /* Process escape sequences like regular strings */
    char* raw = (char*)malloc(raw_len + 1); int rj = 0;
    for (int i = 0; i < raw_len; i++) {
        if (s[i] == '\\' && i+1 < raw_len) {
            i++;
            switch(s[i]) {
                case 'n': raw[rj++]='\n'; break; case 't': raw[rj++]='\t'; break;
                case 'r': raw[rj++]='\r'; break; case '\\': raw[rj++]='\\'; break;
                case '"': raw[rj++]='"'; break;  case '0': raw[rj++]='\0'; break;
                default: raw[rj++]='\\'; raw[rj++]=s[i]; break;
            }
        } else raw[rj++] = s[i];
    }
    raw[rj] = '\0';
    int line = fstr_tok.line;
    /* Build template and extract expressions */
    size_t rlen = strlen(raw);
    char* tmpl = (char*)malloc(rlen + 1);
    int tmpl_len = 0;
    int cap = 4, argc = 1; /* first arg is the template string */
    ASTNode** args = RIZ_ALLOC_ARRAY(ASTNode*, cap);
    for (size_t i = 0; i < rlen; i++) {
        if (raw[i] == '{' && i + 1 < rlen && raw[i+1] != '{') {
            /* Found interpolation: extract inner expression text */
            tmpl[tmpl_len++] = '{'; tmpl[tmpl_len++] = '}';
            i++; /* skip '{' */
            size_t start = i;
            int depth = 1;
            while (i < rlen && depth > 0) {
                if (raw[i] == '{') depth++;
                else if (raw[i] == '}') depth--;
                if (depth > 0) i++;
            }
            /* raw[start..i) is the expression text */
            size_t expr_len = i - start;
            char* expr_src = (char*)malloc(expr_len + 1);
            memcpy(expr_src, raw + start, expr_len);
            expr_src[expr_len] = '\0';
            /* Parse the expression */
            Lexer expr_lex; lexer_init(&expr_lex, expr_src);
            Parser expr_P; parser_init(&expr_P, &expr_lex);
            ASTNode* expr = parser_parse_expression(&expr_P);
            free(expr_src);
            if (argc >= cap) { cap *= 2; args = RIZ_GROW_ARRAY(ASTNode*, args, argc, cap); }
            args[argc++] = expr;
        } else if (raw[i] == '{' && i + 1 < rlen && raw[i+1] == '{') {
            tmpl[tmpl_len++] = '{'; i++; /* escaped {{ -> { */
        } else if (raw[i] == '}' && i + 1 < rlen && raw[i+1] == '}') {
            tmpl[tmpl_len++] = '}'; i++; /* escaped }} -> } */
        } else {
            tmpl[tmpl_len++] = raw[i];
        }
    }
    tmpl[tmpl_len] = '\0';
    args[0] = ast_string_lit(tmpl, line);
    free(tmpl); free(raw);
    ASTNode* callee = ast_identifier("format", line);
    return ast_call(callee, args, argc, line);
}

/* ═══ Primary ═══ */

static ASTNode* parse_primary(Parser* P) {
    int line = P->current.line;
    if (match(P, TOK_INT)) { char*t=token_text(P->previous); int64_t v=strtoll(t,NULL,10);free(t); return ast_int_lit(v,line); }
    if (match(P, TOK_FLOAT)) { char*t=token_text(P->previous); double v=strtod(t,NULL);free(t); return ast_float_lit(v,line); }
    if (match(P, TOK_STRING)) { char*v=parse_string_literal(P->previous); ASTNode*n=ast_string_lit(v,line);free(v); return n; }
    if (match(P, TOK_FSTRING)) { return parse_fstring(P, P->previous); }
    if (match(P, TOK_TRUE))  return ast_bool_lit(true, line);
    if (match(P, TOK_FALSE)) return ast_bool_lit(false, line);
    if (match(P, TOK_NONE))  return ast_none_lit(line);
    if (match(P, TOK_IDENTIFIER)) { char*name=token_text(P->previous); ASTNode*n=ast_identifier(name,line);free(name); return n; }
    if (match(P, TOK_LPAREN)) { ASTNode*expr=parse_expression(P); consume(P,TOK_RPAREN,"Expected ')'"); return expr; }
    /* List literal or list comprehension */
    if (match(P, TOK_LBRACKET)) {
        if (check(P, TOK_RBRACKET)) { consume(P, TOK_RBRACKET, "Expected ']'"); return ast_list_lit(NULL, 0, line); }
        /* Parse first expression, then check if 'for' follows → list comprehension */
        ASTNode* first = parse_or(P);
        if (check(P, TOK_FOR)) {
            /* List comprehension: [expr for var in iter] or [expr for var in iter if cond] */
            consume(P, TOK_FOR, "Expected 'for'");
            Token vt = consume(P, TOK_IDENTIFIER, "Expected variable name"); char* vn = token_text(vt);
            consume(P, TOK_IN, "Expected 'in'");
            ASTNode* iter = parse_or(P);
            ASTNode* cond = NULL;
            if (match(P, TOK_IF)) cond = parse_or(P);
            consume(P, TOK_RBRACKET, "Expected ']'");
            ASTNode* n = ast_list_comp(first, vn, iter, cond, line);
            free(vn); return n;
        }
        /* Regular list literal */
        int cap=8,cnt=1; ASTNode** items=RIZ_ALLOC_ARRAY(ASTNode*,cap); items[0]=first;
        while(match(P,TOK_COMMA)){if(check(P,TOK_RBRACKET))break;if(cnt>=cap){cap*=2;items=RIZ_GROW_ARRAY(ASTNode*,items,cnt,cap);}items[cnt++]=parse_expression(P);}
        consume(P,TOK_RBRACKET,"Expected ']'"); return ast_list_lit(items,cnt,line);
    }
    /* Dict literal — peek-ahead disambiguation */
    if (check(P, TOK_LBRACE)) {
        Lexer saved_lexer = *P->lexer;
        Token saved_current = P->current;
        Token saved_previous = P->previous;
        advance_parser(P);
        bool is_dict = false;
        if (check(P, TOK_RBRACE)) { is_dict = true; }
        else if (check(P,TOK_IDENTIFIER)||check(P,TOK_STRING)||check(P,TOK_INT)||check(P,TOK_FLOAT)) {
            advance_parser(P);
            if (check(P, TOK_COLON)) is_dict = true;
        }
        *P->lexer = saved_lexer; P->current = saved_current; P->previous = saved_previous;
        if (is_dict) { advance_parser(P); return parse_dict_literal(P); }
    }
    /* Match expression */
    if (match(P, TOK_MATCH)) return parse_match_expr(P);
    /* '{' that isn't a dict */
    if (check(P, TOK_LBRACE)) {
        riz_error_col(P->current.line, P->current.column, P->current.column + P->current.length, "Unexpected '{'");
        P->had_error = true;
        return ast_none_lit(line);
    }
    riz_error_col(P->current.line, P->current.column, P->current.column + (P->current.length > 0 ? P->current.length : 1),
                   "Expected expression, got '%.*s'", P->current.length, P->current.start);
    P->had_error = true;
    advance_parser(P);
    return ast_none_lit(line);
}

/* ═══ Call / Index / Member ═══ */

static ASTNode* parse_call(Parser* P) {
    ASTNode* expr = parse_primary(P);
    int line = P->previous.line;
    for (;;) {
        if (match(P, TOK_LPAREN)) {
            int cap=8,cnt=0; ASTNode**args=RIZ_ALLOC_ARRAY(ASTNode*,cap);
            if(!check(P,TOK_RPAREN)){
                do{if(cnt>=RIZ_MAX_ARGS){riz_error_col(P->current.line,P->current.column,P->current.column+P->current.length,"Too many arguments");break;}
                   if(cnt>=cap){cap*=2;args=RIZ_GROW_ARRAY(ASTNode*,args,cnt,cap);}args[cnt++]=parse_expression(P);
                }while(match(P,TOK_COMMA));
            }
            consume(P,TOK_RPAREN,"Expected ')'"); expr=ast_call(expr,args,cnt,line);
        } else if (match(P, TOK_LBRACKET)) {
            /* Check for slice syntax: obj[start:end] or obj[start:end:step] */
            ASTNode* start_e = NULL;
            if (!check(P, TOK_COLON)) start_e = parse_expression(P);
            if (check(P, TOK_COLON)) {
                advance_parser(P); /* consume first ':' */
                ASTNode* end_e = NULL;
                if (!check(P, TOK_COLON) && !check(P, TOK_RBRACKET)) end_e = parse_expression(P);
                ASTNode* step_e = NULL;
                if (check(P, TOK_COLON)) {
                    advance_parser(P); /* consume second ':' */
                    if (!check(P, TOK_RBRACKET)) step_e = parse_expression(P);
                }
                consume(P, TOK_RBRACKET, "Expected ']' after slice");
                expr = ast_slice(expr, start_e, end_e, step_e, line);
            } else {
                consume(P, TOK_RBRACKET, "Expected ']'");
                expr = ast_index(expr, start_e, line);
            }
        } else if (match(P, TOK_DOT)) {
            char* m;
            if (check(P, TOK_IMPORT)) {
                advance_parser(P);
                m = riz_strdup("import");
            } else {
                Token n = consume(P, TOK_IDENTIFIER, "Expected member name");
                m = token_text(n);
            }
            expr = ast_member(expr, m, line);
            free(m);
        } else break;
    }
    return expr;
}

/* ═══ Precedence levels ═══ */

static ASTNode* parse_unary(Parser* P) {
    int line = P->current.line;
    if (match(P,TOK_MINUS)||match(P,TOK_NOT)||match(P,TOK_BANG)) {
        TokenType op=P->previous.type; return ast_unary(op,parse_unary(P),line);
    }
    return parse_call(P);
}
static ASTNode* parse_power(Parser* P) {
    ASTNode*left=parse_unary(P); int line=P->previous.line;
    if(match(P,TOK_POWER)) return ast_binary(TOK_POWER,left,parse_power(P),line);
    return left;
}
static ASTNode* parse_factor(Parser* P) {
    ASTNode*left=parse_power(P); int line=P->previous.line;
    while(match(P,TOK_STAR)||match(P,TOK_SLASH)||match(P,TOK_PERCENT)||match(P,TOK_FLOOR_DIV)){
        TokenType op=P->previous.type; left=ast_binary(op,left,parse_power(P),line);}
    return left;
}
static ASTNode* parse_term(Parser* P) {
    ASTNode*left=parse_factor(P); int line=P->previous.line;
    while(match(P,TOK_PLUS)||match(P,TOK_MINUS)){
        TokenType op=P->previous.type; left=ast_binary(op,left,parse_factor(P),line);}
    return left;
}
static ASTNode* parse_comparison(Parser* P) {
    ASTNode*left=parse_term(P); int line=P->previous.line;
    while(match(P,TOK_LT)||match(P,TOK_GT)||match(P,TOK_LTE)||match(P,TOK_GTE)||match(P,TOK_IN)){
        TokenType op=P->previous.type; left=ast_binary(op,left,parse_term(P),line);}
    return left;
}
static ASTNode* parse_equality(Parser* P) {
    ASTNode*left=parse_comparison(P); int line=P->previous.line;
    while(match(P,TOK_EQ)||match(P,TOK_NEQ)){
        TokenType op=P->previous.type; left=ast_binary(op,left,parse_comparison(P),line);}
    return left;
}
static ASTNode* parse_and(Parser* P) {
    ASTNode*left=parse_equality(P); int line=P->previous.line;
    while(match(P,TOK_AND)) left=ast_binary(TOK_AND,left,parse_equality(P),line);
    return left;
}
static ASTNode* parse_or(Parser* P) {
    ASTNode*left=parse_and(P); int line=P->previous.line;
    while(match(P,TOK_OR)) left=ast_binary(TOK_OR,left,parse_and(P),line);
    return left;
}
static ASTNode* parse_pipe(Parser* P) {
    ASTNode*left=parse_or(P); int line=P->previous.line;
    while(match(P,TOK_PIPE)) left=ast_pipe(left,parse_or(P),line);
    
    /* Ternary: value if condition else other 
       We only allow this if:
       1. Not inside an 'if/while' condition (to avoid 'if x if y else z {' ambiguity)
       2. The 'if' is on the SAME line as the preceding expression.
    */
    if (!P->is_if_condition && check(P, TOK_IF) && P->current.line == line) {
        advance_parser(P); /* consume 'if' */
        ASTNode* cond = parse_expression(P);
        consume(P, TOK_ELSE, "Expected 'else' in ternary expression");
        ASTNode* false_expr = parse_pipe(P); /* right-recursive */
        return ast_ternary(left, cond, false_expr, line);
    }
    return left;
}
static ASTNode* parse_assignment(Parser* P) {
    ASTNode* expr = parse_pipe(P); int line = P->previous.line;
    if (match(P, TOK_ASSIGN)) {
        if (expr->type == NODE_IDENTIFIER) {
            char*name=riz_strdup(expr->as.identifier.name); ast_free(expr);
            return ast_assign(name, parse_assignment(P), line);
        }
        /* Member assignment: obj.field = value */
        if (expr->type == NODE_MEMBER) {
            ASTNode* obj = expr->as.member.object;
            char* member = expr->as.member.member;
            /* Steal children, free only the shell */
            expr->as.member.object = NULL; expr->as.member.member = NULL;
            free(expr);
            return ast_member_assign(obj, member, parse_assignment(P), line);
        }
        /* Index assignment: obj[idx] = value */
        if (expr->type == NODE_INDEX) {
            ASTNode* obj = expr->as.index_expr.object;
            ASTNode* idx = expr->as.index_expr.index;
            expr->as.index_expr.object = NULL; expr->as.index_expr.index = NULL;
            free(expr);
            return ast_index_assign(obj, idx, parse_assignment(P), line);
        }
        riz_error_col(P->previous.line, P->previous.column, P->previous.column + P->previous.length,
                      "Invalid assignment target");
        P->had_error = true;
    }
    if (match(P,TOK_PLUS_ASSIGN)||match(P,TOK_MINUS_ASSIGN)||
        match(P,TOK_STAR_ASSIGN)||match(P,TOK_SLASH_ASSIGN)) {
        TokenType cop=P->previous.type; TokenType bop;
        switch(cop){case TOK_PLUS_ASSIGN:bop=TOK_PLUS;break;case TOK_MINUS_ASSIGN:bop=TOK_MINUS;break;
                    case TOK_STAR_ASSIGN:bop=TOK_STAR;break;default:bop=TOK_SLASH;break;}
        if (expr->type == NODE_IDENTIFIER) {
            char*name=riz_strdup(expr->as.identifier.name); ast_free(expr);
            return ast_compound_assign(name,bop,parse_assignment(P),line);
        }
        riz_error_col(P->previous.line, P->previous.column, P->previous.column + P->previous.length,
                      "Invalid compound assignment target");
        P->had_error = true;
    }
    return expr;
}
static ASTNode* parse_expression(Parser* P) { return parse_assignment(P); }

/* ═══ Statements ═══ */

static ASTNode* parse_block(Parser* P) {
    int line=P->current.line;
    consume(P,TOK_LBRACE,"Expected '{'");
    int cap=8,cnt=0; ASTNode**stmts=RIZ_ALLOC_ARRAY(ASTNode*,cap);
    while(!check(P,TOK_RBRACE)&&!check(P,TOK_EOF)){
        if(cnt>=cap){cap*=2;stmts=RIZ_GROW_ARRAY(ASTNode*,stmts,cnt,cap);}
        stmts[cnt++]=parse_declaration(P);
    }
    consume(P,TOK_RBRACE,"Expected '}'");
    return ast_block(stmts,cnt,line);
}

static ASTNode* parse_if_stmt(Parser* P) {
    int line=P->current.line; consume(P,TOK_IF,"Expected 'if'");
    bool old = P->is_if_condition; P->is_if_condition = true;
    ASTNode*cond=parse_expression(P);
    P->is_if_condition = old;
    ASTNode*then_b=parse_block(P);
    ASTNode*else_b=NULL;
    if(match(P,TOK_ELSE)){if(check(P,TOK_IF))else_b=parse_if_stmt(P);else else_b=parse_block(P);}
    return ast_if_stmt(cond,then_b,else_b,line);
}

static ASTNode* parse_while_stmt(Parser* P) {
    int line=P->current.line; consume(P,TOK_WHILE,"Expected 'while'");
    bool old = P->is_if_condition; P->is_if_condition = true;
    ASTNode*cond=parse_expression(P);
    P->is_if_condition = old;
    ASTNode*body=parse_block(P);
    return ast_while_stmt(cond,body,line);
}

static ASTNode* parse_for_stmt(Parser* P) {
    int line=P->current.line; consume(P,TOK_FOR,"Expected 'for'");
    Token vt=consume(P,TOK_IDENTIFIER,"Expected var name"); char*vn=token_text(vt);
    consume(P,TOK_IN,"Expected 'in'");
    ASTNode*iter=parse_expression(P);
    ASTNode*body=parse_block(P);
    ASTNode*else_b=NULL;
    if(match(P,TOK_ELSE)) else_b=parse_block(P);
    ASTNode*n=ast_for_stmt(vn,iter,body,else_b,line); free(vn); return n;
}

static ASTNode* parse_return_stmt(Parser* P) {
    int line=P->current.line; consume(P,TOK_RETURN,"Expected 'return'");
    ASTNode*val=NULL;
    if(!check(P,TOK_RBRACE)&&!check(P,TOK_EOF)) val=parse_expression(P);
    return ast_return_stmt(val,line);
}

/* ═══ try/catch/throw ═══ */

static ASTNode* parse_try_stmt(Parser* P) {
    int line = P->current.line;
    consume(P, TOK_TRY, "Expected 'try'");
    ASTNode* try_block = parse_block(P);
    consume(P, TOK_CATCH, "Expected 'catch' after try block");
    Token var_tok = consume(P, TOK_IDENTIFIER, "Expected error variable name");
    char* catch_var = token_text(var_tok);
    ASTNode* catch_block = parse_block(P);
    ASTNode* n = ast_try_stmt(try_block, catch_var, catch_block, line);
    free(catch_var);
    return n;
}

static ASTNode* parse_throw_stmt(Parser* P) {
    int line = P->current.line;
    consume(P, TOK_THROW, "Expected 'throw'");
    ASTNode* value = parse_expression(P);
    return ast_throw_stmt(value, line);
}

static ASTNode* parse_statement(Parser* P) {
    if (check(P,TOK_IF))       return parse_if_stmt(P);
    if (check(P,TOK_WHILE))    return parse_while_stmt(P);
    if (check(P,TOK_FOR))      return parse_for_stmt(P);
    if (check(P,TOK_RETURN))   return parse_return_stmt(P);
    if (check(P,TOK_LBRACE))   return parse_block(P);
    if (check(P,TOK_TRY))      return parse_try_stmt(P);
    if (check(P,TOK_THROW))    return parse_throw_stmt(P);
    if (match(P,TOK_BREAK))    return ast_break_stmt(P->previous.line);
    if (match(P,TOK_CONTINUE)) return ast_continue_stmt(P->previous.line);
    int line=P->current.line;
    ASTNode*expr=parse_expression(P);
    return ast_expr_stmt(expr,line);
}

/* ═══ Declarations ═══ */

static ASTNode* parse_let_decl(Parser* P, bool is_mut) {
    int line=P->current.line; advance_parser(P);
    Token nt=consume(P,TOK_IDENTIFIER,"Expected variable name"); char*name=token_text(nt);
    char*type_ann=NULL;
    if(match(P,TOK_COLON)){Token tt=consume(P,TOK_IDENTIFIER,"Expected type");type_ann=token_text(tt);}
    consume(P,TOK_ASSIGN,"Expected '='");
    ASTNode*init=parse_expression(P);
    ASTNode*n=ast_let_decl(name,type_ann,init,is_mut,line); free(name);free(type_ann); return n;
}

static ASTNode* parse_fn_decl(Parser* P) {
    int line=P->current.line; consume(P,TOK_FN,"Expected 'fn'");
    Token nt=consume(P,TOK_IDENTIFIER,"Expected function name"); char*name=token_text(nt);
    consume(P,TOK_LPAREN,"Expected '('");
    int cap=8,pc=0; char**params=RIZ_ALLOC_ARRAY(char*,cap);
    ASTNode** defaults = RIZ_ALLOC_ARRAY(ASTNode*, cap);
    bool seen_default = false;
    if(!check(P,TOK_RPAREN)){
        do{
            if(pc>=cap){cap*=2;params=RIZ_GROW_ARRAY(char*,params,pc,cap);defaults=RIZ_GROW_ARRAY(ASTNode*,defaults,pc,cap);}
            Token pt=consume(P,TOK_IDENTIFIER,"Expected param name");
            params[pc]=token_text(pt);
            if(match(P,TOK_COLON)) consume(P,TOK_IDENTIFIER,"Expected type");
            /* Default parameter value */
            if (match(P, TOK_ASSIGN)) {
                defaults[pc] = parse_expression(P);
                seen_default = true;
            } else {
                defaults[pc] = NULL;
                if (seen_default) {
                    riz_error_col(pt.line, pt.column, pt.column + pt.length, "Non-default parameter after default parameter");
                    P->had_error = true;
                }
            }
            pc++;
        }while(match(P,TOK_COMMA));
    }
    consume(P,TOK_RPAREN,"Expected ')'");
    char*ret=NULL;
    if(match(P,TOK_ARROW)){Token rt=consume(P,TOK_IDENTIFIER,"Expected return type");ret=token_text(rt);}
    ASTNode*body;
    if(match(P,TOK_FAT_ARROW)){
        int el=P->current.line; ASTNode*expr=parse_expression(P);
        ASTNode*ret_s=ast_return_stmt(expr,el); ASTNode**stmts=RIZ_ALLOC_ARRAY(ASTNode*,1);stmts[0]=ret_s;
        body=ast_block(stmts,1,el);
    } else if (check(P,TOK_LBRACE)) {
        body=parse_block(P);
    } else {
        /* signature only */
        body = NULL;
    }
    ASTNode*n=ast_fn_decl(name,params,pc,seen_default?defaults:NULL,ret,body,line);
    if(!seen_default) free(defaults);
    free(name);free(ret); return n;
}

/* ═══ struct / impl ═══ */

static ASTNode* parse_struct_decl(Parser* P) {
    int line = P->current.line;
    consume(P, TOK_STRUCT, "Expected 'struct'");
    Token name_tok = consume(P, TOK_IDENTIFIER, "Expected struct name");
    char* name = token_text(name_tok);
    consume(P, TOK_LBRACE, "Expected '{'");
    int cap = 8, count = 0;
    char** fields = RIZ_ALLOC_ARRAY(char*, cap);
    while (!check(P, TOK_RBRACE) && !check(P, TOK_EOF)) {
        if (count >= cap) { cap *= 2; fields = RIZ_GROW_ARRAY(char*, fields, count, cap); }
        Token ft = consume(P, TOK_IDENTIFIER, "Expected field name");
        fields[count++] = token_text(ft);
        if (match(P, TOK_COLON)) consume(P, TOK_IDENTIFIER, "Expected type"); /* optional type */
        match(P, TOK_COMMA);
    }
    consume(P, TOK_RBRACE, "Expected '}'");
    ASTNode* n = ast_struct_decl(name, fields, count, line);
    free(name); return n;
}

static ASTNode* parse_impl_decl(Parser* P) {
    int line = P->current.line;
    consume(P, TOK_IMPL, "Expected 'impl'");
    Token name_tok = consume(P, TOK_IDENTIFIER, "Expected struct/trait name");
    char* name1 = token_text(name_tok);
    
    if (match(P, TOK_FOR)) {
        /* impl Trait for Struct */
        Token name2_tok = consume(P, TOK_IDENTIFIER, "Expected struct name after 'for'");
        char* name2 = token_text(name2_tok);
        consume(P, TOK_LBRACE, "Expected '{'");
        int cap = 8, count = 0;
        ASTNode** methods = RIZ_ALLOC_ARRAY(ASTNode*, cap);
        while (!check(P, TOK_RBRACE) && !check(P, TOK_EOF)) {
            if (count >= cap) { cap *= 2; methods = RIZ_GROW_ARRAY(ASTNode*, methods, count, cap); }
            methods[count++] = parse_fn_decl(P);
        }
        consume(P, TOK_RBRACE, "Expected '}'");
        ASTNode* n = ast_impl_trait_decl(name1, name2, methods, count, line);
        free(name1); free(name2); return n;
    } else {
        /* impl Struct */
        consume(P, TOK_LBRACE, "Expected '{'");
        int cap = 8, count = 0;
        ASTNode** methods = RIZ_ALLOC_ARRAY(ASTNode*, cap);
        while (!check(P, TOK_RBRACE) && !check(P, TOK_EOF)) {
            if (count >= cap) { cap *= 2; methods = RIZ_GROW_ARRAY(ASTNode*, methods, count, cap); }
            methods[count++] = parse_fn_decl(P);
        }
        consume(P, TOK_RBRACE, "Expected '}'");
        ASTNode* n = ast_impl_decl(name1, methods, count, line);
        free(name1); return n;
    }
}

static ASTNode* parse_trait_decl(Parser* P) {
    int line = P->current.line;
    consume(P, TOK_TRAIT, "Expected 'trait'");
    Token name_tok = consume(P, TOK_IDENTIFIER, "Expected trait name");
    char* name = token_text(name_tok);
    consume(P, TOK_LBRACE, "Expected '{'");
    int cap = 8, count = 0;
    ASTNode** methods = RIZ_ALLOC_ARRAY(ASTNode*, cap);
    while (!check(P, TOK_RBRACE) && !check(P, TOK_EOF)) {
        if (count >= cap) { cap *= 2; methods = RIZ_GROW_ARRAY(ASTNode*, methods, count, cap); }
        methods[count++] = parse_fn_decl(P);
    }
    consume(P, TOK_RBRACE, "Expected '}'");
    ASTNode* n = ast_trait_decl(name, methods, count, line);
    free(name); return n;
}

static ASTNode* parse_import(Parser* P) {
    int line=P->current.line; advance_parser(P);
    Token path_tok=consume(P,TOK_STRING,"Expected file path string after 'import'");
    char*path=parse_string_literal(path_tok);
    ASTNode*n=ast_import(path,line); free(path); return n;
}

/* import_python [path] — same as import_native; default path is platform plugin name */
static ASTNode* parse_import_python(Parser* P) {
    int line = P->current.line;
    advance_parser(P);
    char* path;
    if (check(P, TOK_STRING)) {
        Token path_tok = consume(P, TOK_STRING, "Expected string path after 'import_python'");
        path = parse_string_literal(path_tok);
    } else {
#ifdef _WIN32
        path = riz_strdup("plugin_python.dll");
#elif defined(__APPLE__)
        path = riz_strdup("plugin_python.dylib");
#else
        path = riz_strdup("plugin_python.so");
#endif
    }
    ASTNode* n = ast_import_native(path, line);
    free(path);
    return n;
}

static ASTNode* parse_import_native(Parser* P) {
    int line=P->current.line; advance_parser(P); /* consume import_native */
    Token path_tok=consume(P,TOK_STRING,"Expected library path after 'import_native'");
    char*path=parse_string_literal(path_tok);
    ASTNode*n=ast_import_native(path,line); free(path); return n;
}

static ASTNode* parse_declaration(Parser* P) {
    if (check(P,TOK_LET))           return parse_let_decl(P, false);
    if (check(P,TOK_MUT))           return parse_let_decl(P, true);
    if (check(P,TOK_FN))            return parse_fn_decl(P);
    if (check(P,TOK_IMPORT_PYTHON)) return parse_import_python(P);
    if (check(P,TOK_IMPORT_NATIVE)) return parse_import_native(P);
    if (check(P,TOK_IMPORT))        return parse_import(P);
    if (check(P,TOK_STRUCT))        return parse_struct_decl(P);
    if (check(P,TOK_IMPL))          return parse_impl_decl(P);
    if (check(P,TOK_TRAIT))         return parse_trait_decl(P);
    return parse_statement(P);
}

/* ═══ Public API ═══ */

void parser_init(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->had_error = false;
    parser->panic_mode = false;
    parser->is_if_condition = false;
    advance_parser(parser);
}

ASTNode* parser_parse(Parser* P) {
    int cap=16,cnt=0; ASTNode**decls=RIZ_ALLOC_ARRAY(ASTNode*,cap);
    while(!check(P,TOK_EOF)){
        if(cnt>=cap){cap*=2;decls=RIZ_GROW_ARRAY(ASTNode*,decls,cnt,cap);}
        decls[cnt++]=parse_declaration(P);
    }
    return ast_program(decls,cnt);
}

ASTNode* parser_parse_expression(Parser* P) { return parse_expression(P); }
