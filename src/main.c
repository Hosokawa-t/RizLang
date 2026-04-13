/*
 * Riz Programming Language
 * main.c — Entry point: REPL and file execution
 *
 *   riz              → Start interactive REPL
 *   riz file.riz     → Execute a .riz file
 *   riz --version    → Show version
 *   riz --help       → Show help
 */

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "compiler.h"
#include "vm.h"
#include "codegen.h"
#include "diagnostic.h"
#include "pkg.h"
#include "riz_import.h"
#include "riz_env.h"
#include "static_analysis.h"
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════
 *  File reading
 * ═══════════════════════════════════════════════════════ */

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, COL_RED "Error: Could not open file '%s'\n" COL_RESET, path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)malloc(length + 1);
    if (!buffer) {
        fprintf(stderr, COL_RED "Error: Out of memory\n" COL_RESET);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, length, file);
    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

/* Parse + static analysis; NDJSON diagnostics on stdout (one object per line).
 * strict_warnings: treat riz_warn diagnostics as failure (for CI / production gates). */
static int run_check(const char* path, bool strict_warnings) {
    char* source = read_file(path);
    if (!source) return 1;
    riz_import_configure(path);
    riz_diag_warning_count = 0;
    riz_machine_diag_mode = true;
    riz_diag_source_path = path;
    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer);
    ASTNode* program = parser_parse(&parser);
    int rc = 0;
    if (parser.had_error) rc = 1;
    else if (!riz_static_analysis_ok(program)) rc = 1;
    else if (strict_warnings && riz_diag_warning_count > 0) rc = 1;
    ast_free(program);
    free(source);
    riz_machine_diag_mode = false;
    riz_diag_source_path = NULL;
    return rc;
}

/* argv[0]=riz argv[1]=check argv[2..]= [--strict] <file.riz> */
static int run_check_cli(int argc, char** argv) {
    bool strict = false;
    const char* es = getenv("RIZ_CHECK_STRICT");
    if (es && es[0] != '\0' && strcmp(es, "0") != 0) strict = true;
    const char* path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) strict = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, COL_RED "Unknown option '%s'" COL_RESET " (use: riz check [--strict] <file.riz>)\n", argv[i]);
            return 1;
        } else if (path) {
            fprintf(stderr, COL_RED "Unexpected extra argument '%s'" COL_RESET "\n", argv[i]);
            return 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "Usage: riz check [--strict] <file.riz>\n");
        return 1;
    }
    return run_check(path, strict);
}

/* ═══════════════════════════════════════════════════════
 *  Run source code
 * ═══════════════════════════════════════════════════════ */

static void run_source(Interpreter* interp, const char* source, bool is_repl) {
    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer);

    if (is_repl) {
        /* In REPL mode, try to evaluate as expression first.
         * If the top-level is a single expression, print its value. */
        ASTNode* program = parser_parse(&parser);

        if (parser.had_error) {
            ast_free(program);
            return;
        }

        /* If the program is a single expression statement, print the result */
        if (program->type == NODE_PROGRAM &&
            program->as.program.count == 1 &&
            program->as.program.declarations[0]->type == NODE_EXPR_STMT) {

            ASTNode* expr = program->as.program.declarations[0]->as.expr_stmt.expr;
            RizValue result = interpreter_eval(interp, expr);

            /* Don't print none unless it's an explicit none literal */
            if (result.type != VAL_NONE || expr->type == NODE_NONE_LIT) {
                printf(COL_DIM "=> " COL_RESET);
                if (result.type == VAL_STRING) {
                    printf(COL_GREEN "\"%s\"" COL_RESET, result.as.string);
                } else if (result.type == VAL_INT || result.type == VAL_FLOAT) {
                    printf(COL_CYAN);
                    riz_value_print(result);
                    printf(COL_RESET);
                } else if (result.type == VAL_BOOL) {
                    printf(COL_YELLOW);
                    riz_value_print(result);
                    printf(COL_RESET);
                } else {
                    riz_value_print(result);
                }
                printf("\n");
            }
        } else {
            interpreter_exec(interp, program);
        }

        /* Don't free the AST — function bodies reference it */
    } else {
        ASTNode* program = parser_parse(&parser);
        if (parser.had_error) {
            ast_free(program);
            return;
        }
        interp->program_ast = program;
        interpreter_exec(interp, program);
    }
    interpreter_report_pending_signal(interp);
}

/* Execute a .riz file */
static int run_file(const char* path, int script_argc, char** script_argv) {
    riz_import_configure(path);
    char* source = read_file(path);
    if (!source) return 1;

    riz_runtime_set_cli_context(path, script_argc, script_argv);
    Interpreter* interp = interpreter_new();
    run_source(interp, source, false);

    int exit_code = interp->had_error ? 1 : 0;
    interpreter_free(interp);
    free(source);
    return exit_code;
}

/* Execute a .riz file via the Bytecode VM */
static int run_file_vm(const char* path, int script_argc, char** script_argv) {
    riz_import_configure(path);
    char* source = read_file(path);
    if (!source) return 1;

    riz_runtime_set_cli_context(path, script_argc, script_argv);
    printf(COL_DIM "[VM mode] Compiling %s..." COL_RESET "\n", path);

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer);
    ASTNode* program = parser_parse(&parser);
    if (parser.had_error) { ast_free(program); free(source); return 1; }

    Chunk chunk;
    chunk_init(&chunk);
    if (!compiler_compile(program, &chunk)) {
        fprintf(stderr, COL_RED "Compilation to bytecode failed." COL_RESET "\n");
        chunk_free(&chunk);
        ast_free(program);
        free(source);
        return 1;
    }
    printf(COL_DIM "[VM mode] Compiled %d bytes of bytecode, %d constants." COL_RESET "\n",
           chunk.count, chunk.const_count);

    RizVM vm;
    vm_init(&vm);
    VMResult result = vm_execute(&vm, &chunk);
    int exit_code = (result == VM_OK) ? 0 : 1;

    vm_free(&vm);
    chunk_free(&chunk);
    ast_free(program);
    free(source);
    return exit_code;
}

/* ═══════════════════════════════════════════════════════
 *  REPL (Read-Eval-Print Loop)
 * ═══════════════════════════════════════════════════════ */

static void print_banner(void) {
    printf("\n");
    printf(COL_BOLD COL_CYAN
        "  ██████╗  ██╗ ███████╗\n"
        "  ██╔══██╗ ██║ ╚════██║\n"
        "  ██████╔╝ ██║   ███╔═╝\n"
        "  ██╔══██╗ ██║ ██╔══╝  \n"
        "  ██║  ██║ ██║ ███████╗\n"
        "  ╚═╝  ╚═╝ ╚═╝ ╚══════╝\n"
    COL_RESET);
    printf(COL_BOLD "  Riz" COL_RESET COL_DIM " v%s" COL_RESET
           " — " COL_ITALIC "The AI-Native Programming Language" COL_RESET "\n", RIZ_VERSION);
    printf(COL_DIM "  Type 'exit' to quit, 'help' for info." COL_RESET "\n\n");
}

static void print_help(void) {
    printf("\n" COL_BOLD "  Riz Language — Quick Reference" COL_RESET "\n\n");
    printf(COL_YELLOW "  Variables:" COL_RESET "\n");
    printf("    let x = 42          # immutable\n");
    printf("    mut y = 0           # mutable\n\n");
    printf(COL_YELLOW "  Functions:" COL_RESET "\n");
    printf("    fn add(a, b) { return a + b }\n");
    printf("    fn double(x) => x * 2\n\n");
    printf(COL_YELLOW "  Control Flow:" COL_RESET "\n");
    printf("    if x > 0 { ... } else { ... }\n");
    printf("    while x > 0 { ... }\n");
    printf("    for i in range(10) { ... }\n\n");
    printf(COL_YELLOW "  Pipe Operator:" COL_RESET "\n");
    printf("    [1,2,3] |> len()    # => 3\n\n");
    printf(COL_YELLOW "  Built-in Functions:" COL_RESET "\n");
    printf("    print, len, range, type, str, int, float\n");
    printf("    input, append, pop, abs, min, max, sum, parallel_sum, cpu_count\n");
    printf("    map, filter, format, sorted, zip, assert, debug, panic, …\n");
    printf("    clamp, sign, floor, ceil, round, all, any, bool\n");
    printf("    ord, chr, extend, argv, argc, script_path, parse_flags\n");
    printf("    read_file, write_file, read_lines, write_lines, parse_csv, read_csv\n");
    printf("    parse_tsv, read_tsv, list_dir, walk_dir, glob, mkdir\n");
    printf("    file_exists, dir_exists, cwd, getenv, basename, dirname, join_path\n");
    printf("    json_parse, json_stringify, read_json, write_json, has_key\n\n");
}

static void run_repl(void) {
    riz_enable_ansi();
    print_banner();

    riz_runtime_set_cli_context(NULL, 0, NULL);
    Interpreter* interp = interpreter_new();
    char line[RIZ_LINE_BUF_SIZE];

    for (;;) {
        printf(COL_BOLD COL_CYAN "riz" COL_RESET COL_BOLD "> " COL_RESET);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines */
        if (len == 0) continue;

        /* Special commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (strcmp(line, "help") == 0) { print_help(); continue; }
        if (strcmp(line, "clear") == 0) {
            printf("\033[2J\033[H");
            print_banner();
            continue;
        }

        /* Multi-line input: if line ends with '{', keep reading */
        if (line[len - 1] == '{') {
            char* multiline = (char*)malloc(4096);
            size_t ml_len = 0;
            size_t ml_cap = 4096;
            int brace_depth = 0;

            /* Copy first line */
            memcpy(multiline, line, len);
            ml_len = len;
            multiline[ml_len++] = '\n';

            /* Count braces in first line */
            for (size_t i = 0; i < len; i++) {
                if (line[i] == '{') brace_depth++;
                if (line[i] == '}') brace_depth--;
            }

            while (brace_depth > 0) {
                printf(COL_DIM "...  " COL_RESET);
                fflush(stdout);

                if (!fgets(line, sizeof(line), stdin)) break;
                size_t ll = strlen(line);

                /* Ensure capacity */
                while (ml_len + ll + 1 >= ml_cap) {
                    ml_cap *= 2;
                    multiline = (char*)realloc(multiline, ml_cap);
                }

                memcpy(multiline + ml_len, line, ll);
                ml_len += ll;

                for (size_t i = 0; i < ll; i++) {
                    if (line[i] == '{') brace_depth++;
                    if (line[i] == '}') brace_depth--;
                }
            }

            multiline[ml_len] = '\0';
            run_source(interp, multiline, true);
            free(multiline);
        } else {
            run_source(interp, line, true);
        }
    }

    interpreter_free(interp);
    printf(COL_DIM "Goodbye! 🚀\n" COL_RESET);
}

/* ═══════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════ */

int main(int argc, char* argv[]) {
    riz_enable_ansi();

    if (argc >= 2 && strcmp(argv[1], "pkg") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: riz pkg <init|add|install|update|sync|tree> ...  (try: riz pkg --help)\n");
            return 1;
        }
        return riz_pkg_main(argc - 2, argv + 2);
    }

    if (argc >= 2 && strcmp(argv[1], "install") == 0) {
        return riz_install_main(argc - 2, argv + 2);
    }

    if (argc >= 2 && strcmp(argv[1], "env") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: riz env <doctor|setup|init|shell> ...  (try: riz env --help)\n");
            return 1;
        }
        return riz_env_main(argc - 2, argv + 2, argv[0]);
    }

    if (argc == 1) {
        run_repl();
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("Riz %s\n", RIZ_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: riz [options] [file.riz] [args...]\n\n");
        printf("Options:\n");
        printf("  -v, --version         Show version\n");
        printf("  -h, --help            Show this help\n");
        printf("  --vm <file> [args...] Run file via Bytecode VM (experimental)\n");
        printf("  --aot <file> [--tcc|--gcc]  Compile file to a native binary\n");
        printf("  check [--strict] <file>     Parse + static checks; NDJSON on stdout\n");
        printf("  install [pkg...]      Easy install: name, owner/repo, URL, path, or manifest deps\n");
        printf("  pkg <cmd>             Package manager (install --locked, packages.index, …)\n");
        printf("  env <cmd>             Easy environment: doctor, setup, shell (see riz env --help)\n\n");
        printf("Script arguments are available inside Riz via argv(), argc(), script_path(), and parse_flags().\n");
        printf("If no file is given, starts the interactive REPL.\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "check") == 0) {
        return run_check_cli(argc, argv);
    }

    if (argc >= 3 && strcmp(argv[1], "--vm") == 0) {
        return run_file_vm(argv[2], argc - 3, argv + 3);
    }

    /* --aot input.riz [--tcc | --gcc] : AOT compile to native binary */
    if (argc >= 3 && strcmp(argv[1], "--aot") == 0) {
        bool force_tcc = false;
        bool force_gcc = false;
        if (argc >= 4) {
            if (strcmp(argv[3], "--tcc") == 0) force_tcc = true;
            if (strcmp(argv[3], "--gcc") == 0) force_gcc = true;
        }
        
        char* source = read_file(argv[2]);
        if (!source) return 1;

        printf(COL_DIM "[AOT] Parsing %s..." COL_RESET "\n", argv[2]);
        Lexer lexer; lexer_init(&lexer, source);
        Parser parser; parser_init(&parser, &lexer);
        ASTNode* program = parser_parse(&parser);
        if (parser.had_error) { ast_free(program); free(source); return 1; }

        /* Generate output names */
        char c_path[512], exe_path[512];
        /* Strip .riz extension */
        strncpy(exe_path, argv[2], sizeof(exe_path)-1); exe_path[sizeof(exe_path)-1]='\0';
        char* dot = strrchr(exe_path, '.');
        if (dot) *dot = '\0';
        snprintf(c_path, sizeof(c_path), "%s_aot.c", exe_path);
        #ifdef _WIN32
        strncat(exe_path, ".exe", sizeof(exe_path)-strlen(exe_path)-1);
        #endif

        printf(COL_DIM "[AOT] Generating C code: %s" COL_RESET "\n", c_path);
        if (!codegen_emit(program, c_path, "aot_runtime.h")) {
            fprintf(stderr, COL_RED "AOT code generation failed." COL_RESET "\n");
            ast_free(program); free(source); return 1;
        }

        /* Resolve the compiler */
        bool use_gcc = false;
        if (force_gcc) {
            use_gcc = true;
        } else if (force_tcc) {
            use_gcc = false;
        } else {
            /* Try to detect GCC */
            #ifdef _WIN32
            int gcc_ret = system("gcc --version > NUL 2>&1");
            #else
            int gcc_ret = system("gcc --version > /dev/null 2>&1");
            #endif
            use_gcc = (gcc_ret == 0);
        }
        
        /* Invoke Compiler to build the generated C to a native binary */
        char cmd[1024];
        if (use_gcc) {
            snprintf(cmd, sizeof(cmd),
                "gcc -O2 -std=c11 -I\"src\" -o \"%s\" \"%s\" \"src/aot_runtime.c\" \"src/value.c\" \"src/environment.c\" \"src/chunk.c\" \"src/interpreter.c\" \"src/diagnostic.c\" \"src/ast.c\" \"src/riz_import.c\" \"src/pkg.c\" \"src/riz_env.c\" \"src/lexer.c\" \"src/parser.c\" \"src/compiler.c\" \"src/vm.c\" \"src/codegen.c\" -lm",
                exe_path, c_path);
        } else {
            snprintf(cmd, sizeof(cmd),
                "vendor\\tcc\\tcc.exe -I\"src\" -o \"%s\" \"%s\" \"src/aot_runtime.c\" \"src/value.c\" \"src/environment.c\" \"src/chunk.c\" \"src/interpreter.c\" \"src/diagnostic.c\" \"src/ast.c\" \"src/riz_import.c\" \"src/pkg.c\" \"src/riz_env.c\" \"src/lexer.c\" \"src/parser.c\" \"src/compiler.c\" \"src/vm.c\" \"src/codegen.c\"",
                exe_path, c_path);
        }
        
        printf(COL_DIM "[AOT] Compiling: %s" COL_RESET "\n", cmd);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, COL_RED "%s compilation failed (exit %d)" COL_RESET "\n", use_gcc ? "GCC" : "TCC", ret);
            ast_free(program); free(source); return 1;
        }
        printf(COL_GREEN COL_BOLD "[AOT] ✓ Native binary: %s" COL_RESET "\n", exe_path);
        ast_free(program); free(source);
        return 0;
    }

    if (argv[1][0] == '-') {
        fprintf(stderr, "Usage: riz [check|--vm|--aot|env|pkg|install|file.riz] ...\n");
        return 1;
    }
    return run_file(argv[1], argc - 2, argv + 2);
}
