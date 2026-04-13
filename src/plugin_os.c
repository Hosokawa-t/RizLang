#include "riz_plugin.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static RizPluginAPI* G_API;

static RizPluginValue os_getenv(RizPluginValue* args, int argc) {
    if (args[0].type != VAL_STRING) return G_API->make_none();
    const char* val = getenv(args[0].as.string);
    if (!val) return G_API->make_none();
    return G_API->make_string(val);
}

static RizPluginValue os_system(RizPluginValue* args, int argc) {
    if (args[0].type != VAL_STRING) return G_API->make_int(-1);
    int ret = system(args[0].as.string);
    return G_API->make_int(ret);
}

static RizPluginValue os_cwd(RizPluginValue* args, int argc) {
    char buf[1024];
#ifdef _WIN32
    if (!GetCurrentDirectoryA(sizeof(buf), buf)) {
        return G_API->make_none();
    }
#else
    if (!getcwd(buf, sizeof(buf))) {
        return G_API->make_none();
    }
#endif
    return G_API->make_string(buf);
}

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G_API = api;
    RizPluginValue os_dict = api->make_dict();
    api->dict_set_fn(os_dict, "getenv", "os.getenv", os_getenv, 1);
    api->dict_set_fn(os_dict, "system", "os.system", os_system, 1);
    api->dict_set_fn(os_dict, "cwd", "os.cwd", os_cwd, 0);

    api->define_global(api->interp, "os", os_dict);
}
