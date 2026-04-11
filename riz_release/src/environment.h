/*
 * Riz Programming Language
 * environment.h — Variable scope / environment chain
 */

#ifndef RIZ_ENVIRONMENT_H
#define RIZ_ENVIRONMENT_H

#include "value.h"

/* ─── Environment Entry ───────────────────────────────── */
typedef struct {
    char*    name;
    RizValue value;
    bool     is_mutable;
} EnvEntry;

/* ─── Environment ─────────────────────────────────────── */
struct Environment {
    EnvEntry*    entries;
    int          count;
    int          capacity;
    Environment* parent;
};

/* ─── API ─────────────────────────────────────────────── */

/* Create a new environment, optionally with a parent scope */
Environment* env_new(Environment* parent);

/* Free an environment (does NOT free parent) */
void env_free(Environment* env);

/* Define a new variable in the current scope */
bool env_define(Environment* env, const char* name, RizValue value, bool is_mutable);

/* Look up a variable by name (walks parent chain) */
bool env_get(Environment* env, const char* name, RizValue* out);

/* Update an existing variable (must be mutable; walks parent chain) */
bool env_set(Environment* env, const char* name, RizValue value);

/* Check if a variable exists in the current scope only */
bool env_has_local(Environment* env, const char* name);

#endif /* RIZ_ENVIRONMENT_H */
