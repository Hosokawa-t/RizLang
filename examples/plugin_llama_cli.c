/*
 * Riz plugin — llama.cpp CLI bridge (GGUF inference)
 *
 * Invokes a user-built llama.cpp `llama-cli` (or compatible) with -m / -f / -n.
 * Uses popen (stdout only). No llama.cpp code is linked; binary on PATH or LLAMA_CLI.
 *
 * Build (MinGW / MSYS2, from repo root):
 *   gcc -shared -O2 -Isrc -o plugin_llama_cli.dll examples/plugin_llama_cli.c
 *
 * See: examples/LLAMA_INFER.md
 */

#include "riz_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define popen  _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

static RizPluginAPI G;

/* Executable: default "llama-cli"; override with env LLAMA_CLI or llama_set_cli(). */
static char g_llama_exe[1024] = "llama-cli";

static const char* arg_str(RizPluginValue v) {
    if (v.type == VAL_STRING && v.as.string) return v.as.string;
    return NULL;
}

static int64_t arg_int(RizPluginValue v) {
    if (v.type == VAL_INT) return v.as.integer;
    if (v.type == VAL_FLOAT) return (int64_t)v.as.floating;
    return 64;
}

/* Quote for cmd.exe only when needed (avoids "llama-cli" mis-parse on Windows). */
static void append_shell_arg(char* cmd, size_t* pos, size_t cap, const char* s) {
    int need = (strpbrk(s, " \t&()^<>|") != NULL);
    size_t p = *pos;
    if (p > 0 && p < cap) cmd[p++] = ' ';
    if (need && p < cap) cmd[p++] = '"';
    for (const char* c = s; *c && p < cap; c++) cmd[p++] = *c;
    if (need && p < cap) cmd[p++] = '"';
    if (p >= cap) p = cap - 1;
    cmd[p] = '\0';
    *pos = p;
}

#ifdef _WIN32
static int make_temp_prompt_file(const char* utf8_prompt, char* path_out, size_t path_sz) {
    char dir[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof(dir), dir) == 0) return -1;
    if (!GetTempFileNameA(dir, "rzl", 0, path_out) || path_sz <= strlen(path_out) + 1)
        return -1;
    FILE* f = fopen(path_out, "wb");
    if (!f) {
        DeleteFileA(path_out);
        return -1;
    }
    size_t n = strlen(utf8_prompt);
    if (fwrite(utf8_prompt, 1, n, f) != n) {
        fclose(f);
        DeleteFileA(path_out);
        return -1;
    }
    fclose(f);
    return 0;
}

static void rm_if_exists(const char* p) {
    DeleteFileA(p);
}
#else
static int make_temp_prompt_file(const char* utf8_prompt, char* path_out, size_t path_sz) {
    (void)path_sz;
    char tmpl[] = "/tmp/riz_llama_inXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    size_t n = strlen(utf8_prompt);
    if (write(fd, utf8_prompt, n) != (ssize_t)n) {
        close(fd);
        unlink(tmpl);
        return -1;
    }
    close(fd);
    strncpy(path_out, tmpl, path_sz - 1);
    path_out[path_sz - 1] = '\0';
    return 0;
}

static void rm_if_exists(const char* p) {
    unlink(p);
}
#endif

/* Read all bytes from a popen stream into a malloc'd string (max ~8 MiB). */
static char* read_popen_all(FILE* fp, size_t* out_len) {
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t room = cap - len;
        if (room < 2) {
            cap *= 2;
            if (cap > 8u * 1024u * 1024u) {
                free(buf);
                return NULL;
            }
            char* nb = (char*)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            room = cap - len;
        }
        size_t rd = fread(buf + len, 1, room - 1, fp);
        len += rd;
        buf[len] = '\0';
        if (rd == 0)
            break;
    }
    if (out_len) *out_len = len;
    return buf;
}

/* llama_set_cli(path) — full path to llama-cli if not on PATH */
static RizPluginValue native_llama_set_cli(RizPluginValue* args, int argc) {
    (void)argc;
    const char* s = arg_str(args[0]);
    if (!s || !s[0]) return G.make_bool(false);
    strncpy(g_llama_exe, s, sizeof(g_llama_exe) - 1);
    g_llama_exe[sizeof(g_llama_exe) - 1] = '\0';
    return G.make_bool(true);
}

/*
 * llama_infer(model_path, prompt, max_tokens) -> string
 * Runs: llama-cli -m MODEL -f PROMPT_FILE -n N
 */
static RizPluginValue native_llama_infer(RizPluginValue* args, int argc) {
    if (argc < 3) return G.make_string("[llama_infer] need (model_path, prompt, max_tokens)");

    const char* model = arg_str(args[0]);
    const char* prompt = arg_str(args[1]);
    int64_t max_tok = arg_int(args[2]);
    if (!model || !model[0]) return G.make_string("[llama_infer] empty model_path");
    if (!prompt) return G.make_string("[llama_infer] empty prompt");
    if (max_tok < 1) max_tok = 1;
    if (max_tok > 131072) max_tok = 131072;

    const char* env_cli = getenv("LLAMA_CLI");
    if (env_cli && env_cli[0]) {
        strncpy(g_llama_exe, env_cli, sizeof(g_llama_exe) - 1);
        g_llama_exe[sizeof(g_llama_exe) - 1] = '\0';
    }

    char prompt_path[1024];
    if (make_temp_prompt_file(prompt, prompt_path, sizeof(prompt_path)) != 0)
        return G.make_string("[llama_infer] could not create temp prompt file");

    char cmd[16384];
    size_t pos = 0;
    cmd[0] = '\0';
    append_shell_arg(cmd, &pos, sizeof(cmd), g_llama_exe);
    append_shell_arg(cmd, &pos, sizeof(cmd), "-m");
    append_shell_arg(cmd, &pos, sizeof(cmd), model);
    append_shell_arg(cmd, &pos, sizeof(cmd), "-f");
    append_shell_arg(cmd, &pos, sizeof(cmd), prompt_path);
    append_shell_arg(cmd, &pos, sizeof(cmd), "-n");
    {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%lld", (long long)max_tok);
        append_shell_arg(cmd, &pos, sizeof(cmd), nbuf);
    }

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        rm_if_exists(prompt_path);
        return G.make_string("[llama_infer] popen failed (is llama-cli on PATH?)");
    }

    size_t out_len = 0;
    char* body = read_popen_all(fp, &out_len);
    int rc = pclose(fp);
    rm_if_exists(prompt_path);
    (void)rc;

    if (!body || out_len == 0) {
        if (body) free(body);
        char err[512];
        snprintf(err, sizeof(err),
                 "[llama_infer] empty stdout (exit %d). Check model path and llama-cli.\n",
                 rc);
        return G.make_string(err);
    }

    RizPluginValue ret = G.make_string(body);
    free(body);
    return ret;
}

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;
#ifdef _WIN32
    {
        const char* env_cli = getenv("LLAMA_CLI");
        if (env_cli && env_cli[0]) {
            strncpy(g_llama_exe, env_cli, sizeof(g_llama_exe) - 1);
            g_llama_exe[sizeof(g_llama_exe) - 1] = '\0';
        }
    }
#else
    {
        const char* env_cli = getenv("LLAMA_CLI");
        if (env_cli && env_cli[0]) {
            strncpy(g_llama_exe, env_cli, sizeof(g_llama_exe) - 1);
            g_llama_exe[sizeof(g_llama_exe) - 1] = '\0';
        }
    }
#endif

    api->register_fn(api->interp, "llama_set_cli", native_llama_set_cli, 1);
    api->register_fn(api->interp, "llama_infer",  native_llama_infer,  3);
}
