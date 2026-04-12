/*
 * Riz — resolve import "..." paths against project root / vendor/
 */

#ifndef RIZ_IMPORT_H
#define RIZ_IMPORT_H

#include <stddef.h>
#include <stdbool.h>

/* Call once before running a script (interpreter or VM) with the entry .riz path. */
void riz_import_configure(const char* entry_script_path);

/* Try to resolve import string to an existing file; returns true if out is usable. */
bool riz_import_resolve(char* out, size_t cap, const char* import_path);

#endif
