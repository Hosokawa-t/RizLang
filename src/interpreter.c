/*
 * Riz Programming Language
 * interpreter.c — Tree-walking interpreter (Phase 3: +struct/impl, try/catch/throw)
 *
 * Phase 2 additions:
 *   - Dictionary type with {key: value} literals
 *   - Pattern matching (match expression)
 *   - String/List/Dict method calls (obj.method())
 *   - import "file.riz" support
 *   - New builtins: format, sorted, reversed, enumerate, zip, keys, values, assert, exit
 *   - More builtins: clamp, sign, floor, ceil, round, all, any, bool, ord, chr, extend
 *   - debug / panic builtins; call stack on uncaught throw
 */

#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "riz_import.h"
#include "riz_plugin.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ═══ Forward declarations ═══ */

static RizValue eval(Interpreter* I, ASTNode* node);
static void exec_block(Interpreter* I, ASTNode* block);
static RizValue call_function(Interpreter* I, RizFunction* fn, RizValue* args, int argc);
static void interpreter_clear_error_stack(Interpreter* I);
static void interpreter_snapshot_error_stack(Interpreter* I);
#ifdef _WIN32
static __thread Interpreter* g_interp = NULL;
#else
static Interpreter* g_interp = NULL;
#endif
static RizValue g_cli_argv = { .type = VAL_NONE, .as = { 0 } };
static char* g_cli_script_path = NULL;

/* Forward: struct method helper */
void riz_struct_add_method(RizStructDef* def, const char* name, RizValue fn_val);

#ifdef _WIN32
#define RIZ_PATH_SEP '\\'
#else
#define RIZ_PATH_SEP '/'
#endif

#include <stdarg.h>

void riz_actual_runtime_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, COL_RED COL_BOLD "RuntimeError: " COL_RESET COL_RED);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, COL_RESET "\n");
    va_end(ap);

    if (g_interp) {
        g_interp->had_error = true;
        longjmp(g_interp->panic_jmp, 1);
    }
}

typedef struct {
    char* data;
    size_t len;
    size_t cap;
    bool ok;
} RizStrBuf;

typedef struct {
    const char* cur;
    const char* err;
} RizJsonParser;

typedef struct {
    char** items;
    int count;
    int cap;
    bool ok;
} RizPathList;

static void sb_init(RizStrBuf* sb) {
    sb->cap = 128;
    sb->len = 0;
    sb->data = (char*)malloc(sb->cap);
    sb->ok = sb->data != NULL;
    if (sb->ok) sb->data[0] = '\0';
}

static bool sb_reserve(RizStrBuf* sb, size_t extra) {
    if (!sb->ok) return false;
    if (sb->len + extra + 1 <= sb->cap) return true;
    while (sb->len + extra + 1 > sb->cap) sb->cap *= 2;
    sb->data = (char*)realloc(sb->data, sb->cap);
    sb->ok = sb->data != NULL;
    return sb->ok;
}

static bool sb_append_n(RizStrBuf* sb, const char* s, size_t n) {
    if (!sb_reserve(sb, n)) return false;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append_cstr(RizStrBuf* sb, const char* s) {
    return sb_append_n(sb, s, strlen(s));
}

static bool sb_append_char(RizStrBuf* sb, char ch) {
    if (!sb_reserve(sb, 1)) return false;
    sb->data[sb->len++] = ch;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append_fmt(RizStrBuf* sb, const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n < sizeof(buf)) return sb_append_n(sb, buf, (size_t)n);
    if (!sb_reserve(sb, (size_t)n)) return false;
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
    return true;
}

static char* sb_take(RizStrBuf* sb) {
    if (!sb->ok) {
        free(sb->data);
        return NULL;
    }
    return sb->data;
}

static void sb_reset(RizStrBuf* sb) {
    if (!sb->data) return;
    sb->len = 0;
    sb->data[0] = '\0';
    sb->ok = true;
}

static void path_list_init(RizPathList* list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    list->ok = true;
}

static void path_list_free(RizPathList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    list->ok = true;
}

static bool path_list_grow(RizPathList* list) {
    if (list->count < list->cap) return true;
    int next = list->cap < 16 ? 16 : list->cap * 2;
    char** items = (char**)realloc(list->items, sizeof(char*) * (size_t)next);
    if (!items) {
        list->ok = false;
        return false;
    }
    list->items = items;
    list->cap = next;
    return true;
}

static bool path_list_push_take(RizPathList* list, char* item) {
    if (!list->ok) {
        free(item);
        return false;
    }
    if (!path_list_grow(list)) {
        free(item);
        return false;
    }
    list->items[list->count++] = item;
    return true;
}

static bool path_list_push_copy(RizPathList* list, const char* item) {
    char* copy = riz_strdup(item ? item : "");
    if (!copy) {
        list->ok = false;
        return false;
    }
    return path_list_push_take(list, copy);
}

static int path_list_cmp(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

static void path_list_sort(RizPathList* list) {
    if (list->count > 1) {
        qsort(list->items, (size_t)list->count, sizeof(char*), path_list_cmp);
    }
}

static void path_list_dedup(RizPathList* list) {
    int write = 0;
    for (int read = 0; read < list->count; read++) {
        if (write > 0 && strcmp(list->items[write - 1], list->items[read]) == 0) {
            free(list->items[read]);
            list->items[read] = NULL;
            continue;
        }
        list->items[write++] = list->items[read];
    }
    list->count = write;
}

static RizValue path_list_to_riz_list(RizPathList* list) {
    RizValue out = riz_list_new();
    for (int i = 0; i < list->count; i++) {
        riz_list_append(out.as.list, riz_string_take(list->items[i]));
        list->items[i] = NULL;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    list->ok = true;
    return out;
}

static void cli_context_clear(void) {
    if (g_cli_argv.type != VAL_NONE) {
        riz_value_free(&g_cli_argv);
    }
    g_cli_argv = riz_none();
    free(g_cli_script_path);
    g_cli_script_path = NULL;
}

void riz_runtime_set_cli_context(const char* script_path, int argc, char** argv) {
    cli_context_clear();
    g_cli_argv = riz_list_new();
    for (int i = 0; i < argc; i++) {
        riz_list_append(g_cli_argv.as.list, riz_string(argv[i] ? argv[i] : ""));
    }
    if (script_path && script_path[0]) {
        g_cli_script_path = riz_strdup(script_path);
    }
}

static RizValue cli_argv_copy(void) {
    if (g_cli_argv.type == VAL_LIST) {
        return riz_value_copy(g_cli_argv);
    }
    return riz_list_new();
}

static bool path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

static bool path_is_absolute(const char* path) {
    if (!path || !path[0]) return false;
    if (path_is_sep(path[0])) return true;
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

static size_t path_root_len(const char* path) {
    if (!path || !path[0]) return 0;
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        if (path_is_sep(path[2])) return 3;
        return 2;
    }
#endif
    if (path_is_sep(path[0])) return 1;
    return 0;
}

static bool path_is_dot_or_dotdot(const char* name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool path_has_wildcards(const char* text) {
    for (const char* p = text; *p; p++) {
        if (*p == '*' || *p == '?') return true;
    }
    return false;
}

static char fold_match_char(char ch) {
#ifdef _WIN32
    return (char)tolower((unsigned char)ch);
#else
    return ch;
#endif
}

static bool wildcard_match(const char* pattern, const char* text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return true;
            while (*text) {
                if (wildcard_match(pattern, text)) return true;
                text++;
            }
            return false;
        }
        if (*pattern == '?') {
            if (!*text) return false;
            pattern++;
            text++;
            continue;
        }
        if (fold_match_char(*pattern) != fold_match_char(*text)) return false;
        pattern++;
        if (!*text) return false;
        text++;
    }
    return *text == '\0';
}

static char* path_join2(const char* base, const char* part) {
    RizStrBuf sb;
    size_t start = 0;
    sb_init(&sb);
    if (!part) part = "";
    if (path_is_absolute(part)) {
        if (!sb_append_cstr(&sb, part)) return sb_take(&sb);
        return sb_take(&sb);
    }
    if (base && base[0] && strcmp(base, ".") != 0) {
        if (!sb_append_cstr(&sb, base)) return sb_take(&sb);
    }
    while (part[start] && path_is_sep(part[start])) start++;
    if (sb.len == 0) {
        if (part[start]) {
            if (!sb_append_cstr(&sb, part + start)) return sb_take(&sb);
        } else if (base && base[0]) {
            if (!sb_append_cstr(&sb, base)) return sb_take(&sb);
        } else {
            if (!sb_append_char(&sb, '.')) return sb_take(&sb);
        }
        return sb_take(&sb);
    }
    if (!path_is_sep(sb.data[sb.len - 1]) && part[start]) {
        if (!sb_append_char(&sb, RIZ_PATH_SEP)) return sb_take(&sb);
    }
    if (part[start] && !sb_append_cstr(&sb, part + start)) return sb_take(&sb);
    return sb_take(&sb);
}

static char* path_join_display(const char* base, const char* part) {
    if (!base || !base[0] || strcmp(base, ".") == 0) {
        return riz_strdup(part ? part : "");
    }
    return path_join2(base, part);
}

static long file_size_or_neg(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long n = ftell(f);
    if (n < 0) return -1;
    if (fseek(f, 0, SEEK_SET) != 0) return -1;
    return n;
}

static char* read_file_alloc(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    long sz = file_size_or_neg(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool write_file_bytes(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static bool fs_path_exists(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static bool fs_is_dir(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

static bool fs_is_file(const char* path) {
    return fs_path_exists(path) && !fs_is_dir(path);
}

static bool fs_list_dir_names(const char* path, RizPathList* out, const char** err) {
#ifdef _WIN32
    char* pattern = path_join2(path, "*");
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    if (!pattern) {
        if (err) *err = "out of memory";
        return false;
    }
    handle = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        if (err) *err = "could not open directory";
        return false;
    }
    do {
        if (path_is_dot_or_dotdot(entry.cFileName)) continue;
        if (!path_list_push_copy(out, entry.cFileName)) {
            if (err) *err = "out of memory";
            FindClose(handle);
            return false;
        }
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
#else
    DIR* dir = opendir(path);
    if (!dir) {
        if (err) *err = "could not open directory";
        return false;
    }
    while (true) {
        struct dirent* entry = readdir(dir);
        if (!entry) break;
        if (path_is_dot_or_dotdot(entry->d_name)) continue;
        if (!path_list_push_copy(out, entry->d_name)) {
            if (err) *err = "out of memory";
            closedir(dir);
            return false;
        }
    }
    closedir(dir);
#endif
    path_list_sort(out);
    return true;
}

static bool fs_mkdir_single(const char* path) {
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return true;
    return fs_is_dir(path);
#else
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST && fs_is_dir(path)) return true;
    return false;
#endif
}

static bool fs_mkdir_parents(const char* path) {
    char* work;
    size_t len;
    size_t root;
    if (!path || !path[0]) return false;
    work = riz_strdup(path);
    if (!work) return false;
    len = strlen(work);
    root = path_root_len(work);
    for (size_t i = root; i <= len; i++) {
        if (work[i] != '\0' && !path_is_sep(work[i])) continue;
        char saved = work[i];
        work[i] = '\0';
        if (work[0] &&
            !(root == 1 && i == 1 && path_is_sep(work[0])) &&
            !(root >= 2 && i <= root)) {
            if (!fs_mkdir_single(work)) {
                free(work);
                return false;
            }
        }
        if (saved == '\0') break;
        work[i] = RIZ_PATH_SEP;
        while (path_is_sep(work[i + 1])) i++;
    }
    free(work);
    return true;
}

static bool split_path_segments(const char* pattern, char** root_out, RizPathList* segments, const char** err) {
    size_t root_len;
    const char* p;
    path_list_init(segments);
    root_len = path_root_len(pattern);
    if (root_len > 0) {
        *root_out = riz_strndup(pattern, root_len);
    } else {
        *root_out = riz_strdup(".");
    }
    if (!*root_out) {
        if (err) *err = "out of memory";
        return false;
    }
    p = pattern + root_len;
    while (*p) {
        while (path_is_sep(*p)) p++;
        if (!*p) break;
        const char* start = p;
        while (*p && !path_is_sep(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (!path_list_push_take(segments, riz_strndup(start, len))) {
            if (err) *err = "out of memory";
            free(*root_out);
            *root_out = NULL;
            path_list_free(segments);
            return false;
        }
    }
    return true;
}

static bool walk_dir_recursive(const char* current_path, const char* display_path, RizPathList* out, const char** err) {
    RizPathList names;
    path_list_init(&names);
    if (!fs_list_dir_names(current_path, &names, err)) {
        path_list_free(&names);
        return false;
    }
    for (int i = 0; i < names.count; i++) {
        char* child_path = path_join2(current_path, names.items[i]);
        char* child_display = path_join_display(display_path, names.items[i]);
        bool is_dir;
        if (!child_path || !child_display) {
            free(child_path);
            free(child_display);
            path_list_free(&names);
            if (err) *err = "out of memory";
            return false;
        }
        if (!path_list_push_take(out, child_display)) {
            free(child_path);
            path_list_free(&names);
            if (err) *err = "out of memory";
            return false;
        }
        is_dir = fs_is_dir(child_path);
        if (is_dir && !walk_dir_recursive(child_path, out->items[out->count - 1], out, err)) {
            free(child_path);
            path_list_free(&names);
            return false;
        }
        free(child_path);
    }
    path_list_free(&names);
    return true;
}

static bool glob_recursive(const char* current_path, const char* display_path, RizPathList* segments, int idx,
                           RizPathList* out, const char** err) {
    if (idx >= segments->count) {
        const char* result = (display_path && display_path[0]) ? display_path : current_path;
        return path_list_push_copy(out, result);
    }

    const char* seg = segments->items[idx];
    if (strcmp(seg, "**") == 0) {
        if (!glob_recursive(current_path, display_path, segments, idx + 1, out, err)) return false;
        RizPathList names;
        path_list_init(&names);
        if (!fs_list_dir_names(current_path, &names, err)) {
            path_list_free(&names);
            return false;
        }
        for (int i = 0; i < names.count; i++) {
            char* child_path = path_join2(current_path, names.items[i]);
            char* child_display = path_join_display(display_path, names.items[i]);
            bool ok = true;
            if (!child_path || !child_display) {
                free(child_path);
                free(child_display);
                path_list_free(&names);
                if (err) *err = "out of memory";
                return false;
            }
            if (fs_is_dir(child_path)) {
                ok = glob_recursive(child_path, child_display, segments, idx, out, err);
            }
            free(child_path);
            free(child_display);
            if (!ok) {
                path_list_free(&names);
                return false;
            }
        }
        path_list_free(&names);
        return true;
    }

    if (!path_has_wildcards(seg)) {
        char* child_path = path_join2(current_path, seg);
        char* child_display = path_join_display(display_path, seg);
        bool ok = true;
        if (!child_path || !child_display) {
            free(child_path);
            free(child_display);
            if (err) *err = "out of memory";
            return false;
        }
        if (!fs_path_exists(child_path)) ok = true;
        else if (idx == segments->count - 1) ok = path_list_push_take(out, child_display);
        else if (fs_is_dir(child_path)) ok = glob_recursive(child_path, child_display, segments, idx + 1, out, err);
        else ok = true;
        if (idx != segments->count - 1 || !fs_path_exists(child_path)) free(child_display);
        free(child_path);
        return ok;
    }

    RizPathList names;
    path_list_init(&names);
    if (!fs_list_dir_names(current_path, &names, err)) {
        path_list_free(&names);
        return false;
    }
    for (int i = 0; i < names.count; i++) {
        char* child_path;
        char* child_display;
        bool ok = true;
        if (!wildcard_match(seg, names.items[i])) continue;
        child_path = path_join2(current_path, names.items[i]);
        child_display = path_join_display(display_path, names.items[i]);
        if (!child_path || !child_display) {
            free(child_path);
            free(child_display);
            path_list_free(&names);
            if (err) *err = "out of memory";
            return false;
        }
        if (idx == segments->count - 1) {
            ok = path_list_push_take(out, child_display);
        } else if (fs_is_dir(child_path)) {
            ok = glob_recursive(child_path, child_display, segments, idx + 1, out, err);
            free(child_display);
        } else {
            free(child_display);
        }
        free(child_path);
        if (!ok) {
            path_list_free(&names);
            return false;
        }
    }
    path_list_free(&names);
    return true;
}

static bool row_is_blank(RizValue row) {
    if (row.type != VAL_LIST) return true;
    for (int i = 0; i < row.as.list->count; i++) {
        RizValue item = row.as.list->items[i];
        if (item.type == VAL_STRING && item.as.string[0] != '\0') return false;
    }
    return true;
}

static bool row_push_field(RizValue* row, RizStrBuf* field) {
    char* text = riz_strdup(field->data ? field->data : "");
    if (!text) return false;
    riz_list_append(row->as.list, riz_string_take(text));
    sb_reset(field);
    return true;
}

static bool parse_delimited_rows(const char* text, char delim, RizValue* out, const char** err) {
    RizValue rows = riz_list_new();
    RizValue row = riz_list_new();
    RizStrBuf field;
    bool in_quotes = false;
    bool pending_row = false;
    sb_init(&field);
    if (!field.data) {
        riz_value_free(&rows);
        riz_value_free(&row);
        if (err) *err = "out of memory";
        return false;
    }
    for (const char* p = text;;) {
        char ch = *p;
        if (in_quotes) {
            if (ch == '\0') {
                free(field.data);
                riz_value_free(&rows);
                riz_value_free(&row);
                if (err) *err = "unterminated quoted field";
                return false;
            }
            if (ch == '"') {
                if (p[1] == '"') {
                    if (!sb_append_char(&field, '"')) {
                        free(field.data);
                        riz_value_free(&rows);
                        riz_value_free(&row);
                        if (err) *err = "out of memory";
                        return false;
                    }
                    p += 2;
                    pending_row = true;
                    continue;
                }
                in_quotes = false;
                p++;
                pending_row = true;
                continue;
            }
            if (!sb_append_char(&field, ch)) {
                free(field.data);
                riz_value_free(&rows);
                riz_value_free(&row);
                if (err) *err = "out of memory";
                return false;
            }
            p++;
            pending_row = true;
            continue;
        }
        if (ch == '"') {
            in_quotes = true;
            p++;
            pending_row = true;
            continue;
        }
        if (ch == delim) {
            if (!row_push_field(&row, &field)) {
                free(field.data);
                riz_value_free(&rows);
                riz_value_free(&row);
                if (err) *err = "out of memory";
                return false;
            }
            p++;
            pending_row = true;
            continue;
        }
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            if (pending_row || field.len > 0 || row.as.list->count > 0) {
                if (!row_push_field(&row, &field)) {
                    free(field.data);
                    riz_value_free(&rows);
                    riz_value_free(&row);
                    if (err) *err = "out of memory";
                    return false;
                }
                if (!row_is_blank(row)) {
                    riz_list_append(rows.as.list, row);
                } else {
                    riz_value_free(&row);
                }
                row = riz_list_new();
            }
            if (ch == '\0') break;
            if (ch == '\r' && p[1] == '\n') p++;
            p++;
            pending_row = false;
            continue;
        }
        if (!sb_append_char(&field, ch)) {
            free(field.data);
            riz_value_free(&rows);
            riz_value_free(&row);
            if (err) *err = "out of memory";
            return false;
        }
        pending_row = true;
        p++;
    }
    free(field.data);
    riz_value_free(&row);
    *out = rows;
    return true;
}

static RizValue rows_to_records(RizValue rows) {
    RizValue result = riz_list_new();
    if (rows.type != VAL_LIST || rows.as.list->count == 0) return result;
    RizValue header_row = rows.as.list->items[0];
    if (header_row.type != VAL_LIST) return result;
    for (int i = 1; i < rows.as.list->count; i++) {
        RizValue source_row = rows.as.list->items[i];
        RizValue record = riz_dict_new();
        int header_count = header_row.as.list->count;
        int row_count = source_row.type == VAL_LIST ? source_row.as.list->count : 0;
        int col_count = header_count > row_count ? header_count : row_count;
        for (int col = 0; col < col_count; col++) {
            char name_buf[32];
            const char* key = NULL;
            RizValue value = riz_string("");
            if (col < header_count && header_row.as.list->items[col].type == VAL_STRING &&
                header_row.as.list->items[col].as.string[0] != '\0') {
                key = header_row.as.list->items[col].as.string;
            } else {
                snprintf(name_buf, sizeof(name_buf), "col%d", col + 1);
                key = name_buf;
            }
            if (source_row.type == VAL_LIST && col < row_count) {
                riz_value_free(&value);
                value = riz_value_copy(source_row.as.list->items[col]);
            }
            riz_dict_set(record.as.dict, key, value);
        }
        riz_list_append(result.as.list, record);
    }
    return result;
}

static RizValue list_from_lines(const char* text) {
    RizValue result = riz_list_new();
    const char* start = text;
    const char* p = text;
    while (*p) {
        if (*p == '\r' || *p == '\n') {
            size_t len = (size_t)(p - start);
            char* line = riz_strndup(start, len);
            riz_list_append(result.as.list, riz_string_take(line));
            if (*p == '\r' && p[1] == '\n') p++;
            p++;
            start = p;
            continue;
        }
        p++;
    }
    if (p > start) {
        char* line = riz_strndup(start, (size_t)(p - start));
        riz_list_append(result.as.list, riz_string_take(line));
    }
    return result;
}

static char* basename_dup(const char* path) {
    size_t len = strlen(path);
    while (len > 0 && path_is_sep(path[len - 1])) len--;
#ifdef _WIN32
    if (len == 2 && isalpha((unsigned char)path[0]) && path[1] == ':')
        return riz_strndup(path, len);
#endif
    if (len == 0) return riz_strdup("");
    size_t start = len;
    while (start > 0 && !path_is_sep(path[start - 1])) start--;
    return riz_strndup(path + start, len - start);
}

static char* dirname_dup(const char* path) {
    size_t len = strlen(path);
    while (len > 1 && path_is_sep(path[len - 1])) len--;
#ifdef _WIN32
    if (len >= 2 && isalpha((unsigned char)path[0]) && path[1] == ':') {
        if (len == 2) return riz_strndup(path, 2);
        if (len == 3 && path_is_sep(path[2])) return riz_strndup(path, 3);
    }
#endif
    if (len == 0) return riz_strdup(".");
    while (len > 0 && !path_is_sep(path[len - 1])) len--;
    if (len == 0) return riz_strdup(".");
    while (len > 1 && path_is_sep(path[len - 1])) len--;
    return riz_strndup(path, len);
}

static const char* json_skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool json_hex_digit(char ch, unsigned* out) {
    if (ch >= '0' && ch <= '9') { *out = (unsigned)(ch - '0'); return true; }
    if (ch >= 'a' && ch <= 'f') { *out = (unsigned)(ch - 'a' + 10); return true; }
    if (ch >= 'A' && ch <= 'F') { *out = (unsigned)(ch - 'A' + 10); return true; }
    return false;
}

static bool json_parse_hex4(const char* s, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned d = 0;
        if (!json_hex_digit(s[i], &d)) return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static bool sb_append_codepoint(RizStrBuf* sb, unsigned code) {
    if (code <= 0x7F) return sb_append_char(sb, (char)code);
    if (code <= 0x7FF) {
        return sb_append_char(sb, (char)(0xC0 | (code >> 6))) &&
               sb_append_char(sb, (char)(0x80 | (code & 0x3F)));
    }
    if (code <= 0xFFFF) {
        return sb_append_char(sb, (char)(0xE0 | (code >> 12))) &&
               sb_append_char(sb, (char)(0x80 | ((code >> 6) & 0x3F))) &&
               sb_append_char(sb, (char)(0x80 | (code & 0x3F)));
    }
    if (code <= 0x10FFFF) {
        return sb_append_char(sb, (char)(0xF0 | (code >> 18))) &&
               sb_append_char(sb, (char)(0x80 | ((code >> 12) & 0x3F))) &&
               sb_append_char(sb, (char)(0x80 | ((code >> 6) & 0x3F))) &&
               sb_append_char(sb, (char)(0x80 | (code & 0x3F)));
    }
    return false;
}

static bool json_parse_string_raw(RizJsonParser* jp, char** out) {
    RizStrBuf sb;
    sb_init(&sb);
    if (*jp->cur != '"') {
        jp->err = "expected string";
        free(sb.data);
        return false;
    }
    jp->cur++;
    while (*jp->cur && *jp->cur != '"') {
        if ((unsigned char)*jp->cur < 0x20) {
            jp->err = "control character in string";
            free(sb.data);
            return false;
        }
        if (*jp->cur != '\\') {
            if (!sb_append_char(&sb, *jp->cur++)) {
                jp->err = "out of memory";
                free(sb.data);
                return false;
            }
            continue;
        }
        jp->cur++;
        switch (*jp->cur) {
            case '"': if (!sb_append_char(&sb, '"')) jp->err = "out of memory"; jp->cur++; break;
            case '\\': if (!sb_append_char(&sb, '\\')) jp->err = "out of memory"; jp->cur++; break;
            case '/': if (!sb_append_char(&sb, '/')) jp->err = "out of memory"; jp->cur++; break;
            case 'b': if (!sb_append_char(&sb, '\b')) jp->err = "out of memory"; jp->cur++; break;
            case 'f': if (!sb_append_char(&sb, '\f')) jp->err = "out of memory"; jp->cur++; break;
            case 'n': if (!sb_append_char(&sb, '\n')) jp->err = "out of memory"; jp->cur++; break;
            case 'r': if (!sb_append_char(&sb, '\r')) jp->err = "out of memory"; jp->cur++; break;
            case 't': if (!sb_append_char(&sb, '\t')) jp->err = "out of memory"; jp->cur++; break;
            case 'u': {
                unsigned code = 0;
                if (!json_parse_hex4(jp->cur + 1, &code)) {
                    jp->err = "invalid unicode escape";
                    free(sb.data);
                    return false;
                }
                jp->cur += 5;
                if (code >= 0xD800 && code <= 0xDBFF && jp->cur[0] == '\\' && jp->cur[1] == 'u') {
                    unsigned low = 0;
                    if (!json_parse_hex4(jp->cur + 2, &low) || low < 0xDC00 || low > 0xDFFF) {
                        jp->err = "invalid unicode surrogate pair";
                        free(sb.data);
                        return false;
                    }
                    code = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                    jp->cur += 6;
                }
                if (!sb_append_codepoint(&sb, code)) {
                    jp->err = "invalid unicode codepoint";
                    free(sb.data);
                    return false;
                }
                break;
            }
            default:
                jp->err = "invalid string escape";
                free(sb.data);
                return false;
        }
        if (jp->err) {
            free(sb.data);
            return false;
        }
    }
    if (*jp->cur != '"') {
        jp->err = "unterminated string";
        free(sb.data);
        return false;
    }
    jp->cur++;
    *out = sb_take(&sb);
    if (!*out) {
        jp->err = "out of memory";
        return false;
    }
    return true;
}

static bool json_parse_value(RizJsonParser* jp, RizValue* out);

static bool json_parse_array(RizJsonParser* jp, RizValue* out) {
    RizValue list = riz_list_new();
    jp->cur++;
    jp->cur = json_skip_ws(jp->cur);
    if (*jp->cur == ']') {
        jp->cur++;
        *out = list;
        return true;
    }
    while (true) {
        RizValue item = riz_none();
        if (!json_parse_value(jp, &item)) {
            riz_value_free(&list);
            return false;
        }
        riz_list_append(list.as.list, item);
        jp->cur = json_skip_ws(jp->cur);
        if (*jp->cur == ']') {
            jp->cur++;
            *out = list;
            return true;
        }
        if (*jp->cur != ',') {
            jp->err = "expected ',' or ']'";
            riz_value_free(&list);
            return false;
        }
        jp->cur++;
        jp->cur = json_skip_ws(jp->cur);
    }
}

static bool json_parse_object(RizJsonParser* jp, RizValue* out) {
    RizValue dict = riz_dict_new();
    jp->cur++;
    jp->cur = json_skip_ws(jp->cur);
    if (*jp->cur == '}') {
        jp->cur++;
        *out = dict;
        return true;
    }
    while (true) {
        char* key = NULL;
        RizValue value = riz_none();
        if (!json_parse_string_raw(jp, &key)) {
            riz_value_free(&dict);
            return false;
        }
        jp->cur = json_skip_ws(jp->cur);
        if (*jp->cur != ':') {
            free(key);
            jp->err = "expected ':'";
            riz_value_free(&dict);
            return false;
        }
        jp->cur++;
        jp->cur = json_skip_ws(jp->cur);
        if (!json_parse_value(jp, &value)) {
            free(key);
            riz_value_free(&dict);
            return false;
        }
        riz_dict_set(dict.as.dict, key, value);
        free(key);
        jp->cur = json_skip_ws(jp->cur);
        if (*jp->cur == '}') {
            jp->cur++;
            *out = dict;
            return true;
        }
        if (*jp->cur != ',') {
            jp->err = "expected ',' or '}'";
            riz_value_free(&dict);
            return false;
        }
        jp->cur++;
        jp->cur = json_skip_ws(jp->cur);
    }
}

static bool json_parse_number(RizJsonParser* jp, RizValue* out) {
    const char* start = jp->cur;
    char* end = NULL;
    double value = strtod(start, &end);
    if (end == start) {
        jp->err = "invalid number";
        return false;
    }
    bool is_float = false;
    for (const char* p = start; p < end; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') {
            is_float = true;
            break;
        }
    }
    jp->cur = end;
    *out = is_float ? riz_float(value) : riz_int((int64_t)value);
    return true;
}

static bool json_parse_value(RizJsonParser* jp, RizValue* out) {
    jp->cur = json_skip_ws(jp->cur);
    switch (*jp->cur) {
        case '"': {
            char* text = NULL;
            if (!json_parse_string_raw(jp, &text)) return false;
            *out = riz_string_take(text);
            return true;
        }
        case '{':
            return json_parse_object(jp, out);
        case '[':
            return json_parse_array(jp, out);
        case 't':
            if (strncmp(jp->cur, "true", 4) == 0) { jp->cur += 4; *out = riz_bool(true); return true; }
            break;
        case 'f':
            if (strncmp(jp->cur, "false", 5) == 0) { jp->cur += 5; *out = riz_bool(false); return true; }
            break;
        case 'n':
            if (strncmp(jp->cur, "null", 4) == 0) { jp->cur += 4; *out = riz_none(); return true; }
            break;
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return json_parse_number(jp, out);
        default:
            break;
    }
    jp->err = "invalid JSON value";
    return false;
}

static bool json_write_indent(RizStrBuf* sb, int depth) {
    if (!sb_append_char(sb, '\n')) return false;
    for (int i = 0; i < depth; i++) {
        if (!sb_append_cstr(sb, "  ")) return false;
    }
    return true;
}

static bool json_stringify_value(RizStrBuf* sb, RizValue value, bool pretty, int depth);

static bool json_escape_and_append(RizStrBuf* sb, const char* s) {
    if (!sb_append_char(sb, '"')) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"': if (!sb_append_cstr(sb, "\\\"")) return false; break;
            case '\\': if (!sb_append_cstr(sb, "\\\\")) return false; break;
            case '\b': if (!sb_append_cstr(sb, "\\b")) return false; break;
            case '\f': if (!sb_append_cstr(sb, "\\f")) return false; break;
            case '\n': if (!sb_append_cstr(sb, "\\n")) return false; break;
            case '\r': if (!sb_append_cstr(sb, "\\r")) return false; break;
            case '\t': if (!sb_append_cstr(sb, "\\t")) return false; break;
            default:
                if (*p < 0x20) {
                    if (!sb_append_fmt(sb, "\\u%04x", (unsigned)*p)) return false;
                } else if (!sb_append_char(sb, (char)*p)) {
                    return false;
                }
                break;
        }
    }
    return sb_append_char(sb, '"');
}

static bool json_stringify_object_fields(RizStrBuf* sb, bool pretty, int depth, int count, char** keys, RizValue* values) {
    if (!sb_append_char(sb, '{')) return false;
    if (count == 0) return sb_append_char(sb, '}');
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (!sb_append_char(sb, ',')) return false;
        }
        if (pretty && !json_write_indent(sb, depth + 1)) return false;
        if (!json_escape_and_append(sb, keys[i])) return false;
        if (!sb_append_cstr(sb, pretty ? ": " : ":")) return false;
        if (!json_stringify_value(sb, values[i], pretty, depth + 1)) return false;
    }
    if (pretty && !json_write_indent(sb, depth)) return false;
    return sb_append_char(sb, '}');
}

static bool json_stringify_value(RizStrBuf* sb, RizValue value, bool pretty, int depth) {
    switch (value.type) {
        case VAL_INT:
            return sb_append_fmt(sb, "%lld", (long long)value.as.integer);
        case VAL_FLOAT:
            return sb_append_fmt(sb, "%.17g", value.as.floating);
        case VAL_BOOL:
            return sb_append_cstr(sb, value.as.boolean ? "true" : "false");
        case VAL_NONE:
            return sb_append_cstr(sb, "null");
        case VAL_STRING:
            return json_escape_and_append(sb, value.as.string);
        case VAL_LIST: {
            RizList* list = value.as.list;
            if (!sb_append_char(sb, '[')) return false;
            if (list->count == 0) return sb_append_char(sb, ']');
            for (int i = 0; i < list->count; i++) {
                if (i > 0) {
                    if (!sb_append_char(sb, ',')) return false;
                }
                if (pretty && !json_write_indent(sb, depth + 1)) return false;
                if (!json_stringify_value(sb, list->items[i], pretty, depth + 1)) return false;
            }
            if (pretty && !json_write_indent(sb, depth)) return false;
            return sb_append_char(sb, ']');
        }
        case VAL_DICT:
            return json_stringify_object_fields(sb, pretty, depth, value.as.dict->count,
                                                value.as.dict->keys, value.as.dict->values);
        case VAL_INSTANCE:
            return json_stringify_object_fields(sb, pretty, depth, value.as.instance->def->field_count,
                                                value.as.instance->def->field_names, value.as.instance->fields);
        default:
            return false;
    }
}

/* ═══════════════════════════════════════════════════════
 *  Built-in / Native Functions (Phase 1 + Phase 2)
 * ═══════════════════════════════════════════════════════ */

RizValue native_print(RizValue* args, int argc) {
    for (int i = 0; i < argc; i++) { if (i > 0) printf(" "); riz_value_print(args[i]); }
    printf("\n"); return riz_none();
}
RizValue native_len(RizValue* args, int argc) {
    if (argc!=1){ riz_runtime_error("len() takes 1 argument"); return riz_none(); }
    if (args[0].type==VAL_LIST)   return riz_int(args[0].as.list->count);
    if (args[0].type==VAL_STRING) return riz_int((int64_t)strlen(args[0].as.string));
    if (args[0].type==VAL_DICT)   return riz_int(args[0].as.dict->count);
    riz_runtime_error("len() argument must be list, string, or dict"); return riz_none();
}
RizValue native_range(RizValue* args, int argc) {
    int64_t start=0,stop=0,step=1;
    if (argc==1&&args[0].type==VAL_INT) stop=args[0].as.integer;
    else if (argc==2&&args[0].type==VAL_INT&&args[1].type==VAL_INT) { start=args[0].as.integer; stop=args[1].as.integer; }
    else if (argc==3&&args[0].type==VAL_INT&&args[1].type==VAL_INT&&args[2].type==VAL_INT) { start=args[0].as.integer; stop=args[1].as.integer; step=args[2].as.integer; }
    else { riz_runtime_error("range() takes 1-3 int args"); return riz_none(); }
    if (step==0) { riz_runtime_error("range() step cannot be zero"); return riz_none(); }
    RizValue list = riz_list_new();
    if (step>0) for(int64_t i=start;i<stop;i+=step) riz_list_append(list.as.list, riz_int(i));
    else        for(int64_t i=start;i>stop;i+=step) riz_list_append(list.as.list, riz_int(i));
    return list;
}
RizValue native_type(RizValue* a, int c) { return c==1 ? riz_string(riz_value_type_name(a[0])) : riz_none(); }
RizValue native_str(RizValue* a, int c) {
    if (c!=1) return riz_none();
    char* s = riz_value_to_string(a[0]); return riz_string_take(s);
}
RizValue native_int_cast(RizValue* a, int c) {
    if (c!=1) return riz_none();
    switch (a[0].type) {
        case VAL_INT: return a[0]; case VAL_FLOAT: return riz_int((int64_t)a[0].as.floating);
        case VAL_BOOL: return riz_int(a[0].as.boolean?1:0);
        case VAL_STRING: { char*e; int64_t v=strtoll(a[0].as.string,&e,10);
            if(*e) { riz_runtime_error("Cannot convert '%s' to int",a[0].as.string); return riz_none(); }
            return riz_int(v); }
        default: riz_runtime_error("Cannot convert %s to int",riz_value_type_name(a[0])); return riz_none();
    }
}
RizValue native_float_cast(RizValue* a, int c) {
    if (c!=1) return riz_none();
    switch (a[0].type) {
        case VAL_INT: return riz_float((double)a[0].as.integer); case VAL_FLOAT: return a[0];
        case VAL_BOOL: return riz_float(a[0].as.boolean?1.0:0.0);
        case VAL_STRING: { char*e; double v=strtod(a[0].as.string,&e);
            if(*e) { riz_runtime_error("Cannot convert '%s' to float",a[0].as.string); return riz_none(); }
            return riz_float(v); }
        default: riz_runtime_error("Cannot convert %s to float",riz_value_type_name(a[0])); return riz_none();
    }
}
RizValue native_input(RizValue* a, int c) {
    if (c>0&&a[0].type==VAL_STRING) { printf("%s",a[0].as.string); fflush(stdout); }
    char buf[RIZ_LINE_BUF_SIZE];
    if (fgets(buf,sizeof(buf),stdin)) { size_t l=strlen(buf); if(l>0&&buf[l-1]=='\n')buf[l-1]='\0'; return riz_string(buf); }
    return riz_string("");
}
RizValue native_append(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("append(list, val) expected"); return riz_none(); }
    riz_list_append(a[0].as.list, riz_value_copy(a[1])); return riz_none();
}
RizValue native_pop(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_LIST) return riz_none();
    RizList* l=a[0].as.list; if(l->count==0){riz_runtime_error("pop() from empty list");return riz_none();}
    return l->items[--l->count];
}
RizValue native_abs(RizValue* a, int c) {
    if (c!=1) return riz_none();
    if (a[0].type==VAL_INT){ int64_t v=a[0].as.integer; return riz_int(v<0?-v:v); }
    if (a[0].type==VAL_FLOAT) return riz_float(fabs(a[0].as.floating));
    return riz_none();
}
RizValue native_min(RizValue* a, int c) {
    if (c==0) return riz_none();
    if (c==1&&a[0].type==VAL_LIST) {
        RizList*l=a[0].as.list; if(!l->count)return riz_none(); RizValue b=l->items[0];
        for(int i=1;i<l->count;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=l->items[i].type==VAL_INT?(double)l->items[i].as.integer:l->items[i].as.floating;if(cv<bv)b=l->items[i];}
        return riz_value_copy(b);
    }
    RizValue b=a[0]; for(int i=1;i<c;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=a[i].type==VAL_INT?(double)a[i].as.integer:a[i].as.floating;if(cv<bv)b=a[i];}
    return riz_value_copy(b);
}
RizValue native_max(RizValue* a, int c) {
    if (c==0) return riz_none();
    if (c==1&&a[0].type==VAL_LIST) {
        RizList*l=a[0].as.list; if(!l->count)return riz_none(); RizValue b=l->items[0];
        for(int i=1;i<l->count;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=l->items[i].type==VAL_INT?(double)l->items[i].as.integer:l->items[i].as.floating;if(cv>bv)b=l->items[i];}
        return riz_value_copy(b);
    }
    RizValue b=a[0]; for(int i=1;i<c;i++){double bv=b.type==VAL_INT?(double)b.as.integer:b.as.floating;double cv=a[i].type==VAL_INT?(double)a[i].as.integer:a[i].as.floating;if(cv>bv)b=a[i];}
    return riz_value_copy(b);
}
RizValue native_sum(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_LIST) return riz_none();
    bool hf=false; double t=0;
    for(int i=0;i<a[0].as.list->count;i++){
        if(a[0].as.list->items[i].type==VAL_INT) t+=(double)a[0].as.list->items[i].as.integer;
        else if(a[0].as.list->items[i].type==VAL_FLOAT){t+=a[0].as.list->items[i].as.floating;hf=true;}
    }
    return hf?riz_float(t):riz_int((int64_t)t);
}

typedef struct {
    const RizValue* items;
    int start;
    int end;
    double partial;
} ParallelSumTask;

static int native_cpu_count_raw(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)(info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1);
#else
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && n <= INT_MAX) return (int)n;
#endif
    return 1;
#endif
}

static double sum_numeric_slice(const RizValue* items, int start, int end) {
    double total = 0.0;
    for (int i = start; i < end; i++) {
        if (items[i].type == VAL_INT) total += (double)items[i].as.integer;
        else total += items[i].as.floating;
    }
    return total;
}

static RizValue make_numeric_sum_value(double total, bool has_float) {
    return has_float ? riz_float(total) : riz_int((int64_t)total);
}

#ifdef _WIN32
static DWORD WINAPI parallel_sum_worker(LPVOID param) {
    ParallelSumTask* task = (ParallelSumTask*)param;
    task->partial = sum_numeric_slice(task->items, task->start, task->end);
    return 0;
}
#endif

RizValue native_cpu_count(RizValue* args, int argc) {
    (void)args;
    if (argc != 0) {
        riz_runtime_error("cpu_count() takes no arguments");
        return riz_none();
    }
    return riz_int((int64_t)native_cpu_count_raw());
}

RizValue native_parallel_sum(RizValue* args, int argc) {
    if (argc < 1 || argc > 2 || args[0].type != VAL_LIST) {
        riz_runtime_error("parallel_sum(list[, workers]) expected");
        return riz_none();
    }

    int requested_workers = 0;
    if (argc == 2) {
        if (args[1].type != VAL_INT) {
            riz_runtime_error("parallel_sum(): workers must be int");
            return riz_none();
        }
        if (args[1].as.integer <= 0) {
            riz_runtime_error("parallel_sum(): workers must be >= 1");
            return riz_none();
        }
        requested_workers = (args[1].as.integer > INT_MAX) ? INT_MAX : (int)args[1].as.integer;
    }

    RizList* list = args[0].as.list;
    if (list->count == 0) return riz_int(0);

    bool has_float = false;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type == VAL_INT) continue;
        if (list->items[i].type == VAL_FLOAT) {
            has_float = true;
            continue;
        }
        riz_runtime_error("parallel_sum(): list must contain only int/float");
        return riz_none();
    }

    int workers = requested_workers > 0 ? requested_workers : native_cpu_count_raw();
    if (workers < 1) workers = 1;
    if (workers > list->count) workers = list->count;

    if (workers <= 1 || list->count < 4096) {
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

#ifdef _WIN32
    ParallelSumTask* tasks = (ParallelSumTask*)calloc((size_t)workers, sizeof(ParallelSumTask));
    HANDLE* handles = (HANDLE*)calloc((size_t)workers, sizeof(HANDLE));
    if (!tasks || !handles) {
        free(tasks);
        free(handles);
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

    int base = list->count / workers;
    int rem = list->count % workers;
    int start = 0;
    int launched = 0;
    bool spawn_failed = false;

    for (int i = 0; i < workers; i++) {
        int span = base + (i < rem ? 1 : 0);
        tasks[i].items = list->items;
        tasks[i].start = start;
        tasks[i].end = start + span;
        start += span;

        handles[i] = CreateThread(NULL, 0, parallel_sum_worker, &tasks[i], 0, NULL);
        if (!handles[i]) {
            spawn_failed = true;
            break;
        }
        launched++;
    }

    if (spawn_failed) {
        for (int i = 0; i < launched; i++) {
            WaitForSingleObject(handles[i], INFINITE);
            CloseHandle(handles[i]);
        }
        free(tasks);
        free(handles);
        double total = sum_numeric_slice(list->items, 0, list->count);
        return make_numeric_sum_value(total, has_float);
    }

    double total = 0.0;
    for (int i = 0; i < workers; i++) {
        WaitForSingleObject(handles[i], INFINITE);
        CloseHandle(handles[i]);
        total += tasks[i].partial;
    }
    free(tasks);
    free(handles);
    return make_numeric_sum_value(total, has_float);
#else
    /* Non-Windows builds keep compatibility by falling back to serial sum. */
    double total = sum_numeric_slice(list->items, 0, list->count);
    return make_numeric_sum_value(total, has_float);
#endif
}

RizValue native_time_fn(RizValue* a, int c) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ft.dwHighDateTime;
    t <<= 32;
    t |= ft.dwLowDateTime;
    return riz_float((double)t / 10000000.0 - 11644473600.0);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return riz_float((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0);
#endif
}

/* Phase 2 native map/filter — forward declared */
RizValue native_map(RizValue* a, int c);
RizValue native_filter(RizValue* a, int c);

/* Phase 2 new builtins */

RizValue native_format(RizValue* args, int argc) {
    if (argc<1||args[0].type!=VAL_STRING) { riz_runtime_error("format() first arg must be string"); return riz_none(); }
    const char* tmpl = args[0].as.string;
    size_t cap=256; char* res=(char*)malloc(cap); size_t len=0;
    int arg_idx = 1;
    for (const char* p = tmpl; *p; p++) {
        if (*p=='{' && *(p+1)=='}') {
            if (arg_idx < argc) {
                char* s = riz_value_to_string(args[arg_idx++]);
                size_t sl=strlen(s); while(len+sl+1>=cap){cap*=2;res=(char*)realloc(res,cap);}
                memcpy(res+len,s,sl); len+=sl; free(s);
            }
            p++; /* skip '}' */
        } else {
            if(len+2>=cap){cap*=2;res=(char*)realloc(res,cap);}
            res[len++]=*p;
        }
    }
    res[len]='\0'; return riz_string_take(res);
}

RizValue native_sorted(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) { riz_runtime_error("sorted() takes 1 list arg"); return riz_none(); }
    RizList* src = args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=0;i<src->count;i++) riz_list_append(result.as.list, riz_value_copy(src->items[i]));
    /* insertion sort */
    RizList* l = result.as.list;
    for (int i=1;i<l->count;i++) {
        RizValue key=l->items[i]; int j=i-1;
        double kv = key.type==VAL_INT?(double)key.as.integer:key.as.floating;
        while(j>=0) {
            double jv=l->items[j].type==VAL_INT?(double)l->items[j].as.integer:l->items[j].as.floating;
            if (jv<=kv) break;
            l->items[j+1]=l->items[j]; j--;
        }
        l->items[j+1]=key;
    }
    return result;
}

RizValue native_reversed(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) return riz_none();
    RizList* src = args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=src->count-1;i>=0;i--) riz_list_append(result.as.list, riz_value_copy(src->items[i]));
    return result;
}

RizValue native_enumerate(RizValue* args, int argc) {
    if (argc!=1||args[0].type!=VAL_LIST) return riz_none();
    RizList* src=args[0].as.list;
    RizValue result = riz_list_new();
    for (int i=0;i<src->count;i++) {
        RizValue pair = riz_list_new();
        riz_list_append(pair.as.list, riz_int(i));
        riz_list_append(pair.as.list, riz_value_copy(src->items[i]));
        riz_list_append(result.as.list, pair);
    }
    return result;
}

RizValue native_zip(RizValue* args, int argc) {
    if (argc!=2||args[0].type!=VAL_LIST||args[1].type!=VAL_LIST) return riz_none();
    RizList*a=args[0].as.list; RizList*b=args[1].as.list;
    int cnt = a->count<b->count ? a->count : b->count;
    RizValue result = riz_list_new();
    for (int i=0;i<cnt;i++) {
        RizValue pair = riz_list_new();
        riz_list_append(pair.as.list, riz_value_copy(a->items[i]));
        riz_list_append(pair.as.list, riz_value_copy(b->items[i]));
        riz_list_append(result.as.list, pair);
    }
    return result;
}

RizValue native_keys(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_DICT){riz_runtime_error("keys() takes 1 dict arg");return riz_none();}
    return riz_dict_keys(a[0].as.dict);
}
RizValue native_values(RizValue* a, int c) {
    if (c!=1||a[0].type!=VAL_DICT){riz_runtime_error("values() takes 1 dict arg");return riz_none();}
    return riz_dict_values(a[0].as.dict);
}

RizValue native_assert(RizValue* args, int argc) {
    if (argc<1) { riz_runtime_error("assert() requires at least 1 argument"); return riz_none(); }
    if (!riz_value_is_truthy(args[0])) {
        if (argc>=2 && args[1].type==VAL_STRING)
            riz_runtime_error("Assertion failed: %s", args[1].as.string);
        else
            riz_runtime_error("Assertion failed");
    }
    return riz_none();
}

/* Observable debugging (stderr); returns first argument unchanged. Optional label via second arg. */
RizValue native_debug(RizValue* args, int argc) {
    if (argc < 1) { riz_runtime_error("debug() requires at least 1 argument"); return riz_none(); }
    int line = (g_interp && g_interp->current_line > 0) ? g_interp->current_line : 0;
    fprintf(stderr, COL_DIM "[debug line %d]" COL_RESET " ", line);
    if (argc >= 2) {
        char* lab = riz_value_to_string(args[1]);
        fprintf(stderr, "%s: ", lab);
        free(lab);
    }
    char* s = riz_value_to_string(args[0]);
    fprintf(stderr, "%s\n", s);
    free(s);
    return riz_value_copy(args[0]);
}

/* Fatal error with optional message (any value); prints call stack then exit(1). */
RizValue native_panic(RizValue* args, int argc) {
    char* msg = argc >= 1 ? riz_value_to_string(args[0]) : riz_strdup("panic");
    fprintf(stderr, COL_RED COL_BOLD "panic:" COL_RESET " %s\n", msg);
    free(msg);
    if (g_interp) {
        if (g_interp->call_stack_len > 0) {
            fprintf(stderr, COL_DIM "Call stack (innermost last):\n" COL_RESET);
            for (int i = 0; i < g_interp->call_stack_len; i++)
                fprintf(stderr, COL_DIM "  at %s\n" COL_RESET, g_interp->call_stack[i]);
        }
        g_interp->had_error = true;
        longjmp(g_interp->panic_jmp, 1);
    }
    exit(1);
    return riz_none();
}

RizValue native_exit(RizValue* args, int argc) {
    if (g_interp) {
        g_interp->had_error = true;
        longjmp(g_interp->panic_jmp, 1);
    }
    int code = 0; if (argc == 1 && args[0].type == VAL_INT) code = (int)args[0].as.integer;
    exit(code);
    return riz_none();
}

/* Phase 4: File I/O */
RizValue native_read_file(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) { riz_runtime_error("read_file requires 1 string argument (path)"); return riz_none(); }
    char* string = read_file_alloc(args[0].as.string);
    if (!string) return riz_none();
    return riz_string_take(string);
}

RizValue native_write_file(RizValue* args, int argc) {
    if (argc != 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) { riz_runtime_error("write_file requires (path: str, content: str)"); return riz_none(); }
    size_t len = strlen(args[1].as.string);
    return riz_bool(write_file_bytes(args[0].as.string, args[1].as.string, len));
}

RizValue native_read_lines(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("read_lines(path) requires 1 string argument");
        return riz_none();
    }
    char* text = read_file_alloc(args[0].as.string);
    if (!text) return riz_none();
    RizValue result = list_from_lines(text);
    free(text);
    return result;
}

RizValue native_write_lines(RizValue* args, int argc) {
    RizStrBuf sb;
    sb_init(&sb);
    if (argc != 2 || args[0].type != VAL_STRING || args[1].type != VAL_LIST) {
        riz_runtime_error("write_lines(path, lines) requires (string, list)");
        return riz_none();
    }
    for (int i = 0; i < args[1].as.list->count; i++) {
        char* line = riz_value_to_string(args[1].as.list->items[i]);
        if (i > 0) sb_append_char(&sb, '\n');
        sb_append_cstr(&sb, line);
        free(line);
    }
    char* text = sb_take(&sb);
    if (!text) {
        riz_runtime_error("write_lines(): out of memory");
        return riz_none();
    }
    RizValue ok = riz_bool(write_file_bytes(args[0].as.string, text, strlen(text)));
    free(text);
    return ok;
}

RizValue native_file_exists(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("file_exists(path) requires 1 string argument");
        return riz_none();
    }
    return riz_bool(fs_is_file(args[0].as.string));
}

RizValue native_dir_exists(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("dir_exists(path) requires 1 string argument");
        return riz_none();
    }
    return riz_bool(fs_is_dir(args[0].as.string));
}

RizValue native_cwd(RizValue* args, int argc) {
    char buf[1024];
    (void)args;
    if (argc != 0) {
        riz_runtime_error("cwd() takes no arguments");
        return riz_none();
    }
#ifdef _WIN32
    if (!GetCurrentDirectoryA((DWORD)sizeof(buf), buf)) return riz_none();
#else
    if (!getcwd(buf, sizeof(buf))) return riz_none();
#endif
    return riz_string(buf);
}

RizValue native_getenv_fn(RizValue* args, int argc) {
    if (argc < 1 || argc > 2 || args[0].type != VAL_STRING) {
        riz_runtime_error("getenv(name[, default]) requires a string name");
        return riz_none();
    }
    const char* value = getenv(args[0].as.string);
    if (!value) {
        if (argc == 2) return riz_value_copy(args[1]);
        return riz_none();
    }
    return riz_string(value);
}

RizValue native_basename(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("basename(path) requires 1 string argument");
        return riz_none();
    }
    char* out = basename_dup(args[0].as.string);
    return riz_string_take(out);
}

RizValue native_dirname(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("dirname(path) requires 1 string argument");
        return riz_none();
    }
    char* out = dirname_dup(args[0].as.string);
    return riz_string_take(out);
}

RizValue native_join_path(RizValue* args, int argc) {
    RizStrBuf sb;
    sb_init(&sb);
    if (argc < 1) {
        riz_runtime_error("join_path() requires at least 1 string argument");
        return riz_none();
    }
    for (int i = 0; i < argc; i++) {
        const char* part;
        size_t start = 0;
        if (args[i].type != VAL_STRING) {
            riz_runtime_error("join_path() arguments must all be strings");
            free(sb.data);
            return riz_none();
        }
        part = args[i].as.string;
        if (!part[0]) continue;
        if (path_is_absolute(part)) {
            sb.len = 0;
            if (sb.data) sb.data[0] = '\0';
            sb_append_cstr(&sb, part);
            continue;
        }
        while (part[start] && path_is_sep(part[start])) start++;
        if (sb.len > 0 && !path_is_sep(sb.data[sb.len - 1])) sb_append_char(&sb, RIZ_PATH_SEP);
        sb_append_cstr(&sb, part + start);
    }
    return riz_string_take(sb_take(&sb));
}

RizValue native_json_parse(RizValue* args, int argc) {
    RizJsonParser jp;
    RizValue out = riz_none();
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("json_parse(text) requires 1 string argument");
        return riz_none();
    }
    jp.cur = args[0].as.string;
    jp.err = NULL;
    if (!json_parse_value(&jp, &out)) {
        riz_runtime_error("json_parse(): %s", jp.err ? jp.err : "invalid JSON");
        return riz_none();
    }
    jp.cur = json_skip_ws(jp.cur);
    if (*jp.cur != '\0') {
        riz_value_free(&out);
        riz_runtime_error("json_parse(): trailing characters");
        return riz_none();
    }
    return out;
}

RizValue native_json_stringify(RizValue* args, int argc) {
    RizStrBuf sb;
    bool pretty;
    if (argc < 1 || argc > 2) {
        riz_runtime_error("json_stringify(value[, pretty]) expected");
        return riz_none();
    }
    pretty = argc == 2 && riz_value_is_truthy(args[1]);
    sb_init(&sb);
    if (!json_stringify_value(&sb, args[0], pretty, 0)) {
        free(sb.data);
        riz_runtime_error("json_stringify(): unsupported value type");
        return riz_none();
    }
    return riz_string_take(sb_take(&sb));
}

RizValue native_read_json(RizValue* args, int argc) {
    RizValue text;
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("read_json(path) requires 1 string argument");
        return riz_none();
    }
    text = native_read_file(args, argc);
    if (text.type == VAL_NONE) return text;
    RizValue parsed = native_json_parse(&text, 1);
    riz_value_free(&text);
    return parsed;
}

RizValue native_write_json(RizValue* args, int argc) {
    RizValue stringify_args[2];
    RizValue json_text;
    if (argc < 2 || argc > 3 || args[0].type != VAL_STRING) {
        riz_runtime_error("write_json(path, value[, pretty]) expected");
        return riz_none();
    }
    stringify_args[0] = args[1];
    if (argc == 3) stringify_args[1] = args[2];
    json_text = native_json_stringify(stringify_args, argc == 3 ? 2 : 1);
    if (json_text.type != VAL_STRING) return json_text;
    RizValue write_args[2];
    write_args[0] = args[0];
    write_args[1] = json_text;
    RizValue ok = native_write_file(write_args, 2);
    riz_value_free(&json_text);
    return ok;
}

static bool token_is_option_like(const char* token) {
    if (!token || token[0] != '-' || token[1] == '\0') return false;
    if (isdigit((unsigned char)token[1]) || token[1] == '.') return false;
    return true;
}

static RizValue parse_delimited_builtin(RizValue* args, int argc, char delim, const char* name) {
    RizValue rows = riz_none();
    bool header = true;
    const char* err = NULL;
    if (argc < 1 || argc > 2 || args[0].type != VAL_STRING) {
        riz_runtime_error("%s(text[, header]) expected", name);
        return riz_none();
    }
    if (argc == 2) header = riz_value_is_truthy(args[1]);
    if (!parse_delimited_rows(args[0].as.string, delim, &rows, &err)) {
        riz_runtime_error("%s(): %s", name, err ? err : "parse failed");
        return riz_none();
    }
    if (!header) return rows;
    RizValue records = rows_to_records(rows);
    riz_value_free(&rows);
    return records;
}

static RizValue read_delimited_builtin(RizValue* args, int argc, char delim, const char* name) {
    RizValue text;
    if (argc < 1 || argc > 2 || args[0].type != VAL_STRING) {
        riz_runtime_error("%s(path[, header]) expected", name);
        return riz_none();
    }
    text = native_read_file(args, 1);
    if (text.type != VAL_STRING) return text;
    RizValue parse_args[2];
    parse_args[0] = text;
    if (argc == 2) parse_args[1] = args[1];
    RizValue out = parse_delimited_builtin(parse_args, argc, delim, name);
    riz_value_free(&text);
    return out;
}

RizValue native_argv_fn(RizValue* args, int argc) {
    (void)args;
    if (argc != 0) {
        riz_runtime_error("argv() takes no arguments");
        return riz_none();
    }
    return cli_argv_copy();
}

RizValue native_argc_fn(RizValue* args, int argc) {
    (void)args;
    if (argc != 0) {
        riz_runtime_error("argc() takes no arguments");
        return riz_none();
    }
    if (g_cli_argv.type == VAL_LIST) return riz_int(g_cli_argv.as.list->count);
    return riz_int(0);
}

RizValue native_script_path_fn(RizValue* args, int argc) {
    (void)args;
    if (argc != 0) {
        riz_runtime_error("script_path() takes no arguments");
        return riz_none();
    }
    if (!g_cli_script_path) return riz_none();
    return riz_string(g_cli_script_path);
}

RizValue native_parse_flags(RizValue* args, int argc) {
    RizValue raw = riz_none();
    RizValue out = riz_dict_new();
    RizValue flags = riz_dict_new();
    RizValue positionals = riz_list_new();
    bool ok = true;
    if (argc > 1 || (argc == 1 && args[0].type != VAL_LIST)) {
        riz_runtime_error("parse_flags([args]) expects an optional list of strings");
        riz_value_free(&out);
        riz_value_free(&flags);
        riz_value_free(&positionals);
        return riz_none();
    }
    raw = argc == 1 ? riz_value_copy(args[0]) : cli_argv_copy();
    for (int i = 0; ok && i < raw.as.list->count; i++) {
        RizValue item = raw.as.list->items[i];
        if (item.type != VAL_STRING) {
            riz_runtime_error("parse_flags(): every arg must be a string");
            ok = false;
            break;
        }
        const char* token = item.as.string;
        if (strcmp(token, "--") == 0) {
            for (int j = i + 1; j < raw.as.list->count; j++) {
                if (raw.as.list->items[j].type != VAL_STRING) {
                    riz_runtime_error("parse_flags(): every arg must be a string");
                    ok = false;
                    break;
                }
                riz_list_append(positionals.as.list, riz_value_copy(raw.as.list->items[j]));
            }
            break;
        }
        if (strncmp(token, "--", 2) == 0 && token[2] != '\0') {
            const char* name = token + 2;
            const char* eq = strchr(name, '=');
            if (strncmp(name, "no-", 3) == 0 && name[3] != '\0') {
                riz_dict_set(flags.as.dict, name + 3, riz_bool(false));
                continue;
            }
            if (eq) {
                char* key = riz_strndup(name, (size_t)(eq - name));
                if (!key) { ok = false; break; }
                riz_dict_set(flags.as.dict, key, riz_string(eq + 1));
                free(key);
                continue;
            }
            if (i + 1 < raw.as.list->count &&
                raw.as.list->items[i + 1].type == VAL_STRING &&
                !token_is_option_like(raw.as.list->items[i + 1].as.string)) {
                riz_dict_set(flags.as.dict, name, riz_value_copy(raw.as.list->items[i + 1]));
                i++;
            } else {
                riz_dict_set(flags.as.dict, name, riz_bool(true));
            }
            continue;
        }
        if (token[0] == '-' && token[1] != '\0') {
            if (token[2] == '\0') {
                char key[2] = { token[1], '\0' };
                if (i + 1 < raw.as.list->count &&
                    raw.as.list->items[i + 1].type == VAL_STRING &&
                    !token_is_option_like(raw.as.list->items[i + 1].as.string)) {
                    riz_dict_set(flags.as.dict, key, riz_value_copy(raw.as.list->items[i + 1]));
                    i++;
                } else {
                    riz_dict_set(flags.as.dict, key, riz_bool(true));
                }
                continue;
            }
            if (token[2] == '=') {
                char key[2] = { token[1], '\0' };
                riz_dict_set(flags.as.dict, key, riz_string(token + 3));
                continue;
            }
            for (int j = 1; token[j] != '\0'; j++) {
                char key[2] = { token[j], '\0' };
                riz_dict_set(flags.as.dict, key, riz_bool(true));
            }
            continue;
        }
        riz_list_append(positionals.as.list, riz_value_copy(item));
    }
    if (!ok) {
        riz_value_free(&raw);
        riz_value_free(&out);
        riz_value_free(&flags);
        riz_value_free(&positionals);
        return riz_none();
    }
    riz_dict_set(out.as.dict, "raw", raw);
    riz_dict_set(out.as.dict, "flags", flags);
    riz_dict_set(out.as.dict, "positionals", positionals);
    return out;
}

RizValue native_parse_csv(RizValue* args, int argc) {
    return parse_delimited_builtin(args, argc, ',', "parse_csv");
}

RizValue native_read_csv(RizValue* args, int argc) {
    return read_delimited_builtin(args, argc, ',', "read_csv");
}

RizValue native_parse_tsv(RizValue* args, int argc) {
    return parse_delimited_builtin(args, argc, '\t', "parse_tsv");
}

RizValue native_read_tsv(RizValue* args, int argc) {
    return read_delimited_builtin(args, argc, '\t', "read_tsv");
}

RizValue native_list_dir(RizValue* args, int argc) {
    const char* path = ".";
    RizPathList entries;
    const char* err = NULL;
    path_list_init(&entries);
    if (argc > 1 || (argc == 1 && args[0].type != VAL_STRING)) {
        riz_runtime_error("list_dir([path]) expects 0 or 1 string argument");
        return riz_none();
    }
    if (argc == 1) path = args[0].as.string;
    if (!fs_is_dir(path)) return riz_none();
    if (!fs_list_dir_names(path, &entries, &err)) {
        riz_runtime_error("list_dir(): %s", err ? err : "could not read directory");
        path_list_free(&entries);
        return riz_none();
    }
    return path_list_to_riz_list(&entries);
}

RizValue native_walk_dir(RizValue* args, int argc) {
    const char* path = ".";
    RizPathList entries;
    const char* err = NULL;
    path_list_init(&entries);
    if (argc > 1 || (argc == 1 && args[0].type != VAL_STRING)) {
        riz_runtime_error("walk_dir([path]) expects 0 or 1 string argument");
        return riz_none();
    }
    if (argc == 1) path = args[0].as.string;
    if (!fs_is_dir(path)) return riz_none();
    if (!walk_dir_recursive(path, path, &entries, &err)) {
        riz_runtime_error("walk_dir(): %s", err ? err : "walk failed");
        path_list_free(&entries);
        return riz_none();
    }
    return path_list_to_riz_list(&entries);
}

RizValue native_glob(RizValue* args, int argc) {
    char* root = NULL;
    RizPathList segments;
    RizPathList matches;
    const char* err = NULL;
    path_list_init(&segments);
    path_list_init(&matches);
    if (argc != 1 || args[0].type != VAL_STRING) {
        riz_runtime_error("glob(pattern) requires 1 string argument");
        return riz_none();
    }
    if (!split_path_segments(args[0].as.string, &root, &segments, &err)) {
        riz_runtime_error("glob(): %s", err ? err : "invalid pattern");
        path_list_free(&segments);
        path_list_free(&matches);
        free(root);
        return riz_none();
    }
    if (!glob_recursive(root ? root : ".", root ? root : ".", &segments, 0, &matches, &err)) {
        riz_runtime_error("glob(): %s", err ? err : "glob failed");
        path_list_free(&segments);
        path_list_free(&matches);
        free(root);
        return riz_none();
    }
    path_list_sort(&matches);
    path_list_dedup(&matches);
    path_list_free(&segments);
    free(root);
    return path_list_to_riz_list(&matches);
}

RizValue native_mkdir_fn(RizValue* args, int argc) {
    bool parents = false;
    if (argc < 1 || argc > 2 || args[0].type != VAL_STRING) {
        riz_runtime_error("mkdir(path[, parents]) expects 1 or 2 arguments");
        return riz_none();
    }
    if (argc == 2) parents = riz_value_is_truthy(args[1]);
    if (args[0].as.string[0] == '\0') return riz_bool(false);
    if (fs_is_dir(args[0].as.string)) return riz_bool(true);
    return riz_bool(parents ? fs_mkdir_parents(args[0].as.string) : fs_mkdir_single(args[0].as.string));
}

RizValue native_has_key(RizValue* args, int argc) {
    if (argc!=2||args[0].type!=VAL_DICT||args[1].type!=VAL_STRING) return riz_bool(false);
    return riz_bool(riz_dict_has(args[0].as.dict, args[1].as.string));
}

/* Exported for AOT (aot_runtime.c links interpreter.c). */
RizValue native_clamp(RizValue* args, int argc) {
    if (argc != 3) { riz_runtime_error("clamp(value, lo, hi) takes 3 arguments"); return riz_none(); }
    double v, lo, hi;
    if (args[0].type == VAL_INT) v = (double)args[0].as.integer;
    else if (args[0].type == VAL_FLOAT) v = args[0].as.floating;
    else { riz_runtime_error("clamp(): value must be int or float"); return riz_none(); }
    if (args[1].type == VAL_INT) lo = (double)args[1].as.integer;
    else if (args[1].type == VAL_FLOAT) lo = args[1].as.floating;
    else { riz_runtime_error("clamp(): lo must be int or float"); return riz_none(); }
    if (args[2].type == VAL_INT) hi = (double)args[2].as.integer;
    else if (args[2].type == VAL_FLOAT) hi = args[2].as.floating;
    else { riz_runtime_error("clamp(): hi must be int or float"); return riz_none(); }
    if (lo > hi) { double t = lo; lo = hi; hi = t; }
    double c = v < lo ? lo : (v > hi ? hi : v);
    bool all_int = (args[0].type == VAL_INT && args[1].type == VAL_INT && args[2].type == VAL_INT);
    if (all_int && c == floor(c)) return riz_int((int64_t)c);
    return riz_float(c);
}

RizValue native_sign(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("sign(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) {
        int64_t x = args[0].as.integer;
        return riz_int(x > 0 ? 1 : (x < 0 ? -1 : 0));
    }
    if (args[0].type == VAL_FLOAT) {
        double x = args[0].as.floating;
        return riz_int(x > 0.0 ? 1 : (x < 0.0 ? -1 : 0));
    }
    riz_runtime_error("sign(): expected int or float"); return riz_none();
}

RizValue native_floor_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("floor(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(floor(args[0].as.floating));
    riz_runtime_error("floor(): expected int or float"); return riz_none();
}

RizValue native_ceil_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("ceil(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(ceil(args[0].as.floating));
    riz_runtime_error("ceil(): expected int or float"); return riz_none();
}

RizValue native_round_fn(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("round(x) takes 1 argument"); return riz_none(); }
    if (args[0].type == VAL_INT) return args[0];
    if (args[0].type == VAL_FLOAT) return riz_float(round(args[0].as.floating));
    riz_runtime_error("round(): expected int or float"); return riz_none();
}

RizValue native_all(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_LIST) { riz_runtime_error("all(list) takes 1 list"); return riz_none(); }
    RizList* L = args[0].as.list;
    for (int i = 0; i < L->count; i++)
        if (!riz_value_is_truthy(L->items[i])) return riz_bool(false);
    return riz_bool(true);
}

RizValue native_any(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_LIST) { riz_runtime_error("any(list) takes 1 list"); return riz_none(); }
    RizList* L = args[0].as.list;
    for (int i = 0; i < L->count; i++)
        if (riz_value_is_truthy(L->items[i])) return riz_bool(true);
    return riz_bool(false);
}

RizValue native_as_bool(RizValue* args, int argc) {
    if (argc != 1) { riz_runtime_error("bool(x) takes 1 argument"); return riz_none(); }
    return riz_bool(riz_value_is_truthy(args[0]));
}

RizValue native_ord(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_STRING) { riz_runtime_error("ord(s) takes 1 string"); return riz_none(); }
    const char* s = args[0].as.string;
    size_t n = strlen(s);
    if (n != 1) { riz_runtime_error("ord() expects a single-character string"); return riz_none(); }
    return riz_int((int64_t)(unsigned char)s[0]);
}

RizValue native_chr(RizValue* args, int argc) {
    if (argc != 1 || args[0].type != VAL_INT) { riz_runtime_error("chr(n) takes 1 int"); return riz_none(); }
    int64_t n = args[0].as.integer;
    if (n < 0 || n > 255) { riz_runtime_error("chr(): n must be in 0..255"); return riz_none(); }
    char buf[2] = { (char)n, '\0' };
    return riz_string(buf);
}

RizValue native_extend(RizValue* args, int argc) {
    if (argc != 2 || args[0].type != VAL_LIST || args[1].type != VAL_LIST) {
        riz_runtime_error("extend(list, other) requires two lists"); return riz_none();
    }
    RizList* a = args[0].as.list;
    RizList* b = args[1].as.list;
    for (int i = 0; i < b->count; i++)
        riz_list_append(a, riz_value_copy(b->items[i]));
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Method dispatch for built-in types
 * ═══════════════════════════════════════════════════════ */

RizValue string_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    (void)I;
    const char* s = obj.as.string;
    size_t slen = strlen(s);

    if (strcmp(method,"upper")==0 && argc==0) {
        char* r=riz_strdup(s); for(size_t i=0;i<slen;i++) r[i]=(char)toupper((unsigned char)r[i]);
        return riz_string_take(r);
    }
    if (strcmp(method,"lower")==0 && argc==0) {
        char* r=riz_strdup(s); for(size_t i=0;i<slen;i++) r[i]=(char)tolower((unsigned char)r[i]);
        return riz_string_take(r);
    }
    if (strcmp(method,"trim")==0 && argc==0) {
        while(slen>0 && isspace((unsigned char)s[0])) { s++; slen--; }
        while(slen>0 && isspace((unsigned char)s[slen-1])) slen--;
        char* r=(char*)malloc(slen+1); memcpy(r,s,slen); r[slen]='\0';
        return riz_string_take(r);
    }
    if (strcmp(method,"split")==0) {
        const char* sep = (argc>=1 && args[0].type==VAL_STRING) ? args[0].as.string : " ";
        size_t seplen = strlen(sep);
        RizValue result = riz_list_new();
        const char* p = s;
        if (seplen==0) {
            /* split into chars */
            for(size_t i=0;i<slen;i++) { char c[2]={s[i],'\0'}; riz_list_append(result.as.list,riz_string(c)); }
        } else {
            while(1) {
                const char* found = strstr(p, sep);
                if (!found) { riz_list_append(result.as.list, riz_string(p)); break; }
                size_t part_len = found - p;
                char* part=(char*)malloc(part_len+1); memcpy(part,p,part_len); part[part_len]='\0';
                riz_list_append(result.as.list, riz_string_take(part));
                p = found + seplen;
            }
        }
        return result;
    }
    if (strcmp(method,"contains")==0 && argc==1 && args[0].type==VAL_STRING) {
        return riz_bool(strstr(s, args[0].as.string) != NULL);
    }
    if (strcmp(method,"starts_with")==0 && argc==1 && args[0].type==VAL_STRING) {
        return riz_bool(strncmp(s, args[0].as.string, strlen(args[0].as.string)) == 0);
    }
    if (strcmp(method,"ends_with")==0 && argc==1 && args[0].type==VAL_STRING) {
        size_t sl=strlen(args[0].as.string);
        return riz_bool(slen>=sl && strcmp(s+slen-sl, args[0].as.string)==0);
    }
    if (strcmp(method,"replace")==0 && argc==2 && args[0].type==VAL_STRING && args[1].type==VAL_STRING) {
        const char* old=args[0].as.string; const char* new_s=args[1].as.string;
        size_t old_len=strlen(old), new_len=strlen(new_s);
        if (old_len==0) return riz_string(s);
        /* Count occurrences */
        int count=0; const char* p=s;
        while((p=strstr(p,old))!=NULL){count++;p+=old_len;}
        size_t rlen = slen + count*(new_len - old_len);
        char* r=(char*)malloc(rlen+1); char* dst=r; p=s;
        while(1) {
            const char* found=strstr(p,old);
            if(!found){strcpy(dst,p);break;}
            memcpy(dst,p,found-p);dst+=found-p;
            memcpy(dst,new_s,new_len);dst+=new_len;
            p=found+old_len;
        }
        return riz_string_take(r);
    }
    if (strcmp(method,"chars")==0 && argc==0) {
        RizValue result = riz_list_new();
        for(size_t i=0;i<slen;i++){char c[2]={s[i],'\0'}; riz_list_append(result.as.list,riz_string(c));}
        return result;
    }
    if (strcmp(method,"repeat")==0 && argc==1 && args[0].type==VAL_INT) {
        int64_t n=args[0].as.integer; if(n<=0)return riz_string("");
        char* r=(char*)malloc(slen*n+1);
        for(int64_t i=0;i<n;i++) memcpy(r+i*slen,s,slen);
        r[slen*n]='\0'; return riz_string_take(r);
    }
    if (strcmp(method,"find")==0 && argc==1 && args[0].type==VAL_STRING) {
        const char* found = strstr(s, args[0].as.string);
        return riz_int(found ? (int64_t)(found - s) : -1);
    }
    riz_runtime_error("str has no method '%s'", method);
    return riz_none();
}

RizValue list_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    RizList* list = obj.as.list;

    if (strcmp(method,"push")==0 && argc==1) {
        riz_list_append(list, riz_value_copy(args[0])); return riz_none();
    }
    if (strcmp(method,"pop")==0 && argc==0) {
        if(list->count==0){riz_runtime_error("pop() from empty list");return riz_none();}
        return list->items[--list->count];
    }
    if (strcmp(method,"sort")==0 && argc==0) {
        for(int i=1;i<list->count;i++){
            RizValue key=list->items[i];int j=i-1;
            double kv=key.type==VAL_INT?(double)key.as.integer:key.as.floating;
            while(j>=0){double jv=list->items[j].type==VAL_INT?(double)list->items[j].as.integer:list->items[j].as.floating;if(jv<=kv)break;list->items[j+1]=list->items[j];j--;}
            list->items[j+1]=key;
        }
        return riz_none();
    }
    if (strcmp(method,"reverse")==0 && argc==0) {
        for(int i=0,j=list->count-1;i<j;i++,j--){RizValue t=list->items[i];list->items[i]=list->items[j];list->items[j]=t;}
        return riz_none();
    }
    if (strcmp(method,"join")==0) {
        const char* sep=(argc>=1&&args[0].type==VAL_STRING)?args[0].as.string:"";
        size_t cap=128; char* r=(char*)malloc(cap); size_t len=0; size_t sl=strlen(sep);
        for(int i=0;i<list->count;i++){
            if(i>0){while(len+sl+1>=cap){cap*=2;r=(char*)realloc(r,cap);}memcpy(r+len,sep,sl);len+=sl;}
            char* s=riz_value_to_string(list->items[i]);size_t il=strlen(s);
            while(len+il+1>=cap){cap*=2;r=(char*)realloc(r,cap);}
            memcpy(r+len,s,il);len+=il;free(s);
        }
        r[len]='\0'; return riz_string_take(r);
    }
    if (strcmp(method,"contains")==0 && argc==1) {
        for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) return riz_bool(true);
        return riz_bool(false);
    }
    if (strcmp(method,"index")==0 && argc==1) {
        for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) return riz_int(i);
        return riz_int(-1);
    }
    if (strcmp(method,"count")==0 && argc==1) {
        int n=0; for(int i=0;i<list->count;i++) if(riz_value_equal(list->items[i],args[0])) n++;
        return riz_int(n);
    }
    if (strcmp(method,"slice")==0) {
        int start=(argc>=1&&args[0].type==VAL_INT)?(int)args[0].as.integer:0;
        int end=(argc>=2&&args[1].type==VAL_INT)?(int)args[1].as.integer:list->count;
        if(start<0)start+=list->count; if(end<0)end+=list->count;
        if(start<0)start=0; if(end>list->count)end=list->count;
        RizValue result = riz_list_new();
        for(int i=start;i<end;i++) riz_list_append(result.as.list, riz_value_copy(list->items[i]));
        return result;
    }
    if (strcmp(method,"map")==0 && argc==1) {
        RizValue fn_val=args[0]; RizValue result=riz_list_new();
        for(int i=0;i<list->count;i++){
            RizValue item=list->items[i]; RizValue mapped;
            if(fn_val.type==VAL_FUNCTION) mapped=call_function(I,fn_val.as.function,&item,1);
            else if(fn_val.type==VAL_NATIVE_FN) mapped=fn_val.as.native->fn(&item,1);
            else {riz_runtime_error("map() arg must be callable");return riz_none();}
            riz_list_append(result.as.list,mapped);
        }
        return result;
    }
    if (strcmp(method,"filter")==0 && argc==1) {
        RizValue fn_val=args[0]; RizValue result=riz_list_new();
        for(int i=0;i<list->count;i++){
            RizValue item=list->items[i]; RizValue keep;
            if(fn_val.type==VAL_FUNCTION) keep=call_function(I,fn_val.as.function,&item,1);
            else if(fn_val.type==VAL_NATIVE_FN) keep=fn_val.as.native->fn(&item,1);
            else {riz_runtime_error("filter() arg must be callable");return riz_none();}
            if(riz_value_is_truthy(keep)) riz_list_append(result.as.list, riz_value_copy(item));
        }
        return result;
    }
    riz_runtime_error("list has no method '%s'", method);
    return riz_none();
}

RizValue dict_method(Interpreter* I, RizValue obj, const char* method, RizValue* args, int argc) {
    (void)I;
    RizDict* d = obj.as.dict;

    if (strcmp(method,"keys")==0 && argc==0)   return riz_dict_keys(d);
    if (strcmp(method,"values")==0 && argc==0) return riz_dict_values(d);
    if (strcmp(method,"has")==0 && argc==1 && args[0].type==VAL_STRING) return riz_bool(riz_dict_has(d,args[0].as.string));
    if (strcmp(method,"get")==0 && argc>=1 && args[0].type==VAL_STRING) {
        if(riz_dict_has(d,args[0].as.string)) return riz_dict_get(d,args[0].as.string);
        return argc>=2 ? riz_value_copy(args[1]) : riz_none();
    }
    if (strcmp(method,"set")==0 && argc==2 && args[0].type==VAL_STRING) {
        riz_dict_set(d,args[0].as.string,riz_value_copy(args[1])); return riz_none();
    }
    if (strcmp(method,"delete")==0 && argc==1 && args[0].type==VAL_STRING) {
        riz_dict_delete(d,args[0].as.string); return riz_none();
    }
    if (strcmp(method,"items")==0 && argc==0) {
        RizValue result = riz_list_new();
        for(int i=0;i<d->count;i++){
            RizValue pair=riz_list_new();
            riz_list_append(pair.as.list,riz_string(d->keys[i]));
            riz_list_append(pair.as.list,riz_value_copy(d->values[i]));
            riz_list_append(result.as.list,pair);
        }
        return result;
    }
    if (strcmp(method,"merge")==0 && argc==1 && args[0].type==VAL_DICT) {
        RizDict* other = args[0].as.dict;
        for(int i=0;i<other->count;i++) riz_dict_set(d,other->keys[i],riz_value_copy(other->values[i]));
        return riz_none();
    }
    /* Namespace dicts: callable values under string keys (e.g. py.exec on global `py`) */
    if (riz_dict_has(d, method)) {
        RizValue slot = riz_dict_get(d, method);
        if (slot.type == VAL_NATIVE_FN) {
            NativeFnObj* n = slot.as.native;
            if (n->arity >= 0 && argc != n->arity) {
                riz_runtime_error("%s() takes %d arg(s), %d given", n->name, n->arity, argc);
                return riz_none();
            }
            return n->fn(args, argc);
        }
        if (slot.type == VAL_FUNCTION)
            return call_function(I, slot.as.function, args, argc);
    }
    riz_runtime_error("dict has no method '%s'", method);
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Method call detection and dispatch
 * ═══════════════════════════════════════════════════════ */

static RizValue eval_method_call(Interpreter* I, ASTNode* node) {
    ASTNode* callee_node = node->as.call.callee;
    RizValue obj = eval(I, callee_node->as.member.object);
    const char* method = callee_node->as.member.member;

    int argc = node->as.call.arg_count;
    RizValue* args = NULL;
    if (argc > 0) {
        args = RIZ_ALLOC_ARRAY(RizValue, argc);
        for (int i = 0; i < argc; i++) args[i] = eval(I, node->as.call.args[i]);
    }

    RizValue result;
    if (obj.type == VAL_STRING)     result = string_method(I, obj, method, args, argc);
    else if (obj.type == VAL_LIST)  result = list_method(I, obj, method, args, argc);
    else if (obj.type == VAL_DICT)  result = dict_method(I, obj, method, args, argc);
    else if (obj.type == VAL_INSTANCE) {
        /* Instance method: look up method in struct definition */
        RizInstance* inst = obj.as.instance;
        RizStructDef* def = inst->def;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->method_names[i], method) == 0) {
                RizFunction* fn = def->method_values[i].as.function;
                /* Prepend self */
                int full_argc = argc + 1;
                RizValue* full_args = RIZ_ALLOC_ARRAY(RizValue, full_argc);
                full_args[0] = obj;
                for (int j = 0; j < argc; j++) full_args[j+1] = args[j];
                result = call_function(I, fn, full_args, full_argc);
                free(full_args);
                free(args);
                return result;
            }
        }
        riz_runtime_error("'%s' has no method '%s'", def->name, method);
        result = riz_none();
    } else if (obj.type == VAL_STRUCT_DEF) {
        /* Static method: StructName.method() */
        RizStructDef* def = obj.as.struct_def;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->method_names[i], method) == 0) {
                RizFunction* fn = def->method_values[i].as.function;
                result = call_function(I, fn, args, argc);
                free(args);
                return result;
            }
        }
        riz_runtime_error("'%s' has no static method '%s'", def->name, method);
        result = riz_none();
    }
    else { riz_runtime_error("'%s' has no method '%s'", riz_value_type_name(obj), method); result = riz_none(); }

    free(args);
    return result;
}

/* ═══════════════════════════════════════════════════════
 *  Register built-ins
 * ═══════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════
 *  Phase 5: FFI Plugin Loader
 * ═══════════════════════════════════════════════════════ */

/* FFI API callbacks — these are passed to plugins so they can register functions */
static void RIZ_API_CALL ffi_register_fn(void* interp_ptr, const char* name, RizPluginFn fn, int arity) {
    if (!interp_ptr) return;
    Interpreter* I = (Interpreter*)interp_ptr;
    env_define(I->globals, name, riz_native(name, (NativeFnPtr)fn, arity), false);
}

static void RIZ_API_CALL ffi_register_fn_vm(void* interp_ptr, const char* name, RizPluginFn fn, int arity) {
    if (!interp_ptr) return;
    RizVM* vm = (RizVM*)interp_ptr;
    env_define(vm->globals, name, riz_native(name, (NativeFnPtr)fn, arity), false);
}

static RizValue RIZ_API_CALL ffi_make_dict(void) {
    return riz_dict_new();
}

static void RIZ_API_CALL ffi_dict_set_fn(RizValue dict, const char* key, const char* riz_name, RizPluginFn fn, int arity) {
    if (dict.type != VAL_DICT) return;
    riz_dict_set(dict.as.dict, key, riz_native(riz_name, (NativeFnPtr)fn, arity));
}

static void RIZ_API_CALL ffi_define_global(void* interp_ptr, const char* name, RizValue value) {
    if (!interp_ptr) return;
    Interpreter* I = (Interpreter*)interp_ptr;
    env_define(I->globals, name, value, false);
}

static void RIZ_API_CALL ffi_define_global_vm(void* interp_ptr, const char* name, RizValue value) {
    if (!interp_ptr) return;
    RizVM* vm = (RizVM*)interp_ptr;
    env_define(vm->globals, name, value, false);
}

static RizValue RIZ_API_CALL ffi_make_int(int64_t v)      { return riz_int(v); }
static RizValue RIZ_API_CALL ffi_make_float(double v)     { return riz_float(v); }
static RizValue RIZ_API_CALL ffi_make_bool(bool v)        { return riz_bool(v); }
static RizValue RIZ_API_CALL ffi_make_string(const char*v){ return riz_string(v); }
static void RIZ_API_CALL ffi_panic(void* interp, const char* msg) {
    if (!interp) return;
    Interpreter* I = (Interpreter*)interp;
    fprintf(stderr, "\n\033[1;31m[Riz AI Panic]\033[0m %s\n", msg);
    fprintf(stderr, "  --> interpreter mode, line: %d\n", I->current_line);
    I->had_error = true;
    longjmp(I->panic_jmp, 1);
}

static void RIZ_API_CALL ffi_panic_vm(void* interp, const char* msg) {
    if (!interp) return;
    RizVM* vm = (RizVM*)interp;
    fprintf(stderr, "\n\033[1;31m[Riz VM plugin panic]\033[0m %s\n", msg);
    vm->had_error = true;
    longjmp(vm->panic_jmp, 1);
}

static RizValue RIZ_API_CALL ffi_make_none(void)          { return riz_none(); }
static RizValue RIZ_API_CALL ffi_make_list(void)          { return riz_list_new(); }
static RizValue RIZ_API_CALL ffi_make_native_ptr(void *p, const char *tag, void (*dtor)(void *)) {
    return riz_native_ptr(p, tag, dtor);
}
static void* RIZ_API_CALL ffi_get_native_ptr(RizValue v) {
    if (v.type == VAL_NATIVE_PTR && v.as.native_ptr) return v.as.native_ptr->ptr;
    return NULL;
}
static void RIZ_API_CALL ffi_list_append(RizValue lst, RizValue v) { if(lst.type == VAL_LIST) riz_list_append(lst.as.list, v); }
static int RIZ_API_CALL ffi_list_len(RizValue v) { return v.type == VAL_LIST ? v.as.list->count : 0; }
static RizValue RIZ_API_CALL ffi_list_get(RizValue v, int index) {
    if (v.type != VAL_LIST || index < 0 || index >= v.as.list->count) return riz_none();
    return v.as.list->items[index];
}

static int RIZ_API_CALL ffi_get_current_line(void* interp) {
    return interp ? ((Interpreter*)interp)->current_line : 0;
}

static int RIZ_API_CALL ffi_get_current_line_vm(void* interp) {
    (void)interp;
    return 0;
}


void riz_populate_plugin_api(void* I, RizPluginAPI* api) {
    if (!api) return;
    memset(api, 0, sizeof(RizPluginAPI));
    api->size = sizeof(RizPluginAPI);
    api->register_fn = ffi_register_fn;
    api->make_int = ffi_make_int;
    api->make_float = ffi_make_float;
    api->make_bool = ffi_make_bool;
    api->make_string = ffi_make_string;
    api->make_none = ffi_make_none;
    api->make_list = ffi_make_list;
    api->make_native_ptr = ffi_make_native_ptr;
    api->get_native_ptr = ffi_get_native_ptr;
    api->list_append = ffi_list_append;
    api->list_length = ffi_list_len;
    api->list_get = ffi_list_get;
    api->interp = I;
    api->get_current_line = ffi_get_current_line;
    api->panic = ffi_panic;
    api->make_dict = ffi_make_dict;
    api->dict_set_fn = ffi_dict_set_fn;
    api->define_global = ffi_define_global;
}

bool riz_plugin_load_vm(Environment* env, RizVM* vm, const char* path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s' (error %lu)", path, GetLastError());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)GetProcAddress(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        FreeLibrary(lib);
        return false;
    }
#else
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s': %s", path, dlerror());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)dlsym(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        dlclose(lib);
        return false;
    }
#endif
    RizPluginAPI api = {0};
    api.register_fn = ffi_register_fn_vm;
    api.make_int = ffi_make_int;
    api.make_float = ffi_make_float;
    api.make_bool = ffi_make_bool;
    api.make_string = ffi_make_string;
    api.make_none = ffi_make_none;
    api.make_list = ffi_make_list;
    api.make_native_ptr = ffi_make_native_ptr;
    api.get_native_ptr = ffi_get_native_ptr;
    api.list_append = ffi_list_append;
    api.list_length = ffi_list_len;
    api.list_get = ffi_list_get;
    api.interp = vm;
    api.get_current_line = ffi_get_current_line_vm;
    api.panic = ffi_panic_vm;
    api.make_dict = ffi_make_dict;
    api.dict_set_fn = ffi_dict_set_fn;
    api.define_global = ffi_define_global_vm;
    (void)env;
    init_fn(&api);

    void** np = (void**)realloc(vm->native_libs, sizeof(void*) * (size_t)(vm->native_lib_count + 1));
    if (!np) {
        riz_runtime_error("Out of memory tracking native library");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return false;
    }
    vm->native_libs = np;
    vm->native_libs[vm->native_lib_count++] = lib;
    return true;
}

static void bridge_log(const char* msg) {
    FILE* f = fopen("bridge_debug.txt", "a");
    if (f) { fprintf(f, "%s\n", msg); fflush(f); fclose(f); }
}

static bool load_native_plugin(Interpreter* I, const char* path) {
    char buf[256];
    sprintf(buf, "[Riz Bridge] Loading: %s", path);
    bridge_log(buf);
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        bridge_log("[Riz Bridge] LoadLibraryA FAILED");
        riz_runtime_error("Failed to load native library '%s' (error %lu)", path, GetLastError());
        return false;
    }
    bridge_log("[Riz Bridge] Handle OK. Finding init_fn...");
    RizPluginInitFn init_fn = (RizPluginInitFn)GetProcAddress(lib, "riz_plugin_init");
    if (!init_fn) {
        bridge_log("[Riz Bridge] init_fn NOT FOUND");
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        FreeLibrary(lib);
        return false;
    }
    bridge_log("[Riz Bridge] init_fn found. Calling...");
#else
    /* POSIX: dlopen/dlsym */
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        riz_runtime_error("Failed to load native library '%s': %s", path, dlerror());
        return false;
    }
    RizPluginInitFn init_fn = (RizPluginInitFn)dlsym(lib, "riz_plugin_init");
    if (!init_fn) {
        riz_runtime_error("Library '%s' has no 'riz_plugin_init' entry point", path);
        dlclose(lib);
        return false;
    }
#endif
    /* Build the API bridge and call the plugin's init */
    RizPluginAPI api = {0};
    api.size = sizeof(RizPluginAPI);
    api.register_fn  = ffi_register_fn;
    api.make_int        = ffi_make_int;
    api.make_float      = ffi_make_float;
    api.make_bool       = ffi_make_bool;
    api.make_string     = ffi_make_string;
    api.make_none       = ffi_make_none;
    api.make_list       = ffi_make_list;
    api.make_native_ptr = ffi_make_native_ptr;
    api.get_native_ptr  = ffi_get_native_ptr;
    api.list_append     = ffi_list_append;
    api.list_length     = ffi_list_len;
    api.list_get        = ffi_list_get;
    api.interp          = I;
    api.get_current_line = ffi_get_current_line;
    api.panic           = ffi_panic;
    api.make_dict       = ffi_make_dict;
    api.dict_set_fn     = ffi_dict_set_fn;
    api.define_global   = ffi_define_global;
    
    printf("[Riz Bridge] Calling riz_plugin_init in DLL...\n");
    init_fn(&api);
    printf("[Riz Bridge] riz_plugin_init RETURNED successfully.\n");

    /* Track the handle so we can free it later */
    I->loaded_libs = realloc(I->loaded_libs, sizeof(void*) * (I->lib_count + 1));
    I->loaded_libs[I->lib_count++] = lib;
    return true;
}

void riz_vm_seed_builtins(Environment* g) {
    /* Phase 1 */
    env_define(g, "print",    riz_native("print",    native_print,      -1), false);
    env_define(g, "len",      riz_native("len",      native_len,         1), false);
    env_define(g, "range",    riz_native("range",    native_range,      -1), false);
    env_define(g, "type",     riz_native("type",     native_type,        1), false);
    env_define(g, "str",      riz_native("str",      native_str,         1), false);
    env_define(g, "int",      riz_native("int",      native_int_cast,    1), false);
    env_define(g, "float",    riz_native("float",    native_float_cast,  1), false);
    env_define(g, "input",    riz_native("input",    native_input,      -1), false);
    env_define(g, "append",   riz_native("append",   native_append,      2), false);
    env_define(g, "pop",      riz_native("pop",      native_pop,         1), false);
    env_define(g, "abs",      riz_native("abs",      native_abs,         1), false);
    env_define(g, "min",      riz_native("min",      native_min,        -1), false);
    env_define(g, "max",      riz_native("max",      native_max,        -1), false);
    env_define(g, "sum",      riz_native("sum",      native_sum,         1), false);
    env_define(g, "parallel_sum", riz_native("parallel_sum", native_parallel_sum, -1), false);
    env_define(g, "cpu_count", riz_native("cpu_count", native_cpu_count, 0), false);
    env_define(g, "map",      riz_native("map",      native_map,         2), false);
    env_define(g, "filter",   riz_native("filter",   native_filter,      2), false);
    /* Phase 2 */
    env_define(g, "format",   riz_native("format",   native_format,     -1), false);
    env_define(g, "sorted",   riz_native("sorted",   native_sorted,      1), false);
    env_define(g, "reversed", riz_native("reversed", native_reversed,    1), false);
    env_define(g, "enumerate",riz_native("enumerate",native_enumerate,   1), false);
    env_define(g, "zip",      riz_native("zip",      native_zip,         2), false);
    env_define(g, "keys",     riz_native("keys",     native_keys,        1), false);
    env_define(g, "values",   riz_native("values",   native_values,      1), false);
    env_define(g, "assert",   riz_native("assert",   native_assert,     -1), false);
    env_define(g, "exit",     riz_native("exit",     native_exit,       -1), false);
    env_define(g, "argv",     riz_native("argv",     native_argv_fn,     0), false);
    env_define(g, "argc",     riz_native("argc",     native_argc_fn,     0), false);
    env_define(g, "script_path",riz_native("script_path", native_script_path_fn, 0), false);
    env_define(g, "parse_flags",riz_native("parse_flags", native_parse_flags, -1), false);
    env_define(g, "read_file",riz_native("read_file", native_read_file,   1), false);
    env_define(g, "write_file",riz_native("write_file", native_write_file, 2), false);
    env_define(g, "read_lines",riz_native("read_lines", native_read_lines, 1), false);
    env_define(g, "write_lines",riz_native("write_lines", native_write_lines, 2), false);
    env_define(g, "parse_csv",riz_native("parse_csv", native_parse_csv,   -1), false);
    env_define(g, "read_csv", riz_native("read_csv",  native_read_csv,    -1), false);
    env_define(g, "parse_tsv",riz_native("parse_tsv", native_parse_tsv,   -1), false);
    env_define(g, "read_tsv", riz_native("read_tsv",  native_read_tsv,    -1), false);
    env_define(g, "file_exists",riz_native("file_exists", native_file_exists, 1), false);
    env_define(g, "dir_exists",riz_native("dir_exists", native_dir_exists, 1), false);
    env_define(g, "list_dir", riz_native("list_dir",  native_list_dir,    -1), false);
    env_define(g, "walk_dir", riz_native("walk_dir",  native_walk_dir,    -1), false);
    env_define(g, "glob",     riz_native("glob",      native_glob,         1), false);
    env_define(g, "mkdir",    riz_native("mkdir",     native_mkdir_fn,    -1), false);
    env_define(g, "cwd",      riz_native("cwd",       native_cwd,         0), false);
    env_define(g, "getenv",   riz_native("getenv",    native_getenv_fn,  -1), false);
    env_define(g, "basename", riz_native("basename",  native_basename,    1), false);
    env_define(g, "dirname",  riz_native("dirname",   native_dirname,     1), false);
    env_define(g, "join_path",riz_native("join_path", native_join_path,  -1), false);
    env_define(g, "json_parse",riz_native("json_parse", native_json_parse, 1), false);
    env_define(g, "json_stringify",riz_native("json_stringify", native_json_stringify, -1), false);
    env_define(g, "read_json",riz_native("read_json", native_read_json,   1), false);
    env_define(g, "write_json",riz_native("write_json", native_write_json, -1), false);
    env_define(g, "has_key",  riz_native("has_key",  native_has_key,     2), false);
    env_define(g, "clamp",    riz_native("clamp",    native_clamp,       3), false);
    env_define(g, "sign",     riz_native("sign",     native_sign,        1), false);
    env_define(g, "floor",    riz_native("floor",    native_floor_fn,    1), false);
    env_define(g, "ceil",     riz_native("ceil",     native_ceil_fn,     1), false);
    env_define(g, "round",    riz_native("round",    native_round_fn,    1), false);
    env_define(g, "all",      riz_native("all",      native_all,         1), false);
    env_define(g, "any",      riz_native("any",      native_any,         1), false);
    env_define(g, "bool",     riz_native("bool",     native_as_bool,     1), false);
    env_define(g, "ord",      riz_native("ord",      native_ord,         1), false);
    env_define(g, "chr",      riz_native("chr",      native_chr,         1), false);
    env_define(g, "extend",   riz_native("extend",   native_extend,      2), false);
    env_define(g, "debug",    riz_native("debug",    native_debug,      -1), false);
    env_define(g, "panic",    riz_native("panic",    native_panic,      -1), false);
    env_define(g, "time",     riz_native("time",     native_time_fn,     0), false);
}

static void register_builtins(Interpreter* I) {
    riz_vm_seed_builtins(I->globals);
}

/* ═══════════════════════════════════════════════════════
 *  Interpreter lifecycle
 * ═══════════════════════════════════════════════════════ */

Interpreter* interpreter_new(void) {
    Interpreter* I = RIZ_ALLOC(Interpreter);
    I->globals = env_new(NULL);
    I->current_env = I->globals;
    I->signal = SIG_NONE;
    I->had_error = false;
    I->imported_files = NULL;
    I->import_count = 0;
    I->loaded_libs = NULL;
    I->lib_count = 0;
    I->program_ast = NULL;
    register_builtins(I);
    return I;
}

void interpreter_free(Interpreter* interp) {
    if (!interp) return;
    interpreter_clear_error_stack(interp);
    for (int i = 0; i < interp->call_stack_len; i++) free(interp->call_stack[i]);
    free(interp->call_stack);
    interp->call_stack = NULL;
    interp->call_stack_len = 0;
    interp->call_stack_cap = 0;
    for (int i = 0; i < interp->import_count; i++) free(interp->imported_files[i]);
    free(interp->imported_files);
    env_free_deep(interp->globals);
    interp->globals = NULL;
    /* Unload native plugins after releasing globals (native fn wrappers). */
    for (int i = 0; i < interp->lib_count; i++) {
#ifdef _WIN32
        FreeLibrary((HMODULE)interp->loaded_libs[i]);
#else
        dlclose(interp->loaded_libs[i]);
#endif
    }
    free(interp->loaded_libs);
    if (interp->program_ast) {
        ast_free(interp->program_ast);
        interp->program_ast = NULL;
    }
    free(interp);
}

/* ═══════════════════════════════════════════════════════
 *  Call a function value
 * ═══════════════════════════════════════════════════════ */

static void call_stack_push(Interpreter* I, const char* name) {
    if (I->call_stack_len >= I->call_stack_cap) {
        int n = I->call_stack_cap ? I->call_stack_cap * 2 : 8;
        I->call_stack = (char**)realloc(I->call_stack, sizeof(char*) * (size_t)n);
        I->call_stack_cap = n;
    }
    I->call_stack[I->call_stack_len++] = riz_strdup(name ? name : "fn");
}

static void call_stack_pop(Interpreter* I) {
    if (I->call_stack_len <= 0) return;
    I->call_stack_len--;
    free(I->call_stack[I->call_stack_len]);
    I->call_stack[I->call_stack_len] = NULL;
}

static void interpreter_clear_error_stack(Interpreter* I) {
    if (!I) return;
    for (int i = 0; i < I->error_stack_len; i++) free(I->error_stack[i]);
    free(I->error_stack);
    I->error_stack = NULL;
    I->error_stack_len = 0;
}

static void interpreter_snapshot_error_stack(Interpreter* I) {
    interpreter_clear_error_stack(I);
    if (I->call_stack_len <= 0) return;
    I->error_stack_len = I->call_stack_len;
    I->error_stack = (char**)malloc(sizeof(char*) * (size_t)I->error_stack_len);
    for (int i = 0; i < I->error_stack_len; i++)
        I->error_stack[i] = riz_strdup(I->call_stack[i]);
}

static RizValue call_function(Interpreter* I, RizFunction* fn, RizValue* args, int argc) {
    /* Handle default parameters */
    int required = fn->param_defaults ? fn->required_count : fn->param_count;
    if (argc < required || argc > fn->param_count) {
        riz_runtime_error("Function '%s' expects %d-%d args, got %d",
                          fn->name ? fn->name : "anonymous", required, fn->param_count, argc);
        return riz_none();
    }
    Environment* call_env = env_new(fn->closure);
    for (int i = 0; i < argc; i++) env_define(call_env, fn->params[i], args[i], true);
    /* Fill in defaults for missing args */
    for (int i = argc; i < fn->param_count; i++) {
        if (fn->param_defaults && fn->param_defaults[i]) {
            RizValue defval = eval(I, fn->param_defaults[i]);
            env_define(call_env, fn->params[i], defval, true);
        } else {
            env_define(call_env, fn->params[i], riz_none(), true);
        }
    }
    Environment* saved = I->current_env; I->current_env = call_env;
    call_stack_push(I, fn->name);
    exec_block(I, fn->body);
    RizValue result = riz_none();
    if (I->signal == SIG_RETURN) { result = I->signal_value; I->signal = SIG_NONE; }
    call_stack_pop(I);
    I->current_env = saved;
    return result;
}

/* map/filter native functions (need call_function) */
RizValue native_map(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("map(list,fn) expected"); return riz_none(); }
    RizList*src=a[0].as.list; RizValue fn_val=a[1]; RizValue result=riz_list_new();
    for(int i=0;i<src->count;i++){RizValue item=src->items[i];RizValue mapped;
        if(fn_val.type==VAL_FUNCTION)mapped=call_function(g_interp,fn_val.as.function,&item,1);
        else if(fn_val.type==VAL_NATIVE_FN)mapped=fn_val.as.native->fn(&item,1);
        else{riz_runtime_error("map() 2nd arg must be callable");return riz_none();}
        riz_list_append(result.as.list,mapped);}
    return result;
}
RizValue native_filter(RizValue* a, int c) {
    if (c!=2||a[0].type!=VAL_LIST) { riz_runtime_error("filter(list,fn) expected"); return riz_none(); }
    RizList*src=a[0].as.list; RizValue fn_val=a[1]; RizValue result=riz_list_new();
    for(int i=0;i<src->count;i++){RizValue item=src->items[i];RizValue keep;
        if(fn_val.type==VAL_FUNCTION)keep=call_function(g_interp,fn_val.as.function,&item,1);
        else if(fn_val.type==VAL_NATIVE_FN)keep=fn_val.as.native->fn(&item,1);
        else{riz_runtime_error("filter() 2nd arg must be callable");return riz_none();}
        if(riz_value_is_truthy(keep))riz_list_append(result.as.list,riz_value_copy(item));}
    return result;
}

/* ═══════════════════════════════════════════════════════
 *  Import — load and execute another .riz file
 * ═══════════════════════════════════════════════════════ */

static void run_import(Interpreter* I, const char* path) {
    char resolved[1024];
    const char* use_path = path;
    if (riz_import_resolve(resolved, sizeof(resolved), path))
        use_path = resolved;

    /* Check if already imported */
    for (int i = 0; i < I->import_count; i++)
        if (strcmp(I->imported_files[i], use_path) == 0) return;

    /* Track */
    I->imported_files = (char**)realloc(I->imported_files, sizeof(char*) * (I->import_count + 1));
    I->imported_files[I->import_count++] = riz_strdup(use_path);

    /* Read file */
    FILE* f = fopen(use_path, "rb");
    if (!f) { riz_runtime_error("Cannot import '%s': file not found", path); return; }
    fseek(f, 0, SEEK_END); long length = ftell(f); fseek(f, 0, SEEK_SET);
    char* source = (char*)malloc(length + 1);
    size_t bytes_read = fread(source, 1, length, f); source[bytes_read] = '\0'; fclose(f);

    /* Parse and execute */
    Lexer lexer; lexer_init(&lexer, source);
    Parser parser; parser_init(&parser, &lexer);
    ASTNode* program = parser_parse(&parser);
    if (!parser.had_error) eval(I, program);
    free(source);
}

/* ═══════════════════════════════════════════════════════
 *  Expression Evaluation
 * ═══════════════════════════════════════════════════════ */

static RizValue eval_binary(Interpreter* I, ASTNode* node) {
    RizTokenType op = node->as.binary.op;

    /* Short-circuit logical */
    if (op == TOK_AND) { RizValue l=eval(I,node->as.binary.left); return riz_bool(!riz_value_is_truthy(l)?false:riz_value_is_truthy(eval(I,node->as.binary.right))); }
    if (op == TOK_OR)  { RizValue l=eval(I,node->as.binary.left); return riz_bool(riz_value_is_truthy(l)?true:riz_value_is_truthy(eval(I,node->as.binary.right))); }

    RizValue left = eval(I, node->as.binary.left);
    RizValue right = eval(I, node->as.binary.right);

    /* Tensor / Native Operator Intercept */
    if (left.type == VAL_NATIVE_PTR || right.type == VAL_NATIVE_PTR) {
        if ((left.type != VAL_NATIVE_PTR || strcmp(left.as.native_ptr->type_tag, "Tensor") == 0) &&
            (right.type != VAL_NATIVE_PTR || strcmp(right.as.native_ptr->type_tag, "Tensor") == 0)) {
            const char* op_fn = NULL;
            if (op == TOK_PLUS)  op_fn = "tensor_add";
            if (op == TOK_MINUS) op_fn = "tensor_sub";
            if (op == TOK_STAR)  op_fn = "tensor_matmul";
            
            if (op_fn) {
                RizValue ffi_fn;
                if (env_get(I->globals, op_fn, &ffi_fn)) {
                    if (ffi_fn.type == VAL_NATIVE_FN) {
                        RizValue args[2] = {left, right};
                        return ffi_fn.as.native->fn(args, 2);
                    }
                }
            }
        }
    }

    /* String concatenation */
    if (op==TOK_PLUS && left.type==VAL_STRING && right.type==VAL_STRING) {
        size_t la=strlen(left.as.string),lb=strlen(right.as.string);
        char*r=(char*)malloc(la+lb+1);memcpy(r,left.as.string,la);memcpy(r+la,right.as.string,lb+1);
        return riz_string_take(r);
    }
    /* String + other → auto-concat */
    if (op==TOK_PLUS && (left.type==VAL_STRING||right.type==VAL_STRING)) {
        char*ls=riz_value_to_string(left);char*rs=riz_value_to_string(right);
        size_t la=strlen(ls),lb=strlen(rs);char*r=(char*)malloc(la+lb+1);
        memcpy(r,ls,la);memcpy(r+la,rs,lb+1);free(ls);free(rs);return riz_string_take(r);
    }
    /* String repetition */
    if (op==TOK_STAR && left.type==VAL_STRING && right.type==VAL_INT) {
        int64_t n=right.as.integer; if(n<=0)return riz_string("");
        size_t l=strlen(left.as.string);char*r=(char*)malloc(l*n+1);
        for(int64_t i=0;i<n;i++)memcpy(r+i*l,left.as.string,l);r[l*n]='\0';return riz_string_take(r);
    }
    /* List concatenation */
    if (op==TOK_PLUS && left.type==VAL_LIST && right.type==VAL_LIST) {
        RizValue result = riz_list_new();
        for(int i=0;i<left.as.list->count;i++) riz_list_append(result.as.list, riz_value_copy(left.as.list->items[i]));
        for(int i=0;i<right.as.list->count;i++) riz_list_append(result.as.list, riz_value_copy(right.as.list->items[i]));
        return result;
    }
    /* Dict merge: d1 + d2 */
    if (op==TOK_PLUS && left.type==VAL_DICT && right.type==VAL_DICT) {
        RizValue result = riz_dict_new();
        RizDict* l=left.as.dict; RizDict* r=right.as.dict;
        for(int i=0;i<l->count;i++) riz_dict_set(result.as.dict, l->keys[i], riz_value_copy(l->values[i]));
        for(int i=0;i<r->count;i++) riz_dict_set(result.as.dict, r->keys[i], riz_value_copy(r->values[i]));
        return result;
    }

    /* Equality */
    if (op==TOK_EQ)  return riz_bool(riz_value_equal(left, right));
    if (op==TOK_NEQ) return riz_bool(!riz_value_equal(left, right));

    /* Phase 3: 'in' operator — membership test */
    if (op==TOK_IN) {
        if (right.type==VAL_LIST) {
            for(int i=0;i<right.as.list->count;i++) if(riz_value_equal(left,right.as.list->items[i])) return riz_bool(true);
            return riz_bool(false);
        }
        if (right.type==VAL_DICT && left.type==VAL_STRING) return riz_bool(riz_dict_has(right.as.dict,left.as.string));
        if (right.type==VAL_STRING && left.type==VAL_STRING) return riz_bool(strstr(right.as.string,left.as.string)!=NULL);
        riz_runtime_error("'in' not supported for %s",riz_value_type_name(right)); return riz_bool(false);
    }

    /* Arithmetic */
    bool use_float = (left.type==VAL_FLOAT||right.type==VAL_FLOAT);
    double lf,rf; int64_t li,ri;
    if (use_float) { lf=left.type==VAL_INT?(double)left.as.integer:left.as.floating; rf=right.type==VAL_INT?(double)right.as.integer:right.as.floating; }
    else if (left.type==VAL_INT&&right.type==VAL_INT) { li=left.as.integer; ri=right.as.integer; }
    else { riz_runtime_error("Unsupported operand types: %s and %s",riz_value_type_name(left),riz_value_type_name(right)); I->had_error=true; return riz_none(); }

    switch (op) {
        case TOK_PLUS:  return use_float?riz_float(lf+rf):riz_int(li+ri);
        case TOK_MINUS: return use_float?riz_float(lf-rf):riz_int(li-ri);
        case TOK_STAR:  return use_float?riz_float(lf*rf):riz_int(li*ri);
        case TOK_SLASH:
            if(use_float){if(rf==0){riz_runtime_error("Division by zero");return riz_none();}return riz_float(lf/rf);}
            else{if(ri==0){riz_runtime_error("Division by zero");return riz_none();}return riz_float((double)li/(double)ri);}
        case TOK_FLOOR_DIV:
            if(use_float){if(rf==0){riz_runtime_error("Division by zero");return riz_none();}return riz_int((int64_t)floor(lf/rf));}
            else{if(ri==0){riz_runtime_error("Division by zero");return riz_none();}return riz_int(li/ri);}
        case TOK_PERCENT:
            if(use_float){if(rf==0){riz_runtime_error("Modulo by zero");return riz_none();}return riz_float(fmod(lf,rf));}
            else{if(ri==0){riz_runtime_error("Modulo by zero");return riz_none();}return riz_int(li%ri);}
        case TOK_POWER:
            if(use_float)return riz_float(pow(lf,rf));
            else{if(ri<0)return riz_float(pow((double)li,(double)ri));
                int64_t r=1,b=li,e=ri;while(e>0){if(e&1)r*=b;b*=b;e>>=1;}return riz_int(r);}
        case TOK_LT:  return riz_bool(use_float?lf<rf:li<ri);
        case TOK_GT:  return riz_bool(use_float?lf>rf:li>ri);
        case TOK_LTE: return riz_bool(use_float?lf<=rf:li<=ri);
        case TOK_GTE: return riz_bool(use_float?lf>=rf:li>=ri);
        default: riz_runtime_error("Unknown binary operator"); return riz_none();
    }
}

static RizValue eval_unary(Interpreter* I, ASTNode* node) {
    RizValue v = eval(I, node->as.unary.operand);
    switch (node->as.unary.op) {
        case TOK_MINUS:
            if(v.type==VAL_INT) return riz_int(-v.as.integer);
            if(v.type==VAL_FLOAT) return riz_float(-v.as.floating);
            riz_runtime_error("Cannot negate %s",riz_value_type_name(v)); return riz_none();
        case TOK_NOT: case TOK_BANG: return riz_bool(!riz_value_is_truthy(v));
        default: return riz_none();
    }
}

static RizValue eval_call(Interpreter* I, ASTNode* node) {
    /* Method call: obj.method(args) */
    ASTNode* callee_node = node->as.call.callee;
    if (callee_node->type == NODE_MEMBER) {
        /* Check if it's a property (not method) by trying method dispatch first */
        return eval_method_call(I, node);
    }

    RizValue callee = eval(I, callee_node);
    int argc = node->as.call.arg_count;
    RizValue* args = NULL;
    if (argc > 0) { args = RIZ_ALLOC_ARRAY(RizValue, argc); for(int i=0;i<argc;i++) args[i]=eval(I,node->as.call.args[i]); }

    RizValue result;
    if (callee.type == VAL_NATIVE_FN) {
        NativeFnObj* n=callee.as.native;
        if(n->arity>=0&&argc!=n->arity){riz_runtime_error("%s() takes %d arg(s), %d given",n->name,n->arity,argc);result=riz_none();}
        else result=n->fn(args,argc);
    } else if (callee.type == VAL_FUNCTION) {
        result = call_function(I, callee.as.function, args, argc);
    } else if (callee.type == VAL_STRUCT_DEF) {
        /* Struct constructor: StructName(field1, field2, ...) */
        RizStructDef* def = callee.as.struct_def;
        if (argc != def->field_count) {
            riz_runtime_error("Struct '%s' expects %d fields, got %d", def->name, def->field_count, argc);
            result = riz_none();
        } else {
            result = riz_instance_new(def, args);
        }
    } else { riz_runtime_error("Cannot call %s",riz_value_type_name(callee)); result=riz_none(); }

    free(args);
    return result;
}

static RizValue eval_pipe(Interpreter* I, ASTNode* node) {
    RizValue left = eval(I, node->as.pipe.left);
    ASTNode* right = node->as.pipe.right;

    if (right->type == NODE_CALL) {
        RizValue callee = eval(I, right->as.call.callee);
        int oc = right->as.call.arg_count, ac=oc+1;
        RizValue* args = RIZ_ALLOC_ARRAY(RizValue, ac);
        args[0]=left; for(int i=0;i<oc;i++) args[i+1]=eval(I,right->as.call.args[i]);
        RizValue result;
        if(callee.type==VAL_NATIVE_FN) result=callee.as.native->fn(args,ac);
        else if(callee.type==VAL_FUNCTION) result=call_function(I,callee.as.function,args,ac);
        else{riz_runtime_error("Pipe target must be callable");result=riz_none();}
        free(args); return result;
    }
    if (right->type == NODE_IDENTIFIER) {
        RizValue callee = eval(I, right);
        if(callee.type==VAL_NATIVE_FN) return callee.as.native->fn(&left,1);
        if(callee.type==VAL_FUNCTION)  return call_function(I,callee.as.function,&left,1);
        riz_runtime_error("Pipe target must be callable"); return riz_none();
    }
    riz_runtime_error("Invalid pipe target"); return riz_none();
}

/* ═══ Match expression ═══ */

static RizValue eval_match(Interpreter* I, ASTNode* node) {
    RizValue subject = eval(I, node->as.match_expr.subject);

    for (int i = 0; i < node->as.match_expr.arm_count; i++) {
        RizMatchArm* arm = &node->as.match_expr.arms[i];
        ASTNode* pattern = arm->pattern;
        bool matched = false;

        /* Wildcard: _ */
        if (pattern->type == NODE_IDENTIFIER && strcmp(pattern->as.identifier.name, "_") == 0) {
            matched = true;
        }
        /* Identifier binding (not _): bind the value and always match */
        else if (pattern->type == NODE_IDENTIFIER) {
            /* Create scope with binding */
            Environment* match_env = env_new(I->current_env);
            env_define(match_env, pattern->as.identifier.name, riz_value_copy(subject), false);
            Environment* saved = I->current_env;
            I->current_env = match_env;

            /* Check guard */
            if (arm->guard) {
                RizValue gv = eval(I, arm->guard);
                if (!riz_value_is_truthy(gv)) { I->current_env = saved; continue; }
            }

            /* Execute body */
            RizValue result;
            if (arm->body->type == NODE_BLOCK) { exec_block(I, arm->body); result = riz_none(); }
            else result = eval(I, arm->body);

            I->current_env = saved;
            return result;
        }
        /* Literal pattern: compare with subject */
        else {
            RizValue pv = eval(I, pattern);
            matched = riz_value_equal(subject, pv);
        }

        if (matched) {
            /* Check guard */
            if (arm->guard) {
                RizValue gv = eval(I, arm->guard);
                if (!riz_value_is_truthy(gv)) continue;
            }
            /* Execute body */
            if (arm->body->type == NODE_BLOCK) { exec_block(I, arm->body); return riz_none(); }
            return eval(I, arm->body);
        }
    }
    /* No arm matched */
    return riz_none();
}

/* ═══════════════════════════════════════════════════════
 *  Main eval dispatch
 * ═══════════════════════════════════════════════════════ */

static RizValue eval(Interpreter* I, ASTNode* node) {
    if (!node || I->had_error) return riz_none();
    I->current_line = node->line;
    switch (node->type) {
        /* Literals */
        case NODE_INT_LIT:    return riz_int(node->as.int_lit.value);
        case NODE_FLOAT_LIT:  return riz_float(node->as.float_lit.value);
        case NODE_STRING_LIT: return riz_string(node->as.string_lit.value);
        case NODE_BOOL_LIT:   return riz_bool(node->as.bool_lit.value);
        case NODE_NONE_LIT:   return riz_none();

        case NODE_LIST_LIT: {
            RizValue list = riz_list_new();
            for(int i=0;i<node->as.list_lit.count;i++) riz_list_append(list.as.list, eval(I,node->as.list_lit.items[i]));
            return list;
        }
        case NODE_DICT_LIT: {
            RizValue dict = riz_dict_new();
            for(int i=0;i<node->as.dict_lit.count;i++){
                RizValue key = eval(I, node->as.dict_lit.keys[i]);
                RizValue val = eval(I, node->as.dict_lit.values[i]);
                char* key_str;
                if (key.type==VAL_STRING) key_str=riz_strdup(key.as.string);
                else key_str=riz_value_to_string(key);
                riz_dict_set(dict.as.dict, key_str, val);
                free(key_str);
            }
            return dict;
        }
        case NODE_IDENTIFIER: {
            RizValue val;
            if(!env_get(I->current_env, node->as.identifier.name, &val)){
                riz_runtime_error("Undefined variable '%s'",node->as.identifier.name); I->had_error=true; return riz_none();
            }
            return val;
        }
        case NODE_UNARY:      return eval_unary(I, node);
        case NODE_BINARY:     return eval_binary(I, node);
        case NODE_CALL:       return eval_call(I, node);
        case NODE_PIPE:       return eval_pipe(I, node);
        case NODE_MATCH_EXPR: return eval_match(I, node);

        /* Ternary: value if condition else other */
        case NODE_TERNARY: {
            RizValue cond = eval(I, node->as.ternary.condition);
            if (riz_value_is_truthy(cond)) return eval(I, node->as.ternary.true_expr);
            else return eval(I, node->as.ternary.false_expr);
        }

        /* List comprehension: [expr for var in iter if cond] */
        case NODE_LIST_COMP: {
            RizValue iterable = eval(I, node->as.list_comp.iterable);
            if (iterable.type != VAL_LIST) {
                riz_runtime_error("Cannot iterate over %s in list comprehension", riz_value_type_name(iterable));
                return riz_none();
            }
            RizValue result = riz_list_new();
            RizList* list = iterable.as.list;
            for (int i = 0; i < list->count; i++) {
                Environment* le = env_new(I->current_env);
                Environment* saved = I->current_env;
                I->current_env = le;
                env_define(le, node->as.list_comp.var_name, riz_value_copy(list->items[i]), false);
                bool include = true;
                if (node->as.list_comp.condition) {
                    RizValue cv = eval(I, node->as.list_comp.condition);
                    include = riz_value_is_truthy(cv);
                }
                if (include) {
                    RizValue v = eval(I, node->as.list_comp.expr);
                    riz_list_append(result.as.list, v);
                }
                I->current_env = saved;
            }
            return result;
        }

        /* Slice: obj[start:end:step] */
        case NODE_SLICE: {
            RizValue obj = eval(I, node->as.slice.object);
            if (obj.type == VAL_LIST) {
                int count = obj.as.list->count;
                int start = 0, end = count, step = 1;
                if (node->as.slice.start) { RizValue sv = eval(I, node->as.slice.start); if (sv.type==VAL_INT) { start=(int)sv.as.integer; if(start<0)start+=count; } }
                if (node->as.slice.end)   { RizValue ev = eval(I, node->as.slice.end);   if (ev.type==VAL_INT) { end=(int)ev.as.integer; if(end<0)end+=count; } }
                if (node->as.slice.step)  { RizValue tv = eval(I, node->as.slice.step);  if (tv.type==VAL_INT) step=(int)tv.as.integer; }
                if (step == 0) { riz_runtime_error("Slice step cannot be zero"); return riz_none(); }
                if (start < 0) start = 0; if (end > count) end = count;
                RizValue result = riz_list_new();
                if (step > 0) { for (int i = start; i < end; i += step) riz_list_append(result.as.list, riz_value_copy(obj.as.list->items[i])); }
                else { for (int i = (end > 0 ? end - 1 : count - 1); i >= start; i += step) riz_list_append(result.as.list, riz_value_copy(obj.as.list->items[i])); }
                return result;
            }
            if (obj.type == VAL_STRING) {
                int slen = (int)strlen(obj.as.string);
                int start = 0, end = slen, step = 1;
                if (node->as.slice.start) { RizValue sv = eval(I, node->as.slice.start); if (sv.type==VAL_INT) { start=(int)sv.as.integer; if(start<0)start+=slen; } }
                if (node->as.slice.end)   { RizValue ev = eval(I, node->as.slice.end);   if (ev.type==VAL_INT) { end=(int)ev.as.integer; if(end<0)end+=slen; } }
                if (node->as.slice.step)  { RizValue tv = eval(I, node->as.slice.step);  if (tv.type==VAL_INT) step=(int)tv.as.integer; }
                if (step == 0) { riz_runtime_error("Slice step cannot be zero"); return riz_none(); }
                if (start < 0) start = 0; if (end > slen) end = slen;
                size_t cap = (size_t)(end - start + 1); char* buf = (char*)malloc(cap + 1); int len = 0;
                if (step > 0) { for (int i = start; i < end; i += step) buf[len++] = obj.as.string[i]; }
                else { for (int i = (end > 0 ? end - 1 : slen - 1); i >= start; i += step) buf[len++] = obj.as.string[i]; }
                buf[len] = '\0';
                return riz_string_take(buf);
            }
            riz_runtime_error("Cannot slice %s", riz_value_type_name(obj));
            return riz_none();
        }

        case NODE_INDEX: {
            RizValue obj = eval(I, node->as.index_expr.object);
            RizValue idx = eval(I, node->as.index_expr.index);
            if (obj.type==VAL_LIST && idx.type==VAL_INT) {
                int64_t i=idx.as.integer; if(i<0)i+=obj.as.list->count;
                return riz_list_get(obj.as.list, (int)i);
            }
            if (obj.type==VAL_STRING && idx.type==VAL_INT) {
                int64_t i=idx.as.integer; size_t len=strlen(obj.as.string);
                if(i<0)i+=(int64_t)len;
                if(i<0||i>=(int64_t)len){riz_runtime_error("String index out of range");return riz_none();}
                char buf[2]={obj.as.string[i],'\0'}; return riz_string(buf);
            }
            /* Dict indexing: d["key"] */
            if (obj.type==VAL_DICT && idx.type==VAL_STRING) {
                return riz_dict_get(obj.as.dict, idx.as.string);
            }
            riz_runtime_error("Cannot index %s with %s",riz_value_type_name(obj),riz_value_type_name(idx));
            return riz_none();
        }

        case NODE_ASSIGN: {
            RizValue val = eval(I, node->as.assign.value);
            if(!env_set(I->current_env,node->as.assign.name,val)) I->had_error=true;
            return val;
        }

        case NODE_COMPOUND_ASSIGN: {
            RizValue current; if(!env_get(I->current_env,node->as.compound_assign.name,&current)){riz_runtime_error("Undefined variable '%s'",node->as.compound_assign.name);return riz_none();}
            RizValue rhs = eval(I, node->as.compound_assign.value);
            ASTNode synth; synth.type=NODE_BINARY; synth.line=node->line; synth.as.binary.op=node->as.compound_assign.op;
            ASTNode ln,rn;
            if(current.type==VAL_INT){ln.type=NODE_INT_LIT;ln.as.int_lit.value=current.as.integer;}
            else if(current.type==VAL_FLOAT){ln.type=NODE_FLOAT_LIT;ln.as.float_lit.value=current.as.floating;}
            else if(current.type==VAL_STRING){ln.type=NODE_STRING_LIT;ln.as.string_lit.value=current.as.string;}
            else{riz_runtime_error("Cannot use compound assignment on %s",riz_value_type_name(current));return riz_none();}
            if(rhs.type==VAL_INT){rn.type=NODE_INT_LIT;rn.as.int_lit.value=rhs.as.integer;}
            else if(rhs.type==VAL_FLOAT){rn.type=NODE_FLOAT_LIT;rn.as.float_lit.value=rhs.as.floating;}
            else if(rhs.type==VAL_STRING){rn.type=NODE_STRING_LIT;rn.as.string_lit.value=rhs.as.string;}
            else{riz_runtime_error("Cannot use compound assignment with %s",riz_value_type_name(rhs));return riz_none();}
            synth.as.binary.left=&ln;synth.as.binary.right=&rn;
            RizValue result=eval_binary(I,&synth);
            env_set(I->current_env,node->as.compound_assign.name,result);
            return result;
        }

        case NODE_MEMBER: {
            RizValue obj = eval(I, node->as.member.object);
            const char* m = node->as.member.member;
            /* Properties (non-callable) */
            if (obj.type==VAL_LIST && strcmp(m,"length")==0) return riz_int(obj.as.list->count);
            if (obj.type==VAL_STRING && strcmp(m,"length")==0) return riz_int((int64_t)strlen(obj.as.string));
            if (obj.type==VAL_DICT) {
                if (riz_dict_has(obj.as.dict, m)) return riz_dict_get(obj.as.dict, m);
                if (strcmp(m,"length")==0 || strcmp(m,"count")==0) return riz_int(obj.as.dict->count);
            }
            /* Instance field access */
            if (obj.type==VAL_INSTANCE) {
                RizInstance* inst = obj.as.instance;
                for (int i = 0; i < inst->def->field_count; i++) {
                    if (strcmp(inst->def->field_names[i], m) == 0)
                        return riz_value_copy(inst->fields[i]);
                }
                riz_runtime_error("'%s' has no field '%s'", inst->def->name, m);
                return riz_none();
            }
            riz_runtime_error("'%s' has no member '%s'",riz_value_type_name(obj),m);
            return riz_none();
        }

        case NODE_LAMBDA: {
            RizFunction* fn = RIZ_ALLOC(RizFunction);
            fn->name=riz_strdup("<lambda>"); fn->param_count=node->as.lambda.param_count;
            fn->params=RIZ_ALLOC_ARRAY(char*,fn->param_count);
            for(int i=0;i<fn->param_count;i++) fn->params[i]=riz_strdup(node->as.lambda.params[i]);
            fn->body=node->as.lambda.body; fn->closure=I->current_env;
            fn->param_defaults=NULL; fn->required_count=fn->param_count;
            return riz_fn(fn);
        }

        /* ─── Statements ─── */
        case NODE_EXPR_STMT: return eval(I, node->as.expr_stmt.expr);

        case NODE_LET_DECL: {
            RizValue val = eval(I, node->as.let_decl.initializer);
            env_define(I->current_env, node->as.let_decl.name, val, node->as.let_decl.is_mutable);
            return riz_none();
        }
        case NODE_FN_DECL: {
            RizFunction* fn = RIZ_ALLOC(RizFunction);
            fn->name=riz_strdup(node->as.fn_decl.name); fn->param_count=node->as.fn_decl.param_count;
            fn->params=RIZ_ALLOC_ARRAY(char*,fn->param_count);
            for(int i=0;i<fn->param_count;i++) fn->params[i]=riz_strdup(node->as.fn_decl.params[i]);
            fn->body=node->as.fn_decl.body; fn->closure=I->current_env;
            /* Default parameters (Phase 3) */
            fn->param_defaults = node->as.fn_decl.param_defaults; /* share with AST */
            if (fn->param_defaults) {
                fn->required_count = 0;
                for (int i = 0; i < fn->param_count; i++) {
                    if (!fn->param_defaults[i]) fn->required_count = i + 1;
                }
            } else {
                fn->required_count = fn->param_count;
            }
            env_define(I->current_env, fn->name, riz_fn(fn), false);
            return riz_none();
        }
        case NODE_RETURN_STMT: {
            RizValue val = node->as.return_stmt.value ? eval(I,node->as.return_stmt.value) : riz_none();
            I->signal=SIG_RETURN; I->signal_value=val; return val;
        }
        case NODE_IF_STMT: {
            RizValue cond = eval(I, node->as.if_stmt.condition);
            if (riz_value_is_truthy(cond)) exec_block(I, node->as.if_stmt.then_branch);
            else if (node->as.if_stmt.else_branch) {
                if(node->as.if_stmt.else_branch->type==NODE_IF_STMT) eval(I,node->as.if_stmt.else_branch);
                else exec_block(I, node->as.if_stmt.else_branch);
            }
            return riz_none();
        }
        case NODE_WHILE_STMT: {
            while(true) {
                RizValue c=eval(I,node->as.while_stmt.condition); if(!riz_value_is_truthy(c))break;
                exec_block(I,node->as.while_stmt.body);
                if(I->signal==SIG_BREAK){I->signal=SIG_NONE;break;}
                if(I->signal==SIG_CONTINUE){I->signal=SIG_NONE;continue;}
                if(I->signal==SIG_RETURN)break;
            }
            return riz_none();
        }
        case NODE_FOR_STMT: {
            RizValue iterable = eval(I, node->as.for_stmt.iterable);
            if(iterable.type!=VAL_LIST){riz_runtime_error("Cannot iterate over %s",riz_value_type_name(iterable));return riz_none();}
            RizList* list = iterable.as.list;
            bool did_break = false;
            for(int i=0;i<list->count;i++){
                Environment* le=env_new(I->current_env); Environment* saved=I->current_env; I->current_env=le;
                env_define(le,node->as.for_stmt.var_name,riz_value_copy(list->items[i]),false);
                if(node->as.for_stmt.body->type==NODE_BLOCK){
                    ASTNode* b=node->as.for_stmt.body;
                    for(int j=0;j<b->as.block.count;j++){eval(I,b->as.block.statements[j]);if(I->signal!=SIG_NONE)break;}
                } else eval(I,node->as.for_stmt.body);
                I->current_env=saved;
                if(I->signal==SIG_BREAK){I->signal=SIG_NONE;did_break=true;break;}
                if(I->signal==SIG_CONTINUE){I->signal=SIG_NONE;continue;}
                if(I->signal==SIG_RETURN)break;
            }
            /* for...else: else block runs if loop completed without break */
            if (!did_break && I->signal == SIG_NONE && node->as.for_stmt.else_branch) {
                exec_block(I, node->as.for_stmt.else_branch);
            }
            return riz_none();
        }
        case NODE_BREAK_STMT:    I->signal=SIG_BREAK; return riz_none();
        case NODE_CONTINUE_STMT: I->signal=SIG_CONTINUE; return riz_none();

        case NODE_IMPORT: run_import(I, node->as.import_stmt.path); return riz_none();
        case NODE_IMPORT_NATIVE: load_native_plugin(I, node->as.import_native.path); return riz_none();
        case NODE_BLOCK:  exec_block(I, node); return riz_none();

        /* Phase 3: struct declaration */
        case NODE_STRUCT_DECL: {
            /* Create field names array (owned by struct def) */
            char** fields = RIZ_ALLOC_ARRAY(char*, node->as.struct_decl.field_count);
            for (int i = 0; i < node->as.struct_decl.field_count; i++)
                fields[i] = riz_strdup(node->as.struct_decl.field_names[i]);
            RizValue def = riz_struct_def_new(node->as.struct_decl.name, fields, node->as.struct_decl.field_count);
            env_define(I->current_env, node->as.struct_decl.name, def, false);
            return riz_none();
        }

        /* Phase 3: impl block */
        case NODE_IMPL_DECL: {
            RizValue sv;
            if (!env_get(I->current_env, node->as.impl_decl.struct_name, &sv) || sv.type != VAL_STRUCT_DEF) {
                riz_runtime_error("'%s' is not a struct", node->as.impl_decl.struct_name);
                return riz_none();
            }
            RizStructDef* def = sv.as.struct_def;
            for (int i = 0; i < node->as.impl_decl.method_count; i++) {
                ASTNode* method = node->as.impl_decl.methods[i];
                RizFunction* fn = RIZ_ALLOC(RizFunction);
                fn->name = riz_strdup(method->as.fn_decl.name);
                fn->param_count = method->as.fn_decl.param_count;
                fn->params = RIZ_ALLOC_ARRAY(char*, fn->param_count);
                for (int j = 0; j < fn->param_count; j++)
                    fn->params[j] = riz_strdup(method->as.fn_decl.params[j]);
                fn->body = method->as.fn_decl.body;
                fn->closure = I->current_env;
                fn->param_defaults = method->as.fn_decl.param_defaults;
                if (fn->param_defaults) {
                    fn->required_count = 0;
                    for (int j = 0; j < fn->param_count; j++) {
                        if (!fn->param_defaults[j]) fn->required_count = j + 1;
                    }
                } else {
                    fn->required_count = fn->param_count;
                }
                riz_struct_add_method(def, fn->name, riz_fn(fn));
            }
            return riz_none();
        }

        /* Phase 4: trait declaration */
        case NODE_TRAIT_DECL: {
            RizTraitDef* def = RIZ_ALLOC(RizTraitDef);
            def->name = riz_strdup(node->as.trait_decl.name);
            def->method_count = node->as.trait_decl.method_count;
            def->method_names = RIZ_ALLOC_ARRAY(char*, def->method_count);
            def->method_arity = RIZ_ALLOC_ARRAY(int, def->method_count);
            for (int i = 0; i < def->method_count; i++) {
                ASTNode* method = node->as.trait_decl.methods[i];
                def->method_names[i] = riz_strdup(method->as.fn_decl.name);
                def->method_arity[i] = method->as.fn_decl.param_count;
            }
            RizValue val; val.type = VAL_TRAIT_DEF; val.as.trait_def = def;
            env_define(I->current_env, def->name, val, false);
            return riz_none();
        }

        /* Phase 4: impl Trait for Struct block */
        case NODE_IMPL_TRAIT_DECL: {
            RizValue trait_val, struct_val;
            if (!env_get(I->current_env, node->as.impl_trait_decl.trait_name, &trait_val) || trait_val.type != VAL_TRAIT_DEF) {
                riz_runtime_error("'%s' is not a trait", node->as.impl_trait_decl.trait_name);
                return riz_none();
            }
            if (!env_get(I->current_env, node->as.impl_trait_decl.struct_name, &struct_val) || struct_val.type != VAL_STRUCT_DEF) {
                riz_runtime_error("'%s' is not a struct", node->as.impl_trait_decl.struct_name);
                return riz_none();
            }
            RizTraitDef* trait_def = trait_val.as.trait_def;
            RizStructDef* struct_def = struct_val.as.struct_def;

            int provided_count = node->as.impl_trait_decl.method_count;
            ASTNode** methods = node->as.impl_trait_decl.methods;

            for (int i = 0; i < trait_def->method_count; i++) {
                const char* t_name = trait_def->method_names[i];
                int t_arity = trait_def->method_arity[i];
                bool found = false;
                for (int j = 0; j < provided_count; j++) {
                    if (strcmp(methods[j]->as.fn_decl.name, t_name) == 0) {
                        if (methods[j]->as.fn_decl.param_count != t_arity) {
                            riz_runtime_error("Method '%s' in impl for '%s' expects %d params but trait requires %d", 
                                              t_name, struct_def->name, methods[j]->as.fn_decl.param_count, t_arity);
                            return riz_none();
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    riz_runtime_error("Struct '%s' does not implement required trait method '%s'", struct_def->name, t_name);
                    return riz_none();
                }
            }

            for (int i = 0; i < provided_count; i++) {
                ASTNode* method = methods[i];
                RizFunction* fn = RIZ_ALLOC(RizFunction);
                fn->name = riz_strdup(method->as.fn_decl.name);
                fn->param_count = method->as.fn_decl.param_count;
                fn->params = RIZ_ALLOC_ARRAY(char*, fn->param_count);
                for (int j = 0; j < fn->param_count; j++) fn->params[j] = riz_strdup(method->as.fn_decl.params[j]);
                fn->body = method->as.fn_decl.body; fn->closure = I->current_env;
                fn->param_defaults = method->as.fn_decl.param_defaults;
                if (fn->param_defaults) {
                    fn->required_count = 0;
                    for (int j = 0; j < fn->param_count; j++) if (!fn->param_defaults[j]) fn->required_count = j + 1;
                } else { fn->required_count = fn->param_count; }
                riz_struct_add_method(struct_def, fn->name, riz_fn(fn));
            }
            return riz_none();
        }

        /* Phase 3: try/catch */
        case NODE_TRY_STMT: {
            exec_block(I, node->as.try_stmt.try_block);
            if (I->signal == SIG_THROW) {
                I->signal = SIG_NONE;
                RizValue error_val = I->signal_value;
                interpreter_clear_error_stack(I);
                Environment* catch_env = env_new(I->current_env);
                env_define(catch_env, node->as.try_stmt.catch_var, error_val, false);
                Environment* saved = I->current_env;
                I->current_env = catch_env;
                exec_block(I, node->as.try_stmt.catch_block);
                I->current_env = saved;
            }
            return riz_none();
        }

        /* Phase 3: throw */
        case NODE_THROW_STMT: {
            RizValue val = eval(I, node->as.throw_stmt.value);
            interpreter_snapshot_error_stack(I);
            I->signal = SIG_THROW;
            I->signal_value = val;
            return riz_none();
        }

        /* Phase 3: member assignment — obj.field = value */
        case NODE_MEMBER_ASSIGN: {
            RizValue obj = eval(I, node->as.member_assign.object);
            RizValue val = eval(I, node->as.member_assign.value);
            const char* member = node->as.member_assign.member;
            if (obj.type == VAL_INSTANCE) {
                RizInstance* inst = obj.as.instance;
                for (int i = 0; i < inst->def->field_count; i++) {
                    if (strcmp(inst->def->field_names[i], member) == 0) {
                        inst->fields[i] = val;
                        return val;
                    }
                }
                riz_runtime_error("'%s' has no field '%s'", inst->def->name, member);
            } else if (obj.type == VAL_DICT) {
                riz_dict_set(obj.as.dict, member, val);
                return val;
            } else {
                riz_runtime_error("Cannot assign to member of %s", riz_value_type_name(obj));
            }
            return riz_none();
        }

        /* Phase 3: index assignment — obj[idx] = value */
        case NODE_INDEX_ASSIGN: {
            RizValue obj = eval(I, node->as.index_assign.object);
            RizValue idx = eval(I, node->as.index_assign.index);
            RizValue val = eval(I, node->as.index_assign.value);
            if (obj.type == VAL_LIST && idx.type == VAL_INT) {
                int64_t i = idx.as.integer;
                if (i < 0) i += obj.as.list->count;
                if (i < 0 || i >= obj.as.list->count) { riz_runtime_error("Index out of range"); return riz_none(); }
                obj.as.list->items[i] = val;
                return val;
            }
            if (obj.type == VAL_DICT) {
                char* key = (idx.type==VAL_STRING) ? idx.as.string : riz_value_to_string(idx);
                riz_dict_set(obj.as.dict, key, val);
                if (idx.type != VAL_STRING) free(key);
                return val;
            }
            riz_runtime_error("Cannot assign to index of %s", riz_value_type_name(obj));
            return riz_none();
        }

        case NODE_PROGRAM: {
            for(int i=0;i<node->as.program.count;i++){eval(I,node->as.program.declarations[i]);if(I->signal!=SIG_NONE)break;}
            return riz_none();
        }
        default: riz_runtime_error("Unknown AST node type: %d",node->type); return riz_none();
    }
}

/* ═══ Block execution ═══ */

static void exec_block(Interpreter* I, ASTNode* block) {
    if(!block) return;
    if(block->type!=NODE_BLOCK){eval(I,block);return;}
    Environment* be=env_new(I->current_env); Environment* saved=I->current_env; I->current_env=be;
    for(int i=0;i<block->as.block.count;i++){
        eval(I,block->as.block.statements[i]);
        if(I->signal!=SIG_NONE) break;
    }
    I->current_env=saved;
}

/* ═══ Public API ═══ */

void interpreter_exec(Interpreter* I, ASTNode* program) {
    if (setjmp(I->panic_jmp) == 0) {
        g_interp = I;
        eval(I, program);
    }
    // Any error (longjmp) returns here safely.
}
RizValue interpreter_eval(Interpreter* interp, ASTNode* node) { g_interp=interp; return eval(interp,node); }

void interpreter_report_pending_signal(Interpreter* interp) {
    if (!interp || interp->signal != SIG_THROW) return;
    fprintf(stderr, COL_RED COL_BOLD "Uncaught exception:" COL_RESET " ");
    char* s = riz_value_to_string(interp->signal_value);
    fprintf(stderr, "%s\n", s);
    free(s);
    if (interp->error_stack_len > 0) {
        fprintf(stderr, COL_DIM "Call stack (innermost last):\n" COL_RESET);
        for (int i = 0; i < interp->error_stack_len; i++)
            fprintf(stderr, COL_DIM "  at %s\n" COL_RESET, interp->error_stack[i]);
    }
    interpreter_clear_error_stack(interp);
    interp->had_error = true;
    interp->signal = SIG_NONE;
}
