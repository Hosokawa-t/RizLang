#include "riz_plugin.h"
#include <math.h>
#include <stdlib.h>

/* Extract numeric value */
static double get_num(RizPluginValue v) {
    if (v.type == VAL_INT) return (double)v.as.integer;
    if (v.type == VAL_FLOAT) return v.as.floating;
    return 0.0;
}

static RizPluginAPI* G_API;

static RizPluginValue math_sin(RizPluginValue* args, int argc) {
    return G_API->make_float(sin(get_num(args[0])));
}

static RizPluginValue math_cos(RizPluginValue* args, int argc) {
    return G_API->make_float(cos(get_num(args[0])));
}

static RizPluginValue math_tan(RizPluginValue* args, int argc) {
    return G_API->make_float(tan(get_num(args[0])));
}

static RizPluginValue math_sqrt(RizPluginValue* args, int argc) {
    return G_API->make_float(sqrt(get_num(args[0])));
}

static RizPluginValue math_pow(RizPluginValue* args, int argc) {
    return G_API->make_float(pow(get_num(args[0]), get_num(args[1])));
}

static RizPluginValue math_log(RizPluginValue* args, int argc) {
    return G_API->make_float(log(get_num(args[0])));
}

static RizPluginValue math_log10(RizPluginValue* args, int argc) {
    return G_API->make_float(log10(get_num(args[0])));
}

static RizPluginValue math_abs(RizPluginValue* args, int argc) {
    return G_API->make_float(fabs(get_num(args[0])));
}

/* Constants */
static RizPluginValue math_pi(RizPluginValue* args, int argc) {
    return G_API->make_float(3.14159265358979323846);
}
static RizPluginValue math_e(RizPluginValue* args, int argc) {
    return G_API->make_float(2.71828182845904523536);
}

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G_API = api;
    RizPluginValue math_dict = api->make_dict();
    api->dict_set_fn(math_dict, "sin", "math.sin", math_sin, 1);
    api->dict_set_fn(math_dict, "cos", "math.cos", math_cos, 1);
    api->dict_set_fn(math_dict, "tan", "math.tan", math_tan, 1);
    api->dict_set_fn(math_dict, "sqrt", "math.sqrt", math_sqrt, 1);
    api->dict_set_fn(math_dict, "pow", "math.pow", math_pow, 2);
    api->dict_set_fn(math_dict, "log", "math.log", math_log, 1);
    api->dict_set_fn(math_dict, "log10", "math.log10", math_log10, 1);
    api->dict_set_fn(math_dict, "abs", "math.abs", math_abs, 1);
    
    /* Variables implemented as 0-arity functions for now, 
       because dict_set_fn registers functions. 
       We will need a dict_set_value in API to set bare values! */
    api->dict_set_fn(math_dict, "pi", "math.pi", math_pi, 0);
    api->dict_set_fn(math_dict, "e", "math.e", math_e, 0);

    api->define_global(api->interp, "math", math_dict);
}
