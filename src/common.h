/*
 * Riz Programming Language
 * common.h — Shared definitions, macros, and constants
 *
 * Copyright (c) 2026 Riz Contributors
 */

#ifndef RIZ_COMMON_H
#define RIZ_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

/* ─── Version ─────────────────────────────────────────── */
#define RIZ_VERSION_MAJOR 0
#define RIZ_VERSION_MINOR 9
#define RIZ_VERSION_PATCH 6
#define RIZ_VERSION "0.9.6"

/* ─── Limits ──────────────────────────────────────────── */
#define RIZ_MAX_ARGS       255
#define RIZ_MAX_PARAMS     255
#define RIZ_MAX_LOCALS     256
#define RIZ_INITIAL_CAP    8
#define RIZ_LINE_BUF_SIZE  1024

/* ─── ANSI Colors ─────────────────────────────────────── */
#ifdef _WIN32
  /* Avoid Windows API name collisions:
   * windows.h defines enum TOKEN_TYPE { ..., TokenType, ... }
   * which clashes with our 'typedef enum { ... } TokenType;' in lexer.h.
   * We redirect the Windows identifier before inclusion. */
  #define WIN32_LEAN_AND_MEAN
  #define NOCOMM
  #define TokenType __win_TokenType
  #include <windows.h>
  #undef TokenType
  static inline void riz_enable_ansi(void) {
      #ifdef _WIN32
      SetConsoleOutputCP(65001);
      SetConsoleCP(65001);
      #endif
      HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD mode = 0;
      GetConsoleMode(h, &mode);
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      h = GetStdHandle(STD_ERROR_HANDLE);
      GetConsoleMode(h, &mode);
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
#else
  static inline void riz_enable_ansi(void) {}
#endif

#define COL_RESET   "\033[0m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_BLUE    "\033[34m"
#define COL_MAGENTA "\033[35m"
#define COL_CYAN    "\033[36m"
#define COL_WHITE   "\033[37m"
#define COL_BOLD    "\033[1m"
#define COL_DIM     "\033[2m"
#define COL_ITALIC  "\033[3m"

/* ─── Error Reporting ─────────────────────────────────── */
/* Parse errors (parser.c); machine mode for `riz check` — see diagnostic.c */
void riz_error(int line, const char* fmt, ...);

#define riz_runtime_error(fmt, ...) \
    fprintf(stderr, COL_RED COL_BOLD "RuntimeError: " COL_RESET \
            COL_RED fmt COL_RESET "\n", ##__VA_ARGS__)

/* ─── Memory Helpers ──────────────────────────────────── */
#define RIZ_ALLOC(type) \
    ((type*)calloc(1, sizeof(type)))

#define RIZ_ALLOC_ARRAY(type, count) \
    ((type*)calloc((count), sizeof(type)))

#define RIZ_GROW_ARRAY(type, ptr, old_cap, new_cap) \
    ((type*)realloc((ptr), sizeof(type) * (new_cap)))

static inline char* riz_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

static inline char* riz_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    char* copy = (char*)malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n);
        copy[n] = '\0';
    }
    return copy;
}

#endif /* RIZ_COMMON_H */
