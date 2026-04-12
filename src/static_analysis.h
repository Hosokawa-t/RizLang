/*
 * Riz — optional static checks after parse (`riz check`, LSP)
 */

#ifndef RIZ_STATIC_ANALYSIS_H
#define RIZ_STATIC_ANALYSIS_H

#include "ast.h"
#include <stdbool.h>

/* Walk the program AST; emit riz_error / riz_warn. Returns false if any error was reported. */
bool riz_static_analysis_ok(ASTNode* program);

#endif
