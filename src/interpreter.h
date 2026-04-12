/*
 * Riz Programming Language
 * interpreter.h — Tree-walking interpreter interface
 */

#ifndef RIZ_INTERPRETER_H
#define RIZ_INTERPRETER_H

#include "ast.h"
#include "environment.h"
#include "vm.h"

/* ─── Signal Types for control flow ───────────────────── */
typedef enum {
    SIG_NONE,
    SIG_RETURN,
    SIG_BREAK,
    SIG_CONTINUE,
    SIG_THROW,
} SignalType;

/* ─── Interpreter State ───────────────────────────────── */
typedef struct {
    Environment* globals;       /* global scope */
    Environment* current_env;   /* current scope */
    SignalType   signal;        /* control flow signal */
    RizValue     signal_value;  /* return value when SIG_RETURN */
    bool         had_error;
    int          current_line;  /* currently executing line for diagnostic panics */
    /* Import tracking (Phase 2) */
    char**       imported_files;
    int          import_count;
    /* FFI: loaded native libraries (Phase 5) */
    void**       loaded_libs;   /* HMODULE handles (Windows) / dlopen handles (POSIX) */
    int          lib_count;
    /* File runs: root AST; freed after globals in interpreter_free. REPL: always NULL. */
    ASTNode*     program_ast;
} Interpreter;

/* ─── API ─────────────────────────────────────────────── */

/* Register the same built-ins as the interpreter uses (for the bytecode VM globals). */
void riz_vm_seed_builtins(Environment* env);

/* Load a native .dll/.so into VM globals (same riz_plugin_init as interpreter). */
bool riz_plugin_load_vm(Environment* env, RizVM* vm, const char* path);

/* Create and initialize an interpreter with built-in functions */
Interpreter* interpreter_new(void);

/* Free the interpreter */
void interpreter_free(Interpreter* interp);

/* Execute a program (NODE_PROGRAM) */
void interpreter_exec(Interpreter* interp, ASTNode* program);

/* Evaluate a single expression and return the result */
RizValue interpreter_eval(Interpreter* interp, ASTNode* node);

#endif /* RIZ_INTERPRETER_H */
