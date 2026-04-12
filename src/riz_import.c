/*
 * Riz — import path resolution (vendor/, project root)
 */

#include "riz_import.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#include <unistd.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

static char s_project_root[1024];
static bool s_have_root;

static bool file_readable(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void copy_dirname(const char* path, char* dir, size_t cap) {
    if (!path || !dir || cap < 2) {
        if (cap) dir[0] = '.';
        if (cap > 1) dir[1] = '\0';
        return;
    }
    strncpy(dir, path, cap - 1);
    dir[cap - 1] = '\0';
    char* s = strrchr(dir, PATH_SEP);
    if (!s) s = strrchr(dir, '/');
    if (!s) s = strrchr(dir, '\\');
    if (s) *s = '\0';
    else {
        dir[0] = '.';
        dir[1] = '\0';
    }
}

static void parent_dir(char* dir) {
    char* s = strrchr(dir, PATH_SEP);
    if (!s) s = strrchr(dir, '/');
    if (!s) s = strrchr(dir, '\\');
    if (s && s != dir) *s = '\0';
    else {
        dir[0] = '.';
        dir[1] = '\0';
    }
}

static bool find_project_root(char* start_dir, char* out_root, size_t cap) {
    char cur[1024];
    strncpy(cur, start_dir, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    for (int depth = 0; depth < 64; depth++) {
        char candidate[1100];
        snprintf(candidate, sizeof(candidate), "%s" PATH_SEP_STR "riz.json", cur);
        if (file_readable(candidate)) {
            strncpy(out_root, cur, cap - 1);
            out_root[cap - 1] = '\0';
            return true;
        }
        if (strcmp(cur, ".") == 0) break;
        parent_dir(cur);
    }
    return false;
}

void riz_import_configure(const char* entry_script_path) {
    s_have_root = false;
    s_project_root[0] = '\0';
    if (!entry_script_path || !entry_script_path[0]) return;

    char dir[1024];
    copy_dirname(entry_script_path, dir, sizeof(dir));
    if (find_project_root(dir, s_project_root, sizeof(s_project_root)))
        s_have_root = true;
}

bool riz_import_resolve(char* out, size_t cap, const char* import_path) {
    if (!import_path || !import_path[0] || !out || cap < 2)
        return false;

    if (file_readable(import_path)) {
        strncpy(out, import_path, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }

    if (s_have_root) {
        snprintf(out, cap, "%s" PATH_SEP_STR "vendor" PATH_SEP_STR "%s", s_project_root, import_path);
        if (file_readable(out))
            return true;
    }

    snprintf(out, cap, "vendor" PATH_SEP_STR "%s", import_path);
    if (file_readable(out))
        return true;

    /* Package layout: import "pkg" → vendor/pkg/main.riz */
    if (!strchr(import_path, '/') && !strchr(import_path, '\\')) {
        if (s_have_root) {
            snprintf(out, cap, "%s" PATH_SEP_STR "vendor" PATH_SEP_STR "%s" PATH_SEP_STR "main.riz",
                     s_project_root, import_path);
            if (file_readable(out))
                return true;
        }
        snprintf(out, cap, "vendor" PATH_SEP_STR "%s" PATH_SEP_STR "main.riz", import_path);
        if (file_readable(out))
            return true;
    }

    out[0] = '\0';
    return false;
}
