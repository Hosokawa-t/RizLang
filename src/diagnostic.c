/*
 * Riz — riz_error() implementation (terminal colors vs NDJSON for LSP bridge)
 */

#include "diagnostic.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

bool riz_machine_diag_mode = false;
const char* riz_diag_source_path = NULL;

static void json_escape_and_print(FILE* out, const char* s) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc((int)c, out);
    }
    fputc('"', out);
}

static void emit_diag_line(int line, int col0, int col1_exc, const char* buf) {
    if (riz_machine_diag_mode) {
        FILE* out = stdout;
        fprintf(out, "{\"line\":%d", line);
        if (col0 >= 0) {
            int end = col1_exc;
            if (end <= col0)
                end = col0 + 1;
            fprintf(out, ",\"startColumn\":%d,\"endColumn\":%d", col0, end);
        }
        fprintf(out, ",\"message\":");
        json_escape_and_print(out, buf);
        fprintf(out, ",\"source\":\"riz-parse\"");
        if (riz_diag_source_path && riz_diag_source_path[0]) {
            fprintf(out, ",\"file\":");
            json_escape_and_print(out, riz_diag_source_path);
        }
        fprintf(out, "}\n");
        fflush(out);
        return;
    }

    fprintf(stderr, COL_RED COL_BOLD "Error" COL_RESET
            COL_RED " [line %d]: " COL_RESET COL_RED "%s" COL_RESET "\n",
            line, buf);
}

void riz_error(int line, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    emit_diag_line(line, -1, -1, buf);
}

void riz_error_col(int line, int start_column, int end_column_exclusive, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    emit_diag_line(line, start_column, end_column_exclusive, buf);
}
