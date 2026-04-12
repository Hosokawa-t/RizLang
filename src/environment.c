/*
 * Riz Programming Language
 * environment.c — Variable scope chain implementation
 */

#include "environment.h"
#include "value.h"

/* ═══════════════════════════════════════════════════════
 *  Create / Free
 * ═══════════════════════════════════════════════════════ */

Environment* env_new(Environment* parent) {
    Environment* env = RIZ_ALLOC(Environment);
    env->entries = NULL;
    env->count = 0;
    env->capacity = 0;
    env->parent = parent;
    return env;
}

void env_free(Environment* env) {
    if (!env) return;
    for (int i = 0; i < env->count; i++) {
        free(env->entries[i].name);
        /* Don't free values deeply — they may be referenced elsewhere */
    }
    free(env->entries);
    free(env);
}

void env_free_deep(Environment* env) {
    if (!env) return;
    /* Reverse order: later bindings often reference earlier ones (e.g. instances before struct defs). */
    for (int i = env->count - 1; i >= 0; i--) {
        riz_value_free(&env->entries[i].value);
        free(env->entries[i].name);
    }
    free(env->entries);
    free(env);
}

/* ═══════════════════════════════════════════════════════
 *  Lookup helpers
 * ═══════════════════════════════════════════════════════ */

static int find_entry(Environment* env, const char* name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════ */

bool env_define(Environment* env, const char* name, RizValue value, bool is_mutable) {
    /* Check if already defined in current scope */
    int idx = find_entry(env, name);
    if (idx >= 0) {
        /* Redefine: overwrite the existing entry */
        env->entries[idx].value = value;
        env->entries[idx].is_mutable = is_mutable;
        return true;
    }

    /* Grow if needed */
    if (env->count >= env->capacity) {
        int new_cap = env->capacity < RIZ_INITIAL_CAP ? RIZ_INITIAL_CAP : env->capacity * 2;
        env->entries = RIZ_GROW_ARRAY(EnvEntry, env->entries, env->capacity, new_cap);
        env->capacity = new_cap;
    }

    env->entries[env->count].name = riz_strdup(name);
    env->entries[env->count].value = value;
    env->entries[env->count].is_mutable = is_mutable;
    env->count++;
    return true;
}

bool env_get(Environment* env, const char* name, RizValue* out) {
    Environment* current = env;
    while (current) {
        int idx = find_entry(current, name);
        if (idx >= 0) {
            *out = current->entries[idx].value;
            return true;
        }
        current = current->parent;
    }
    return false;
}

bool env_set(Environment* env, const char* name, RizValue value) {
    Environment* current = env;
    while (current) {
        int idx = find_entry(current, name);
        if (idx >= 0) {
            if (!current->entries[idx].is_mutable) {
                riz_runtime_error("Cannot assign to immutable variable '%s'. Use 'mut' to declare mutable variables.", name);
                return false;
            }
            current->entries[idx].value = value;
            return true;
        }
        current = current->parent;
    }
    riz_runtime_error("Undefined variable '%s'", name);
    return false;
}

bool env_has_local(Environment* env, const char* name) {
    return find_entry(env, name) >= 0;
}
