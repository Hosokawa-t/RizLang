/*
 * Riz — common definitions and platform detection
 */

#ifndef RIZ_COMMON_H
#define RIZ_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <math.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#define RIZ_VERSION "0.9.7"
#define RIZ_MAX_ARGS 255
#define RIZ_LINE_BUF_SIZE 1024
#define RIZ_INITIAL_CAP 8
#define RIZ_MAX_LOCALS 512
#define RIZ_MAX_UPVALUES 512
#define RIZ_STACK_MAX 1024

static inline char* riz_strndup(const char* s, size_t n) {
    char* res = (char*)malloc(n + 1);
    if (res) { memcpy(res, s, n); res[n] = '\0'; }
    return res;
}

#ifdef _WIN32
#define RIZ_EXPORT __declspec(dllexport)
  #define WIN32_LEAN_AND_MEAN
  #define NOCOMM
  #define TokenType __win_TokenType
  #include <windows.h>
  #undef TokenType
  static inline void riz_enable_ansi(void) {
      SetConsoleOutputCP(65001);
      SetConsoleCP(65001);
      HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD mode = 0;
      GetConsoleMode(h, &mode);
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      h = GetStdHandle(STD_ERROR_HANDLE);
      GetConsoleMode(h, &mode);
      SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
#else
#define RIZ_EXPORT
  static inline void riz_enable_ansi(void) {}
#endif

/* ─── ANSI Colors ────────────────────────────────────── */
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
void riz_error(int line, const char* fmt, ...);

void riz_actual_runtime_error(const char* fmt, ...);
#define riz_runtime_error(fmt, ...) riz_actual_runtime_error(fmt, ##__VA_ARGS__)

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

#endif
