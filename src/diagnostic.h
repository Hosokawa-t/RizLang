/*
 * Riz — parse error reporting (human terminal vs machine-readable lines)
 */

#ifndef RIZ_DIAGNOSTIC_H
#define RIZ_DIAGNOSTIC_H

#include <stdbool.h>

/* When true, riz_error* prints one NDJSON object per line to stdout (for `riz check`). */
extern bool riz_machine_diag_mode;

/* Current source path for machine diagnostics (UTF-8); may be NULL. */
extern const char* riz_diag_source_path;

/* Column-aware parse errors (see common.h for riz_error). */
void riz_error_col(int line, int start_column, int end_column_exclusive, const char* fmt, ...);

#endif
