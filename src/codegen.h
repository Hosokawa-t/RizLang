/*
 * Riz Programming Language
 * codegen.h — AOT C code generator interface
 */

#ifndef RIZ_CODEGEN_H
#define RIZ_CODEGEN_H

#include "ast.h"

/* Generate C source code from AST into a file.
 * runtime_path: path to riz_runtime.h (for the #include in generated code)
 * Returns true on success. */
bool codegen_emit(ASTNode* program, const char* output_path, const char* runtime_path);

#endif /* RIZ_CODEGEN_H */
