/*
 * Riz Interoperability Plugin -- CPython Bridge
 * 
 * Embeds Python to allow execution, importing modules, and dynamic 
 * marshalling of types.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "riz_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RizPluginAPI G;

/* ─── Object Wrapper ──────────────────────── */

static void pyobj_dtor(void* p) {
    PyObject* po = (PyObject*)p;
    if (po) {
        /* Python is protected by GIL, if we are in another thread this is unsafe,
           but Riz AOT runs synchronously. */
        Py_XDECREF(po); 
    }
}

static RizPluginValue wrap(PyObject* po) {
    if (!po) return G.make_none();
    return G.make_native_ptr(po, "PyObject", pyobj_dtor);
}

static PyObject* as_py(RizPluginValue v) {
    return (PyObject*)G.get_native_ptr(v);
}

/* ─── Type Marshalling ────────────────────── */

static PyObject* _riz2py(RizPluginValue v) {
    switch (v.type) {
        case VAL_INT:     return PyLong_FromLongLong(v.as.integer);
        case VAL_FLOAT:   return PyFloat_FromDouble(v.as.floating);
        case VAL_BOOL:    return PyBool_FromLong(v.as.boolean ? 1 : 0);
        case VAL_STRING:  return PyUnicode_FromString(v.as.string);
        case VAL_LIST: {
            int len = G.list_length(v);
            PyObject* list = PyList_New(len);
            for (int i = 0; i < len; i++) {
                RizPluginValue element = G.list_get(v, i);
                PyObject* py_elem = _riz2py(element);
                PyList_SetItem(list, i, py_elem); /* SetItem steals reference */
            }
            return list;
        }
        case VAL_NATIVE_PTR: {
            /* If it's already a PyObject, extract it directly.
               Otherwise we must return None in Python context. */
            PyObject* po = as_py(v);
            if (po) {
                Py_INCREF(po);
                return po;
            }
            Py_RETURN_NONE;
        }
        default:
            Py_RETURN_NONE;
    }
}

static RizPluginValue _py2riz(PyObject* po) {
    if (!po || po == Py_None) return G.make_none();
    if (PyBool_Check(po))     return G.make_bool(po == Py_True);
    if (PyLong_Check(po))     return G.make_int(PyLong_AsLongLong(po));
    if (PyFloat_Check(po))    return G.make_float(PyFloat_AsDouble(po));
    if (PyUnicode_Check(po))  return G.make_string(PyUnicode_AsUTF8(po));
    if (PyList_Check(po)) {
        RizPluginValue lst = G.make_list();
        Py_ssize_t size = PyList_Size(po);
        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyList_GetItem(po, i); /* Borrowed ref */
            G.list_append(lst, _py2riz(item));
        }
        return lst;
    }
    if (PyTuple_Check(po)) {
        RizPluginValue lst = G.make_list();
        Py_ssize_t size = PyTuple_Size(po);
        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyTuple_GetItem(po, i); /* Borrowed ref */
            G.list_append(lst, _py2riz(item));
        }
        return lst;
    }
    
    /* Complex object (functions, modules, dicts, numpy arrays) -> abstract */
    Py_INCREF(po);
    return wrap(po);
}

/* ─── API Functions ───────────────────────── */

/* Execute freeform Python code (no return) 
 * py_exec("import numpy as np")
 */
static RizPluginValue fn_py_exec(RizPluginValue* a, int n) {
    if (a[0].type != VAL_STRING) return G.make_none();
    PyRun_SimpleString(a[0].as.string);
    return G.make_none();
}

/* Evaluate a Python expression and return the converted result
 * let v = py_eval("1 + 2")
 */
static RizPluginValue fn_py_eval(RizPluginValue* a, int n) {
    if (a[0].type != VAL_STRING) return G.make_none();
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    PyObject* result = PyRun_String(a[0].as.string, Py_eval_input, globals, globals);
    if (!result) {
        PyErr_Print();
        return G.make_none();
    }
    RizPluginValue r = _py2riz(result);
    Py_DECREF(result);
    return r;
}

/* Import a Python module and return it as an object
 * let math = py_import("math")
 */
static RizPluginValue fn_py_import(RizPluginValue* a, int n) {
    if (a[0].type != VAL_STRING) return G.make_none();
    PyObject* mod_name = PyUnicode_FromString(a[0].as.string);
    PyObject* mod = PyImport_Import(mod_name);
    Py_DECREF(mod_name);
    
    if (!mod) {
        PyErr_Print();
        if (G.panic) G.panic(G.interp, "Failed to import Python module.");
        return G.make_none();
    }
    return wrap(mod);
}

/* Get an attribute from a Python object
 * let sqrt_fn = py_getattr(math, "sqrt")
 */
static RizPluginValue fn_py_getattr(RizPluginValue* a, int n) {
    PyObject* obj = as_py(a[0]);
    if (!obj || a[1].type != VAL_STRING) return G.make_none();
    
    PyObject* attr = PyObject_GetAttrString(obj, a[1].as.string);
    if (!attr) {
        PyErr_Clear();
        return G.make_none();
    }
    
    RizPluginValue r = _py2riz(attr);
    Py_DECREF(attr); /* If _py2riz wrapped it, it INCREF'd internally */
    return r;
}

/* Call a Python object (function, class constructor)
 * py_call(sqrt_fn, [16.0])
 */
static RizPluginValue fn_py_call(RizPluginValue* a, int n) {
    PyObject* callable = as_py(a[0]);
    if (!callable || !PyCallable_Check(callable)) {
        if (G.panic) G.panic(G.interp, "Target is not a valid Python callable.");
        return G.make_none();
    }
    
    PyObject* py_args = PyTuple_New(n - 1);
    for (int i = 1; i < n; i++) {
        PyTuple_SetItem(py_args, i - 1, _riz2py(a[i]));
    }
    
    PyObject* result = PyObject_CallObject(callable, py_args);
    Py_DECREF(py_args);
    
    if (!result) {
        PyErr_Print();
        if (G.panic) G.panic(G.interp, "Python Exception raised during py_call().");
        return G.make_none();
    }
    
    RizPluginValue ret = _py2riz(result);
    Py_DECREF(result);
    return ret;
}

/* ─── Entry Point ────────────────────────────── */

RIZ_EXPORT void riz_plugin_init(RizPluginAPI* api) {
    G = *api;
    
    /* Initialize the embedded Python interpreter */
    if (!Py_IsInitialized()) {
        Py_Initialize();
        printf("[Python Plugin] Embedded CPython Initialized.\n");
    }

    api->register_fn(api->interp, "py_exec",    fn_py_exec,     1);
    api->register_fn(api->interp, "py_eval",    fn_py_eval,     1);
    api->register_fn(api->interp, "py_import",  fn_py_import,   1);
    api->register_fn(api->interp, "py_getattr", fn_py_getattr,  2);
    api->register_fn(api->interp, "py_call",    fn_py_call,    -1);

    /* Global `py` namespace: py.exec("..."), py.import("sys"), py.call(fn, ...), etc. */
    if (api->make_dict && api->dict_set_fn && api->define_global) {
        RizPluginValue pyd = api->make_dict();
        api->dict_set_fn(pyd, "exec",    "py.exec",    fn_py_exec,     1);
        api->dict_set_fn(pyd, "eval",    "py.eval",    fn_py_eval,     1);
        api->dict_set_fn(pyd, "import",  "py.import",  fn_py_import,   1);
        api->dict_set_fn(pyd, "getattr", "py.getattr", fn_py_getattr,  2);
        api->dict_set_fn(pyd, "call",    "py.call",    fn_py_call,    -1);
        api->define_global(api->interp, "py", pyd);
    }
}
