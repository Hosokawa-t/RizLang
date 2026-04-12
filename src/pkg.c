/*
 * Riz package manager — riz.json + riz.deps, vendor/, git, riz.lock
 * (Mojo-like: manifest dependencies, reproducible lock, remote fetch)
 */

#include "pkg.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(d) _mkdir(d)
#define VJOIN(buf, sz, name) snprintf(buf, sz, "vendor\\%s", name)
#define POPEN _popen
#define PCLOSE _pclose
#define PATH_EXISTS(p) (_access((p), 0) == 0)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(d) mkdir(d, 0755)
#define VJOIN(buf, sz, name) snprintf(buf, sz, "vendor/%s", name)
#define POPEN popen
#define PCLOSE pclose
#define PATH_EXISTS(p) (access((p), F_OK) == 0)
#endif

#define RIZ_PKG_JSON "riz.json"
#define RIZ_PKG_DEPS "riz.deps"
#define RIZ_PKG_LOCK "riz.lock"
#define RIZ_PKG_INDEX_DEFAULT "packages.index"
#define MAX_DEPS 128

typedef struct {
    char name[256];
    char kind[16];
    char spec[1536];
    char hash[64];
} LockRow;

typedef struct {
    char name[256];
    char spec[1536];
} DepEntry;

static int git_rev_parse_head(const char* repo, char* out, size_t cap);

static void ensure_dir(const char* path);

static bool is_http_url(const char* s) {
    if (!s || !s[0])
        return false;
    return strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0;
}

static unsigned url_hash32(const char* url) {
    unsigned h = 5381u;
    for (const unsigned char* p = (const unsigned char*)url; *p; p++)
        h = ((h << 5u) + h) + (unsigned)*p;
    return h;
}

static int fetch_url_curl(const char* url, const char* dest) {
    char cmd[5200];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "curl -sL -f -o \"%s\" \"%s\" >nul 2>&1", dest, url);
#else
    snprintf(cmd, sizeof(cmd), "curl -sL -f -o \"%s\" \"%s\" >/dev/null 2>&1", dest, url);
#endif
    return system(cmd) == 0 ? 0 : 1;
}

#ifndef _WIN32
static int fetch_url_wget(const char* url, const char* dest) {
    char cmd[5200];
    snprintf(cmd, sizeof(cmd), "wget -q -O \"%s\" \"%s\" >/dev/null 2>&1", dest, url);
    return system(cmd) == 0 ? 0 : 1;
}
#else
static int fetch_url_powershell(const char* url, const char* dest) {
    if (!SetEnvironmentVariableA("RIZ_PKG_FETCH_URL", url))
        return 1;
    if (!SetEnvironmentVariableA("RIZ_PKG_FETCH_DEST", dest))
        return 1;
    int st = system(
        "powershell -NoProfile -NonInteractive -Command "
        "\"try { Invoke-WebRequest -Uri $env:RIZ_PKG_FETCH_URL -OutFile $env:RIZ_PKG_FETCH_DEST "
        "-UseBasicParsing } catch { exit 1 }\"");
    SetEnvironmentVariableA("RIZ_PKG_FETCH_URL", NULL);
    SetEnvironmentVariableA("RIZ_PKG_FETCH_DEST", NULL);
    return st == 0 ? 0 : 1;
}
#endif

/* Returns false on failure (download error). */
static bool resolve_index_to_local(const char* configured, char* out, size_t cap) {
    if (!configured || !configured[0]) {
        strncpy(out, RIZ_PKG_INDEX_DEFAULT, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
    if (!is_http_url(configured)) {
        strncpy(out, configured, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }

    ensure_dir(".riz");
#ifdef _WIN32
    ensure_dir(".riz\\cache");
    char cachepath[1100];
    snprintf(cachepath, sizeof(cachepath), ".riz\\cache\\idx_%08x.txt", url_hash32(configured));
#else
    ensure_dir(".riz/cache");
    char cachepath[1100];
    snprintf(cachepath, sizeof(cachepath), ".riz/cache/idx_%08x.txt", url_hash32(configured));
#endif

    bool refresh = getenv("RIZ_PACKAGE_INDEX_REFRESH") != NULL && getenv("RIZ_PACKAGE_INDEX_REFRESH")[0];
    bool need = refresh;
    if (!need) {
        FILE* t = fopen(cachepath, "rb");
        if (!t)
            need = true;
        else {
            if (fseek(t, 0, SEEK_END) != 0) {
                fclose(t);
                need = true;
            } else {
                long sz = ftell(t);
                fclose(t);
                need = (sz <= 0);
            }
        }
    }
    if (need) {
        int ok = fetch_url_curl(configured, cachepath) == 0;
#ifndef _WIN32
        if (!ok)
            ok = fetch_url_wget(configured, cachepath) == 0;
#else
        if (!ok)
            ok = fetch_url_powershell(configured, cachepath) == 0;
#endif
        if (!ok) {
            fprintf(stderr,
                    "pkg: could not download package index (network fetch failed). "
                    "Install curl or wget, set RIZ_PACKAGE_INDEX to a local file, or fix the URL.\n");
            return false;
        }
    }
    strncpy(out, cachepath, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

static void print_usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  riz pkg init [name]       Create %s, %s, %s, vendor/\n"
        "  riz pkg add <n> [spec]    Add dep (spec or %s / URL + .riz/cache)\n"
        "  riz pkg install [--locked]  Resolve deps; --locked = verify %s, no network/copy\n"
        "  riz pkg update            Re-fetch only git dependencies\n"
        "  riz pkg sync              Write merged deps into %s \"dependencies\"\n"
        "  riz pkg tree              Print merged dependency list\n",
        RIZ_PKG_JSON, RIZ_PKG_DEPS, RIZ_PKG_INDEX_DEFAULT, RIZ_PKG_INDEX_DEFAULT, RIZ_PKG_LOCK, RIZ_PKG_JSON);
}

static void trim_crlf(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void ensure_dir(const char* path) { MKDIR(path); }

static bool path_is_file(const char* p) {
    FILE* f = fopen(p, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

static char* read_file_all(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len)
        *out_len = rd;
    return buf;
}

static const char* skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static bool parse_json_string(const char** pp, char* out, size_t cap) {
    const char* p = skip_ws(*pp);
    if (*p != '"')
        return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (i + 1 >= cap)
                return false;
            out[i++] = *p++;
            continue;
        }
        if (i + 1 >= cap)
            return false;
        out[i++] = *p++;
    }
    if (*p != '"')
        return false;
    p++;
    out[i] = '\0';
    *pp = p;
    return true;
}

static void config_package_index_path(char* out, size_t cap) {
    const char* ev = getenv("RIZ_PACKAGE_INDEX");
    if (ev && ev[0]) {
        strncpy(out, ev, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    char* j = read_file_all(RIZ_PKG_JSON, NULL);
    if (!j) {
        strncpy(out, RIZ_PKG_INDEX_DEFAULT, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    const char* p = strstr(j, "\"packageIndex\"");
    if (!p) {
        free(j);
        strncpy(out, RIZ_PKG_INDEX_DEFAULT, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        free(j);
        strncpy(out, RIZ_PKG_INDEX_DEFAULT, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    p++;
    p = skip_ws(p);
    if (!parse_json_string(&p, out, cap)) {
        free(j);
        strncpy(out, RIZ_PKG_INDEX_DEFAULT, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    free(j);
}

/* 0 = found */
static int lookup_index_spec(const char* name, char* spec_out, size_t cap) {
    char configured[1024];
    char idxpath[1200];
    config_package_index_path(configured, sizeof(configured));
    if (!resolve_index_to_local(configured, idxpath, sizeof(idxpath)))
        return 1;
    FILE* f = fopen(idxpath, "rb");
    if (!f)
        return 1;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char* tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        trim_crlf(line);
        char* sp = tab + 1;
        trim_crlf(sp);
        if (strcmp(line, name) != 0)
            continue;
        fclose(f);
        strncpy(spec_out, sp, cap - 1);
        spec_out[cap - 1] = '\0';
        return 0;
    }
    fclose(f);
    return 1;
}

static bool split_lock_line(char* s, LockRow* row) {
    memset(row, 0, sizeof(*row));
    char* a = s;
    char* t = strchr(a, '\t');
    if (!t)
        return false;
    *t = '\0';
    strncpy(row->name, a, sizeof(row->name) - 1);
    a = t + 1;
    t = strchr(a, '\t');
    if (!t)
        return false;
    *t = '\0';
    strncpy(row->kind, a, sizeof(row->kind) - 1);
    a = t + 1;
    t = strchr(a, '\t');
    if (!t)
        return false;
    *t = '\0';
    strncpy(row->spec, a, sizeof(row->spec) - 1);
    a = t + 1;
    trim_crlf(a);
    strncpy(row->hash, a, sizeof(row->hash) - 1);
    return row->name[0] != '\0';
}

static int read_lock_rows(LockRow* rows, int maxr) {
    FILE* f = fopen(RIZ_PKG_LOCK, "rb");
    if (!f)
        return -1;
    int n = 0;
    char line[4096];
    while (n < maxr && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char buf[4096];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        trim_crlf(buf);
        LockRow tmp;
        if (!split_lock_line(buf, &tmp))
            continue;
        rows[n++] = tmp;
    }
    fclose(f);
    return n;
}

static bool vendor_pkg_exists(const char* pkg) {
    char p[1200];
    VJOIN(p, sizeof(p), pkg);
    return PATH_EXISTS(p);
}

static bool validate_locked_state(DepEntry* deps, int dep_n) {
    LockRow rows[MAX_DEPS];
    int nr = read_lock_rows(rows, MAX_DEPS);
    if (nr < 0) {
        fprintf(stderr, "pkg: --locked requires %s (run install without --locked first)\n", RIZ_PKG_LOCK);
        return false;
    }
    for (int i = 0; i < dep_n; i++) {
        int j = -1;
        for (int k = 0; k < nr; k++) {
            if (strcmp(rows[k].name, deps[i].name) == 0) {
                j = k;
                break;
            }
        }
        if (j < 0) {
            fprintf(stderr, "pkg: --locked: %s has no entry for '%s'\n", RIZ_PKG_LOCK, deps[i].name);
            return false;
        }
        if (strcmp(rows[j].spec, deps[i].spec) != 0) {
            fprintf(stderr, "pkg: --locked: spec mismatch for '%s' (lock vs manifest)\n", deps[i].name);
            return false;
        }
    }
    for (int i = 0; i < dep_n; i++) {
        if (!vendor_pkg_exists(deps[i].name)) {
            fprintf(stderr, "pkg: --locked: missing vendor directory for '%s'\n", deps[i].name);
            return false;
        }
        int j = -1;
        for (int k = 0; k < nr; k++) {
            if (strcmp(rows[k].name, deps[i].name) == 0) {
                j = k;
                break;
            }
        }
        if (strcmp(rows[j].kind, "git") == 0 && strcmp(rows[j].hash, "unknown") != 0) {
            char dst[1200];
            VJOIN(dst, sizeof(dst), deps[i].name);
            char cur[64];
            cur[0] = '\0';
            if (git_rev_parse_head(dst, cur, sizeof(cur)) != 0) {
                fprintf(stderr, "pkg: --locked: cannot read git HEAD in vendor/%s\n", deps[i].name);
                return false;
            }
            if (strcmp(cur, rows[j].hash) != 0) {
                fprintf(stderr, "pkg: --locked: git HEAD mismatch for '%s'\n", deps[i].name);
                return false;
            }
        }
    }
    return true;
}

static void json_escape_fwrite(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static const char* match_brace_close(const char* lbrace) {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (const char* p = lbrace; *p; p++) {
        if (in_str) {
            if (esc) {
                esc = false;
                continue;
            }
            if (*p == '\\') {
                esc = true;
                continue;
            }
            if (*p == '"')
                in_str = false;
            continue;
        }
        if (*p == '"') {
            in_str = true;
            continue;
        }
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static bool parse_deps_object_inner(const char** pp, DepEntry* deps, int* ndeps, int maxd) {
    const char* p = skip_ws(*pp);
    if (*p != '{')
        return false;
    p++;
    while (1) {
        p = skip_ws(p);
        if (*p == '}') {
            p++;
            *pp = p;
            return true;
        }
        char key[256], val[1536];
        if (!parse_json_string(&p, key, sizeof(key)))
            return false;
        p = skip_ws(p);
        if (*p != ':')
            return false;
        p++;
        if (!parse_json_string(&p, val, sizeof(val)))
            return false;
        if (*ndeps < maxd) {
            strncpy(deps[*ndeps].name, key, sizeof(deps[*ndeps].name) - 1);
            deps[*ndeps].name[sizeof(deps[*ndeps].name) - 1] = '\0';
            strncpy(deps[*ndeps].spec, val, sizeof(deps[*ndeps].spec) - 1);
            deps[*ndeps].spec[sizeof(deps[*ndeps].spec) - 1] = '\0';
            (*ndeps)++;
        }
        p = skip_ws(p);
        if (*p == ',')
            p++;
    }
}

static bool parse_riz_json_dependencies(const char* json, DepEntry* deps, int* ndeps, int maxdeps) {
    *ndeps = 0;
    if (!json)
        return false;
    const char* d = strstr(json, "\"dependencies\"");
    if (!d)
        return true;
    d = strchr(d, ':');
    if (!d)
        return false;
    d++;
    d = skip_ws(d);
    return parse_deps_object_inner(&d, deps, ndeps, maxdeps);
}

static void dep_put(DepEntry* deps, int* ndeps, int maxdeps, const char* name, const char* spec) {
    for (int i = 0; i < *ndeps; i++) {
        if (strcmp(deps[i].name, name) == 0) {
            strncpy(deps[i].spec, spec, sizeof(deps[i].spec) - 1);
            deps[i].spec[sizeof(deps[i].spec) - 1] = '\0';
            return;
        }
    }
    if (*ndeps >= maxdeps)
        return;
    strncpy(deps[*ndeps].name, name, sizeof(deps[*ndeps].name) - 1);
    deps[*ndeps].name[sizeof(deps[*ndeps].name) - 1] = '\0';
    strncpy(deps[*ndeps].spec, spec, sizeof(deps[*ndeps].spec) - 1);
    deps[*ndeps].spec[sizeof(deps[*ndeps].spec) - 1] = '\0';
    (*ndeps)++;
}

/* Merge: base from riz.json, then riz.deps lines override (local dev overrides). */
static int merge_deps_from_disk(DepEntry* deps, int maxdeps) {
    int n = 0;
    char* j = read_file_all(RIZ_PKG_JSON, NULL);
    if (j) {
        parse_riz_json_dependencies(j, deps, &n, maxdeps);
        free(j);
    }

    FILE* f = fopen(RIZ_PKG_DEPS, "rb");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            char* tab = strchr(line, '\t');
            if (!tab)
                continue;
            *tab = '\0';
            trim_crlf(line);
            char spec[1536];
            strncpy(spec, tab + 1, sizeof(spec) - 1);
            spec[sizeof(spec) - 1] = '\0';
            trim_crlf(spec);
            if (line[0] && spec[0])
                dep_put(deps, &n, maxdeps, line, spec);
        }
        fclose(f);
    }
    return n;
}

static const char* strip_path_prefix(const char* spec) {
    if (strncmp(spec, "path:", 5) == 0)
        return spec + 5;
    return spec;
}

static bool is_git_spec(const char* spec) {
    const char* s = skip_ws(spec);
    if (strncmp(s, "git+", 4) == 0)
        return true;
    if (strncmp(s, "git@", 4) == 0)
        return true;
    if (strstr(s, "://") != NULL)
        return true;
    return false;
}

static void parse_git_parts(const char* spec, char* url, size_t ucap, char* ref, size_t rcap) {
    const char* s = skip_ws(spec);
    if (strncmp(s, "git+", 4) == 0)
        s += 4;
    ref[0] = '\0';
    const char* hash = strrchr(s, '#');
    if (hash && hash > s) {
        size_t ulen = (size_t)(hash - s);
        if (ulen >= ucap)
            ulen = ucap - 1;
        memcpy(url, s, ulen);
        url[ulen] = '\0';
        strncpy(ref, hash + 1, rcap - 1);
        ref[rcap - 1] = '\0';
        trim_crlf(url);
        trim_crlf(ref);
    } else {
        strncpy(url, s, ucap - 1);
        url[ucap - 1] = '\0';
        trim_crlf(url);
    }
}

static int copy_one_file(const char* src, const char* dst_path) {
#ifdef _WIN32
    char cmd[3072];
    snprintf(cmd, sizeof(cmd), "cmd /C copy /Y \"%s\" \"%s\" >nul 2>&1", src, dst_path);
#else
    char cmd[3072];
    snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\" 2>/dev/null", src, dst_path);
#endif
    return system(cmd);
}

static int copy_tree_cmd(const char* src, const char* dst) {
    ensure_dir(dst);
    const char* psrc = strip_path_prefix(src);
    if (path_is_file(psrc)) {
        const char* base = psrc;
        const char* s = strrchr(psrc, '/');
        if (!s)
            s = strrchr(psrc, '\\');
        if (s)
            base = s + 1;
        char dstfile[1200];
#ifdef _WIN32
        snprintf(dstfile, sizeof(dstfile), "%s\\%s", dst, base);
#else
        snprintf(dstfile, sizeof(dstfile), "%s/%s", dst, base);
#endif
        return copy_one_file(psrc, dstfile);
    }
#ifdef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cmd /C xcopy /E /I /Y \"%s\\*\" \"%s\\\" >nul 2>&1", psrc, dst);
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cp -R \"%s/.\" \"%s/\" 2>/dev/null", psrc, dst);
#endif
    return system(cmd);
}

static int rmdir_recursive(const char* path) {
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "cmd /C if exist \"%s\" rd /s /q \"%s\" >nul 2>&1", path, path);
#else
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>/dev/null", path);
#endif
    return system(cmd);
}

static int git_rev_parse_head(const char* repo, char* out, size_t cap) {
    char cmd[1600];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse HEAD 2>nul", repo);
#else
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse HEAD 2>/dev/null", repo);
#endif
    FILE* pf = POPEN(cmd, "r");
    if (!pf)
        return 1;
    if (!fgets(out, (int)cap, pf)) {
        PCLOSE(pf);
        return 1;
    }
    PCLOSE(pf);
    trim_crlf(out);
    return 0;
}

static int install_git_dep(const char* name, const char* spec) {
    char url[1200], ref[256];
    parse_git_parts(spec, url, sizeof(url), ref, sizeof(ref));
    char dst[1024];
    VJOIN(dst, sizeof(dst), name);

    ensure_dir("vendor");
    rmdir_recursive(dst);

    char clone[4096];
    if (ref[0])
        snprintf(clone, sizeof(clone), "git clone --depth 1 -b \"%s\" \"%s\" \"%s\"", ref, url, dst);
    else
        snprintf(clone, sizeof(clone), "git clone --depth 1 \"%s\" \"%s\"", url, dst);
#ifdef _WIN32
    {
        char wrap[4500];
        snprintf(wrap, sizeof(wrap), "cmd /C %s >nul 2>&1", clone);
        if (system(wrap) != 0) {
            fprintf(stderr, "pkg: git clone failed for '%s' (install Git? check URL/ref)\n", name);
            return 1;
        }
    }
#else
    {
        char wrap[4500];
        snprintf(wrap, sizeof(wrap), "%s >/dev/null 2>&1", clone);
        if (system(wrap) != 0) {
            fprintf(stderr, "pkg: git clone failed for '%s' (install Git? check URL/ref)\n", name);
            return 1;
        }
    }
#endif
    return 0;
}

static int install_path_dep(const char* name, const char* spec) {
    char dst[1024];
    VJOIN(dst, sizeof(dst), name);
    ensure_dir("vendor");
    const char* src = strip_path_prefix(spec);
    if (copy_tree_cmd(src, dst) != 0) {
        fprintf(stderr, "pkg: copy failed for '%s'\n", name);
        return 1;
    }
    return 0;
}

static void write_lock_file(DepEntry* deps, int n) {
    FILE* out = fopen(RIZ_PKG_LOCK, "wb");
    if (!out)
        return;
    fprintf(out, "# riz-lock v1 — generated by riz pkg install/update\n");
    for (int i = 0; i < n; i++) {
        char dst[1200];
        VJOIN(dst, sizeof(dst), deps[i].name);
        if (is_git_spec(deps[i].spec)) {
            char hash[64];
            hash[0] = '\0';
            if (git_rev_parse_head(dst, hash, sizeof(hash)) != 0)
                strncpy(hash, "unknown", sizeof(hash) - 1);
            fprintf(out, "%s\tgit\t%s\t%s\n", deps[i].name, deps[i].spec, hash);
        } else {
            fprintf(out, "%s\tpath\t%s\tlocal\n", deps[i].name, deps[i].spec);
        }
    }
    fclose(out);
}

static bool find_dependencies_lbrace(const char* json, const char** out_lbrace) {
    const char* d = strstr(json, "\"dependencies\"");
    if (!d)
        return false;
    d = strchr(d, '{');
    if (!d)
        return false;
    *out_lbrace = d;
    return true;
}

static int rewrite_riz_json_dependencies_inner(const DepEntry* deps, int n) {
    char* json = read_file_all(RIZ_PKG_JSON, NULL);
    if (!json) {
        fprintf(stderr, "pkg: %s not found\n", RIZ_PKG_JSON);
        return 1;
    }
    const char* lbrace = NULL;
    if (!find_dependencies_lbrace(json, &lbrace)) {
        /* Inject "dependencies": { } before root closing brace */
        const char* root0 = strchr(json, '{');
        if (!root0) {
            free(json);
            return 1;
        }
        const char* root_close = match_brace_close(root0);
        if (!root_close) {
            free(json);
            return 1;
        }
        const char* q = root_close;
        while (q > json && isspace((unsigned char)q[-1]))
            q--;
        bool need_comma = (q > json && q[-1] != '{' && q[-1] != ',');
        size_t prefix_len = (size_t)(root_close - json);
        FILE* f = fopen(RIZ_PKG_JSON, "wb");
        if (!f) {
            free(json);
            return 1;
        }
        fwrite(json, 1, prefix_len, f);
        if (need_comma)
            fprintf(f, ",");
        fprintf(f, "\n  \"dependencies\": {\n");
        for (int i = 0; i < n; i++) {
            fprintf(f, "    ");
            json_escape_fwrite(f, deps[i].name);
            fprintf(f, ": ");
            json_escape_fwrite(f, deps[i].spec);
            if (i + 1 < n)
                fprintf(f, ",\n");
            else
                fprintf(f, "\n");
        }
        fprintf(f, "  }\n");
        fwrite(root_close, 1, strlen(root_close), f);
        fclose(f);
        free(json);
        return 0;
    }

    const char* rbrace = match_brace_close(lbrace);
    if (!rbrace) {
        free(json);
        fprintf(stderr, "pkg: malformed %s (dependencies brace)\n", RIZ_PKG_JSON);
        return 1;
    }

    FILE* f = fopen(RIZ_PKG_JSON, "wb");
    if (!f) {
        free(json);
        return 1;
    }
    fwrite(json, 1, (size_t)(lbrace - json) + 1, f);
    if (n > 0) {
        fprintf(f, "\n");
        for (int i = 0; i < n; i++) {
            fprintf(f, "    ");
            json_escape_fwrite(f, deps[i].name);
            fprintf(f, ": ");
            json_escape_fwrite(f, deps[i].spec);
            if (i + 1 < n)
                fprintf(f, ",\n");
            else
                fprintf(f, "\n");
        }
        fprintf(f, "  ");
    }
    fwrite(rbrace, 1, strlen(rbrace), f);
    fclose(f);
    free(json);
    return 0;
}

static int write_riz_json(const char* name, const char* version) {
    FILE* f = fopen(RIZ_PKG_JSON, "wb");
    if (!f) {
        fprintf(stderr, "pkg: cannot write %s\n", RIZ_PKG_JSON);
        return 1;
    }
    fprintf(f,
            "{\n"
            "  \"name\": \"%s\",\n"
            "  \"version\": \"%s\",\n"
            "  \"description\": \"Riz project (see %s and dependencies)\",\n"
            "  \"packageIndex\": \"%s\",\n"
            "  \"dependencies\": {\n"
            "  }\n"
            "}\n",
            name, version, RIZ_PKG_DEPS, RIZ_PKG_INDEX_DEFAULT);
    fclose(f);
    return 0;
}

static int append_dep_line(const char* name, const char* srcpath) {
    FILE* f = fopen(RIZ_PKG_DEPS, "ab");
    if (!f) {
        fprintf(stderr, "pkg: cannot append %s\n", RIZ_PKG_DEPS);
        return 1;
    }
    fprintf(f, "%s\t%s\n", name, srcpath);
    fclose(f);
    return 0;
}

static int cmd_init(int argc, char** argv) {
    const char* name = "myriz";
    if (argc >= 1 && argv[0] && argv[0][0])
        name = argv[0];

    if (write_riz_json(name, RIZ_VERSION))
        return 1;

    FILE* f = fopen(RIZ_PKG_DEPS, "wb");
    if (!f) {
        fprintf(stderr, "pkg: cannot write %s\n", RIZ_PKG_DEPS);
        return 1;
    }
    fprintf(f, "# name<TAB>spec — spec: local path, path:C:\\..., or git+https://...#branch\n");
    fprintf(f, "# riz.deps overrides same package name from %s when merging\n", RIZ_PKG_JSON);
    fclose(f);

    FILE* idx = fopen(RIZ_PKG_INDEX_DEFAULT, "wb");
    if (idx) {
        fprintf(idx, "# name<TAB>git-url-or-path — used by: riz pkg add <name>\n");
        fprintf(idx, "# packageIndex: local file, or https://... (cached under .riz/cache/)\n");
        fprintf(idx, "# Env: RIZ_PACKAGE_INDEX, RIZ_PACKAGE_INDEX_REFRESH=1 to force re-fetch\n");
        fclose(idx);
    }

    ensure_dir("vendor");
    printf("Created %s, %s, %s, and vendor/\n", RIZ_PKG_JSON, RIZ_PKG_DEPS, RIZ_PKG_INDEX_DEFAULT);
    return 0;
}

static int cmd_add(int argc, char** argv) {
    if (argc < 1) {
        fprintf(stderr, "pkg: need <name> [path|git-spec]\n");
        return 1;
    }
    const char* name = argv[0];
    char spec_buf[1536];
    const char* spec;
    if (argc == 1) {
        if (lookup_index_spec(name, spec_buf, sizeof(spec_buf)) != 0) {
            fprintf(stderr,
                    "pkg: no index entry for '%s' (edit %s or set RIZ_PACKAGE_INDEX)\n",
                    name, RIZ_PKG_INDEX_DEFAULT);
            return 1;
        }
        spec = spec_buf;
    } else {
        spec = argv[1];
    }
    char dst[1024];
    VJOIN(dst, sizeof(dst), name);

    ensure_dir("vendor");
    int err;
    if (is_git_spec(spec))
        err = install_git_dep(name, spec);
    else
        err = install_path_dep(name, spec);
    if (err)
        return 1;

    if (append_dep_line(name, spec))
        return 1;

    DepEntry merged[MAX_DEPS];
    int nm = merge_deps_from_disk(merged, MAX_DEPS);
    FILE* jf = fopen(RIZ_PKG_JSON, "rb");
    if (jf) {
        fclose(jf);
        rewrite_riz_json_dependencies_inner(merged, nm);
    }

    printf("Added package '%s' -> %s\n", name, dst);
    return 0;
}

static int run_install(int git_only, bool locked) {
    DepEntry deps[MAX_DEPS];
    int n = merge_deps_from_disk(deps, MAX_DEPS);
    if (n == 0) {
        fprintf(stderr, "pkg: no dependencies in %s or %s\n", RIZ_PKG_JSON, RIZ_PKG_DEPS);
        return 1;
    }

    if (locked) {
        if (!validate_locked_state(deps, n))
            return 1;
        printf("pkg: --locked: vendor matches %s (skipping fetch/copy)\n", RIZ_PKG_LOCK);
        write_lock_file(deps, n);
        return 0;
    }

    ensure_dir("vendor");
    int errors = 0;
    for (int i = 0; i < n; i++) {
        printf("install: %s <- %s\n", deps[i].name, deps[i].spec);
        if (git_only && !is_git_spec(deps[i].spec))
            continue;
        if (is_git_spec(deps[i].spec)) {
            if (install_git_dep(deps[i].name, deps[i].spec) != 0)
                errors++;
        } else {
            if (install_path_dep(deps[i].name, deps[i].spec) != 0)
                errors++;
        }
    }

    if (!errors)
        write_lock_file(deps, n);
    else
        fprintf(stderr, "pkg: %d step(s) failed; lock file not updated\n", errors);

    return errors ? 1 : 0;
}

static int cmd_install(int argc, char** argv) {
    bool locked = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--locked") == 0)
            locked = true;
    }
    bool have = false;
    FILE* df = fopen(RIZ_PKG_DEPS, "rb");
    if (df) {
        fclose(df);
        have = true;
    }
    FILE* jf = fopen(RIZ_PKG_JSON, "rb");
    if (jf) {
        fclose(jf);
        have = true;
    }
    if (!have) {
        fprintf(stderr, "pkg: need %s or %s (run: riz pkg init)\n", RIZ_PKG_DEPS, RIZ_PKG_JSON);
        return 1;
    }
    return run_install(0, locked);
}

static int cmd_update(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return run_install(1, false);
}

static int cmd_sync(int argc, char** argv) {
    (void)argc;
    (void)argv;
    DepEntry deps[MAX_DEPS];
    int n = merge_deps_from_disk(deps, MAX_DEPS);
    FILE* jchk = fopen(RIZ_PKG_JSON, "rb");
    if (!jchk) {
        fprintf(stderr, "pkg: %s not found\n", RIZ_PKG_JSON);
        return 1;
    }
    fclose(jchk);
    if (rewrite_riz_json_dependencies_inner(deps, n))
        return 1;
    printf("Synced %d entr%s into %s \"dependencies\"\n", n, n == 1 ? "y" : "ies", RIZ_PKG_JSON);
    return 0;
}

static int cmd_tree(int argc, char** argv) {
    (void)argc;
    (void)argv;
    DepEntry deps[MAX_DEPS];
    int n = merge_deps_from_disk(deps, MAX_DEPS);
    if (n == 0) {
        printf("(no dependencies)\n");
        return 0;
    }
    for (int i = 0; i < n; i++)
        printf("  %s  [%s]  %s\n", deps[i].name, is_git_spec(deps[i].spec) ? "git" : "path", deps[i].spec);
    return 0;
}

int riz_pkg_main(int argc, char** argv) {
    if (argc < 1) {
        print_usage();
        return 1;
    }
    if (strcmp(argv[0], "init") == 0)
        return cmd_init(argc - 1, argv + 1);
    if (strcmp(argv[0], "add") == 0)
        return cmd_add(argc - 1, argv + 1);
    if (strcmp(argv[0], "install") == 0)
        return cmd_install(argc - 1, argv + 1);
    if (strcmp(argv[0], "update") == 0)
        return cmd_update(argc - 1, argv + 1);
    if (strcmp(argv[0], "sync") == 0)
        return cmd_sync(argc - 1, argv + 1);
    if (strcmp(argv[0], "tree") == 0)
        return cmd_tree(argc - 1, argv + 1);
    if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        print_usage();
        return 0;
    }

    fprintf(stderr, "pkg: unknown subcommand '%s'\n", argv[0]);
    print_usage();
    return 1;
}
