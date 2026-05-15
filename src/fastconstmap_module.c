/*
 * fastconstmap — Python C extension
 * Exposes ConstMap and VerifiedConstMap built from a {str: int} dict.
 *
 * Apache License 2.0
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "constmap.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Error helpers                                                             */
/* ------------------------------------------------------------------------- */

static void fcm_set_python_error(int rc) {
    switch (rc) {
    case FCM_E_LENGTH_MISMATCH:
        PyErr_SetString(PyExc_ValueError, "input length mismatch");
        break;
    case FCM_E_NOMEM:
        PyErr_NoMemory();
        break;
    case FCM_E_DUPLICATE_KEY:
        PyErr_SetString(PyExc_ValueError,
                        "duplicate key hash detected (two keys hash to the same xxhash value)");
        break;
    case FCM_E_CONSTRUCT_FAIL:
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to construct map after 100 iterations");
        break;
    case FCM_E_INVALID_FORMAT:
        PyErr_SetString(PyExc_ValueError, "invalid serialized data (bad magic)");
        break;
    case FCM_E_CHECKSUM:
        PyErr_SetString(PyExc_ValueError, "serialized data checksum mismatch");
        break;
    case FCM_E_SHORT_BUFFER:
        PyErr_SetString(PyExc_ValueError, "serialized data is truncated");
        break;
    default:
        PyErr_Format(PyExc_RuntimeError, "fastconstmap error %d", rc);
        break;
    }
}

/*
 * Convert a Python dict into fcm_key_t[] + uint64_t[]. Keys must be str,
 * values must be ints fitting in unsigned 64 bits. Returns 0 on success,
 * -1 on failure (Python exception set).
 *
 * `*out_n` receives the number of entries.
 * `*out_keys` and `*out_values` are heap-allocated; caller frees both.
 * `*out_keep` is a Python tuple that keeps the str/bytes alive while keys[i].bytes
 * pointers are in use; caller must Py_DECREF it after construction.
 */
static int fcm_dict_to_arrays(PyObject *dict,
                              size_t *out_n,
                              fcm_key_t **out_keys,
                              uint64_t **out_values,
                              PyObject **out_keep) {
    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "expected a dict");
        return -1;
    }
    Py_ssize_t n = PyDict_Size(dict);
    if (n < 0) return -1;

    fcm_key_t *keys = NULL;
    uint64_t  *vals = NULL;
    PyObject  *keep = NULL;

    keep = PyTuple_New(n);
    if (!keep) goto fail;
    keys = (fcm_key_t *)PyMem_Malloc((size_t)n * sizeof(fcm_key_t));
    vals = (uint64_t  *)PyMem_Malloc((size_t)n * sizeof(uint64_t));
    if (!keys || !vals) { PyErr_NoMemory(); goto fail; }

    Py_ssize_t pos = 0;
    Py_ssize_t i = 0;
    PyObject *k, *v;
    while (PyDict_Next(dict, &pos, &k, &v)) {
        /* Accept str (encode utf-8) or bytes. */
        const char *kbytes;
        Py_ssize_t klen;
        PyObject *kref = NULL;
        if (PyUnicode_Check(k)) {
            kbytes = PyUnicode_AsUTF8AndSize(k, &klen);
            if (!kbytes) goto fail;
            kref = k;
            Py_INCREF(kref);
        } else if (PyBytes_Check(k)) {
            if (PyBytes_AsStringAndSize(k, (char **)&kbytes, &klen) < 0) goto fail;
            kref = k;
            Py_INCREF(kref);
        } else {
            PyErr_Format(PyExc_TypeError,
                         "all keys must be str (or bytes); got %s",
                         Py_TYPE(k)->tp_name);
            goto fail;
        }
        PyTuple_SET_ITEM(keep, i, kref);  /* steals ref */

        if (!PyLong_Check(v)) {
            PyErr_Format(PyExc_TypeError,
                         "all values must be int; got %s",
                         Py_TYPE(v)->tp_name);
            goto fail;
        }
        /* Accept any int in [-2^63, 2^64 - 1]. Try unsigned first; on
         * overflow, fall back to signed (so we can take negatives via
         * two's-complement). */
        unsigned long long u = PyLong_AsUnsignedLongLong(v);
        if (u == (unsigned long long)-1 && PyErr_Occurred()) {
            PyErr_Clear();
            long long s = PyLong_AsLongLong(v);
            if (s == -1 && PyErr_Occurred()) {
                PyErr_Clear();
                PyErr_SetString(PyExc_OverflowError,
                                "value does not fit in 64 bits "
                                "(must be in [-2**63, 2**64 - 1])");
                goto fail;
            }
            u = (unsigned long long)s;
        }
        vals[i] = (uint64_t)u;

        keys[i].bytes = kbytes;
        keys[i].len   = (size_t)klen;
        i++;
    }

    *out_n      = (size_t)n;
    *out_keys   = keys;
    *out_values = vals;
    *out_keep   = keep;
    return 0;

fail:
    PyMem_Free(keys);
    PyMem_Free(vals);
    Py_XDECREF(keep);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* ConstMap type                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    fcm_constmap_t cm;
    /* When `view.obj` is non-NULL the map is a zero-copy view: `cm.data`
     * points into `view`'s buffer and must NOT be freed. */
    Py_buffer view;
} PyConstMap;

/* Release whatever storage the map currently holds (owned heap data or a
 * borrowed buffer). Safe to call on a zero-initialised struct. */
static void pyconstmap_release(PyConstMap *self) {
    if (self->view.obj != NULL) {
        /* Borrowed: data lives inside the buffer; just drop our reference. */
        PyBuffer_Release(&self->view);   /* sets view.obj = NULL */
        self->cm.data = NULL;
    }
    fcm_constmap_free(&self->cm);        /* frees owned data; no-op if NULL */
}

static int PyConstMap_init(PyConstMap *self, PyObject *args, PyObject *kwargs) {
    PyObject *dict;
    static char *kwlist[] = {"mapping", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwlist, &dict)) return -1;

    size_t     n = 0;
    fcm_key_t *keys = NULL;
    uint64_t  *vals = NULL;
    PyObject  *keep = NULL;
    if (fcm_dict_to_arrays(dict, &n, &keys, &vals, &keep) < 0) return -1;

    pyconstmap_release(self);

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = fcm_constmap_new(&self->cm, keys, vals, n);
    Py_END_ALLOW_THREADS

    PyMem_Free(keys);
    PyMem_Free(vals);
    Py_DECREF(keep);

    if (rc != FCM_OK) { fcm_set_python_error(rc); return -1; }
    return 0;
}

static void PyConstMap_dealloc(PyConstMap *self) {
    pyconstmap_release(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *PyConstMap_get(PyConstMap *self, PyObject *arg) {
    const char *s;
    Py_ssize_t  n;
    if (PyUnicode_Check(arg)) {
        s = PyUnicode_AsUTF8AndSize(arg, &n);
        if (!s) return NULL;
    } else if (PyBytes_Check(arg)) {
        if (PyBytes_AsStringAndSize(arg, (char **)&s, &n) < 0) return NULL;
    } else {
        PyErr_Format(PyExc_TypeError, "key must be str or bytes, not %s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    uint64_t v = fcm_constmap_lookup(&self->cm, s, (size_t)n);
    return PyLong_FromUnsignedLongLong((unsigned long long)v);
}

/* Batch lookup: input is any iterable of str/bytes; returns a list of ints. */
static PyObject *PyConstMap_get_many(PyConstMap *self, PyObject *arg) {
    /* Fast path for lists/tuples: avoid iterator overhead. */
    if (PyList_Check(arg) || PyTuple_Check(arg)) {
        Py_ssize_t n = PySequence_Fast_GET_SIZE(arg);
        PyObject **items = PySequence_Fast_ITEMS(arg);
        PyObject *out = PyList_New(n);
        if (!out) return NULL;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *k = items[i];
            const char *s;
            Py_ssize_t klen;
            if (PyUnicode_Check(k)) {
                s = PyUnicode_AsUTF8AndSize(k, &klen);
                if (!s) { Py_DECREF(out); return NULL; }
            } else if (PyBytes_Check(k)) {
                if (PyBytes_AsStringAndSize(k, (char **)&s, &klen) < 0) {
                    Py_DECREF(out); return NULL;
                }
            } else {
                Py_DECREF(out);
                PyErr_Format(PyExc_TypeError,
                             "keys must be str or bytes, not %s",
                             Py_TYPE(k)->tp_name);
                return NULL;
            }
            uint64_t v = fcm_constmap_lookup(&self->cm, s, (size_t)klen);
            PyObject *iv = PyLong_FromUnsignedLongLong((unsigned long long)v);
            if (!iv) { Py_DECREF(out); return NULL; }
            PyList_SET_ITEM(out, i, iv);
        }
        return out;
    }

    /* General iterable. */
    PyObject *it = PyObject_GetIter(arg);
    if (!it) return NULL;
    PyObject *out = PyList_New(0);
    if (!out) { Py_DECREF(it); return NULL; }
    PyObject *item;
    while ((item = PyIter_Next(it)) != NULL) {
        const char *s;
        Py_ssize_t klen;
        if (PyUnicode_Check(item)) {
            s = PyUnicode_AsUTF8AndSize(item, &klen);
            if (!s) { Py_DECREF(item); goto fail; }
        } else if (PyBytes_Check(item)) {
            if (PyBytes_AsStringAndSize(item, (char **)&s, &klen) < 0) {
                Py_DECREF(item); goto fail;
            }
        } else {
            PyErr_Format(PyExc_TypeError,
                         "keys must be str or bytes, not %s",
                         Py_TYPE(item)->tp_name);
            Py_DECREF(item); goto fail;
        }
        uint64_t v = fcm_constmap_lookup(&self->cm, s, (size_t)klen);
        Py_DECREF(item);
        PyObject *iv = PyLong_FromUnsignedLongLong((unsigned long long)v);
        if (!iv) goto fail;
        if (PyList_Append(out, iv) < 0) { Py_DECREF(iv); goto fail; }
        Py_DECREF(iv);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_DECREF(out); return NULL; }
    return out;
fail:
    Py_DECREF(it);
    Py_DECREF(out);
    return NULL;
}

static Py_ssize_t PyConstMap_length(PyConstMap *self) {
    return (Py_ssize_t)self->cm.n;
}

static PyObject *PyConstMap_subscript(PyConstMap *self, PyObject *key) {
    return PyConstMap_get(self, key);
}

static PyObject *PyConstMap_serialize(PyConstMap *self, PyObject *Py_UNUSED(ignored)) {
    size_t sz = fcm_constmap_serialized_size(&self->cm);
    PyObject *out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)sz);
    if (!out) return NULL;
    fcm_constmap_write(&self->cm, PyBytes_AsString(out));
    return out;
}

static PyObject *PyConstMap_deserialize(PyTypeObject *type, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyConstMap *self = (PyConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyBuffer_Release(&view); return NULL; }
    memset(&self->cm, 0, sizeof(self->cm));
    int rc = fcm_constmap_read(&self->cm, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    if (rc != FCM_OK) {
        Py_DECREF(self);
        fcm_set_python_error(rc);
        return NULL;
    }
    return (PyObject *)self;
}

static PyObject *PyConstMap_save(PyConstMap *self, PyObject *arg) {
    PyObject *path = PyOS_FSPath(arg);
    if (!path) return NULL;
    const char *fname = PyUnicode_AsUTF8(path);
    if (!fname) { Py_DECREF(path); return NULL; }
    FILE *f = fopen(fname, "wb");
    if (!f) { Py_DECREF(path); PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, path); return NULL; }
    Py_DECREF(path);
    size_t sz = fcm_constmap_serialized_size(&self->cm);
    void *buf = PyMem_Malloc(sz);
    if (!buf) { fclose(f); PyErr_NoMemory(); return NULL; }
    fcm_constmap_write(&self->cm, buf);
    size_t w = fwrite(buf, 1, sz, f);
    PyMem_Free(buf);
    int err = ferror(f);
    fclose(f);
    if (w != sz || err) {
        PyErr_SetString(PyExc_OSError, "short write");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyConstMap_load(PyTypeObject *type, PyObject *arg) {
    PyObject *path = PyOS_FSPath(arg);
    if (!path) return NULL;
    const char *fname = PyUnicode_AsUTF8(path);
    if (!fname) { Py_DECREF(path); return NULL; }
    FILE *f = fopen(fname, "rb");
    if (!f) { PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, path); Py_DECREF(path); return NULL; }
    Py_DECREF(path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); PyErr_SetString(PyExc_ValueError, "empty file"); return NULL; }
    void *buf = PyMem_Malloc((size_t)len);
    if (!buf) { fclose(f); PyErr_NoMemory(); return NULL; }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (r != (size_t)len) { PyMem_Free(buf); PyErr_SetString(PyExc_OSError, "short read"); return NULL; }
    PyConstMap *self = (PyConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyMem_Free(buf); return NULL; }
    memset(&self->cm, 0, sizeof(self->cm));
    int rc = fcm_constmap_read(&self->cm, buf, (size_t)len);
    PyMem_Free(buf);
    if (rc != FCM_OK) { Py_DECREF(self); fcm_set_python_error(rc); return NULL; }
    return (PyObject *)self;
}

static PyObject *PyConstMap_data_bytes(PyConstMap *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromUnsignedLongLong((unsigned long long)self->cm.data_len * 8ULL);
}

static PyObject *PyConstMap_serialized_size(PyConstMap *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromSize_t(fcm_constmap_serialized_size(&self->cm));
}

/* Serialize directly into a caller-provided writable buffer (e.g. a
 * SharedMemory block), avoiding the intermediate bytes object. */
static PyObject *PyConstMap_write_into(PyConstMap *self, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_WRITABLE) < 0) return NULL;
    size_t need = fcm_constmap_serialized_size(&self->cm);
    if (view.len < 0 || (size_t)view.len < need) {
        PyErr_Format(PyExc_ValueError,
                     "buffer too small: need %zu bytes, got %zd",
                     need, view.len);
        PyBuffer_Release(&view);
        return NULL;
    }
    fcm_constmap_write(&self->cm, view.buf);
    PyBuffer_Release(&view);
    return PyLong_FromSize_t(need);
}

/* Zero-copy: build a map that reads directly out of `buffer`. The buffer
 * is kept alive by the returned object; lookups touch it directly. */
static PyObject *PyConstMap_from_buffer(PyTypeObject *type, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyConstMap *self = (PyConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyBuffer_Release(&view); return NULL; }
    /* tp_alloc zeroed the struct, so self->view.obj == NULL and cm is clear. */
    int rc = fcm_constmap_view(&self->cm, view.buf, (size_t)view.len);
    if (rc != FCM_OK) {
        PyBuffer_Release(&view);
        Py_DECREF(self);
        if (rc == FCM_E_UNALIGNED) {
            PyErr_SetString(PyExc_ValueError,
                "buffer is not 8-byte aligned; cannot create a zero-copy view "
                "(use from_bytes() to copy instead)");
        } else {
            fcm_set_python_error(rc);
        }
        return NULL;
    }
    self->view = view;  /* keep the buffer alive for the map's lifetime */
    return (PyObject *)self;
}

static PyMethodDef PyConstMap_methods[] = {
    {"get",             (PyCFunction)PyConstMap_get,             METH_O,
     "Return the value for `key`. If `key` was not in the original mapping, the return value is undefined."},
    {"get_many",        (PyCFunction)PyConstMap_get_many,        METH_O,
     "Look up an iterable of keys. Returns a list of ints in the same order."},
    {"to_bytes",        (PyCFunction)PyConstMap_serialize,       METH_NOARGS,
     "Serialize the map to bytes."},
    {"from_bytes",      (PyCFunction)PyConstMap_deserialize,     METH_O   | METH_CLASS,
     "Deserialize a map from a bytes-like object (copies the data)."},
    {"serialized_size", (PyCFunction)PyConstMap_serialized_size, METH_NOARGS,
     "Number of bytes that to_bytes()/write_into() will produce."},
    {"write_into",      (PyCFunction)PyConstMap_write_into,      METH_O,
     "Serialize directly into a writable buffer (e.g. SharedMemory.buf). Returns bytes written."},
    {"from_buffer",     (PyCFunction)PyConstMap_from_buffer,     METH_O   | METH_CLASS,
     "Create a zero-copy map that reads directly from a buffer (e.g. shared memory). "
     "The buffer is kept alive by the returned map and must not be modified while in use."},
    {"save",            (PyCFunction)PyConstMap_save,            METH_O,
     "Save the map to a file path."},
    {"load",            (PyCFunction)PyConstMap_load,            METH_O   | METH_CLASS,
     "Load a map from a file path."},
    {"nbytes",          (PyCFunction)PyConstMap_data_bytes,      METH_NOARGS,
     "Heap size of the lookup array in bytes."},
    {NULL, NULL, 0, NULL}
};

static PyMappingMethods PyConstMap_as_mapping = {
    (lenfunc)PyConstMap_length,
    (binaryfunc)PyConstMap_subscript,
    NULL,
};

static PyTypeObject PyConstMapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "fastconstmap._fastconstmap.ConstMap",
    .tp_basicsize = sizeof(PyConstMap),
    .tp_dealloc   = (destructor)PyConstMap_dealloc,
    .tp_as_mapping = &PyConstMap_as_mapping,
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Immutable map from strings to uint64. Returns undefined values for missing keys.",
    .tp_methods   = PyConstMap_methods,
    .tp_init      = (initproc)PyConstMap_init,
    .tp_new       = PyType_GenericNew,
};

/* ------------------------------------------------------------------------- */
/* VerifiedConstMap type                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    fcm_verified_constmap_t vm;
    /* Non-NULL view.obj => zero-copy view: data/checks point into the buffer. */
    Py_buffer view;
} PyVerifiedConstMap;

static void pyverified_release(PyVerifiedConstMap *self) {
    if (self->view.obj != NULL) {
        PyBuffer_Release(&self->view);
        self->vm.data   = NULL;
        self->vm.checks = NULL;
    }
    fcm_verified_constmap_free(&self->vm);
}

static int PyVerifiedConstMap_init(PyVerifiedConstMap *self, PyObject *args, PyObject *kwargs) {
    PyObject *dict;
    static char *kwlist[] = {"mapping", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwlist, &dict)) return -1;

    size_t     n = 0;
    fcm_key_t *keys = NULL;
    uint64_t  *vals = NULL;
    PyObject  *keep = NULL;
    if (fcm_dict_to_arrays(dict, &n, &keys, &vals, &keep) < 0) return -1;

    pyverified_release(self);

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = fcm_verified_constmap_new(&self->vm, keys, vals, n);
    Py_END_ALLOW_THREADS

    PyMem_Free(keys);
    PyMem_Free(vals);
    Py_DECREF(keep);

    if (rc != FCM_OK) { fcm_set_python_error(rc); return -1; }
    return 0;
}

static void PyVerifiedConstMap_dealloc(PyVerifiedConstMap *self) {
    pyverified_release(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* Returns None for missing keys. */
static PyObject *PyVerifiedConstMap_get(PyVerifiedConstMap *self, PyObject *args) {
    PyObject *key;
    PyObject *default_ = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &default_)) return NULL;
    const char *s;
    Py_ssize_t  n;
    if (PyUnicode_Check(key)) {
        s = PyUnicode_AsUTF8AndSize(key, &n);
        if (!s) return NULL;
    } else if (PyBytes_Check(key)) {
        if (PyBytes_AsStringAndSize(key, (char **)&s, &n) < 0) return NULL;
    } else {
        PyErr_Format(PyExc_TypeError, "key must be str or bytes, not %s",
                     Py_TYPE(key)->tp_name);
        return NULL;
    }
    uint64_t v = fcm_verified_constmap_lookup(&self->vm, s, (size_t)n);
    if (v == FCM_NOT_FOUND) { Py_INCREF(default_); return default_; }
    return PyLong_FromUnsignedLongLong((unsigned long long)v);
}

static PyObject *PyVerifiedConstMap_subscript(PyVerifiedConstMap *self, PyObject *key) {
    const char *s;
    Py_ssize_t  n;
    if (PyUnicode_Check(key)) {
        s = PyUnicode_AsUTF8AndSize(key, &n);
        if (!s) return NULL;
    } else if (PyBytes_Check(key)) {
        if (PyBytes_AsStringAndSize(key, (char **)&s, &n) < 0) return NULL;
    } else {
        PyErr_Format(PyExc_TypeError, "key must be str or bytes, not %s",
                     Py_TYPE(key)->tp_name);
        return NULL;
    }
    uint64_t v = fcm_verified_constmap_lookup(&self->vm, s, (size_t)n);
    if (v == FCM_NOT_FOUND) { PyErr_SetObject(PyExc_KeyError, key); return NULL; }
    return PyLong_FromUnsignedLongLong((unsigned long long)v);
}

static int PyVerifiedConstMap_contains(PyVerifiedConstMap *self, PyObject *key) {
    const char *s;
    Py_ssize_t  n;
    if (PyUnicode_Check(key)) {
        s = PyUnicode_AsUTF8AndSize(key, &n);
        if (!s) return -1;
    } else if (PyBytes_Check(key)) {
        if (PyBytes_AsStringAndSize(key, (char **)&s, &n) < 0) return -1;
    } else {
        return 0;  /* non-str/bytes keys are not in the map */
    }
    uint64_t v = fcm_verified_constmap_lookup(&self->vm, s, (size_t)n);
    return v == FCM_NOT_FOUND ? 0 : 1;
}

static PyObject *PyVerifiedConstMap_get_many(PyVerifiedConstMap *self, PyObject *args, PyObject *kwargs) {
    PyObject *arg;
    PyObject *default_ = Py_None;
    static char *kwlist[] = {"keys", "default", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwlist, &arg, &default_)) return NULL;

    if (PyList_Check(arg) || PyTuple_Check(arg)) {
        Py_ssize_t n = PySequence_Fast_GET_SIZE(arg);
        PyObject **items = PySequence_Fast_ITEMS(arg);
        PyObject *out = PyList_New(n);
        if (!out) return NULL;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *k = items[i];
            const char *s;
            Py_ssize_t klen;
            if (PyUnicode_Check(k)) {
                s = PyUnicode_AsUTF8AndSize(k, &klen);
                if (!s) { Py_DECREF(out); return NULL; }
            } else if (PyBytes_Check(k)) {
                if (PyBytes_AsStringAndSize(k, (char **)&s, &klen) < 0) {
                    Py_DECREF(out); return NULL;
                }
            } else {
                Py_DECREF(out);
                PyErr_Format(PyExc_TypeError,
                             "keys must be str or bytes, not %s",
                             Py_TYPE(k)->tp_name);
                return NULL;
            }
            uint64_t v = fcm_verified_constmap_lookup(&self->vm, s, (size_t)klen);
            PyObject *iv;
            if (v == FCM_NOT_FOUND) {
                iv = default_;
                Py_INCREF(iv);
            } else {
                iv = PyLong_FromUnsignedLongLong((unsigned long long)v);
                if (!iv) { Py_DECREF(out); return NULL; }
            }
            PyList_SET_ITEM(out, i, iv);
        }
        return out;
    }

    PyObject *it = PyObject_GetIter(arg);
    if (!it) return NULL;
    PyObject *out = PyList_New(0);
    if (!out) { Py_DECREF(it); return NULL; }
    PyObject *item;
    while ((item = PyIter_Next(it)) != NULL) {
        const char *s;
        Py_ssize_t klen;
        if (PyUnicode_Check(item)) {
            s = PyUnicode_AsUTF8AndSize(item, &klen);
            if (!s) { Py_DECREF(item); goto fail; }
        } else if (PyBytes_Check(item)) {
            if (PyBytes_AsStringAndSize(item, (char **)&s, &klen) < 0) {
                Py_DECREF(item); goto fail;
            }
        } else {
            PyErr_Format(PyExc_TypeError,
                         "keys must be str or bytes, not %s",
                         Py_TYPE(item)->tp_name);
            Py_DECREF(item); goto fail;
        }
        uint64_t v = fcm_verified_constmap_lookup(&self->vm, s, (size_t)klen);
        Py_DECREF(item);
        PyObject *iv;
        if (v == FCM_NOT_FOUND) {
            iv = default_;
            Py_INCREF(iv);
        } else {
            iv = PyLong_FromUnsignedLongLong((unsigned long long)v);
            if (!iv) goto fail;
        }
        if (PyList_Append(out, iv) < 0) { Py_DECREF(iv); goto fail; }
        Py_DECREF(iv);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_DECREF(out); return NULL; }
    return out;
fail:
    Py_DECREF(it);
    Py_DECREF(out);
    return NULL;
}

static Py_ssize_t PyVerifiedConstMap_length(PyVerifiedConstMap *self) {
    return (Py_ssize_t)self->vm.n;
}

static PyObject *PyVerifiedConstMap_serialize(PyVerifiedConstMap *self, PyObject *Py_UNUSED(ignored)) {
    size_t sz = fcm_verified_constmap_serialized_size(&self->vm);
    PyObject *out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)sz);
    if (!out) return NULL;
    fcm_verified_constmap_write(&self->vm, PyBytes_AsString(out));
    return out;
}

static PyObject *PyVerifiedConstMap_deserialize(PyTypeObject *type, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyVerifiedConstMap *self = (PyVerifiedConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyBuffer_Release(&view); return NULL; }
    memset(&self->vm, 0, sizeof(self->vm));
    int rc = fcm_verified_constmap_read(&self->vm, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    if (rc != FCM_OK) { Py_DECREF(self); fcm_set_python_error(rc); return NULL; }
    return (PyObject *)self;
}

static PyObject *PyVerifiedConstMap_save(PyVerifiedConstMap *self, PyObject *arg) {
    PyObject *path = PyOS_FSPath(arg);
    if (!path) return NULL;
    const char *fname = PyUnicode_AsUTF8(path);
    if (!fname) { Py_DECREF(path); return NULL; }
    FILE *f = fopen(fname, "wb");
    if (!f) { Py_DECREF(path); PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, path); return NULL; }
    Py_DECREF(path);
    size_t sz = fcm_verified_constmap_serialized_size(&self->vm);
    void *buf = PyMem_Malloc(sz);
    if (!buf) { fclose(f); PyErr_NoMemory(); return NULL; }
    fcm_verified_constmap_write(&self->vm, buf);
    size_t w = fwrite(buf, 1, sz, f);
    PyMem_Free(buf);
    int err = ferror(f);
    fclose(f);
    if (w != sz || err) { PyErr_SetString(PyExc_OSError, "short write"); return NULL; }
    Py_RETURN_NONE;
}

static PyObject *PyVerifiedConstMap_load(PyTypeObject *type, PyObject *arg) {
    PyObject *path = PyOS_FSPath(arg);
    if (!path) return NULL;
    const char *fname = PyUnicode_AsUTF8(path);
    if (!fname) { Py_DECREF(path); return NULL; }
    FILE *f = fopen(fname, "rb");
    if (!f) { PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, path); Py_DECREF(path); return NULL; }
    Py_DECREF(path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); PyErr_SetString(PyExc_ValueError, "empty file"); return NULL; }
    void *buf = PyMem_Malloc((size_t)len);
    if (!buf) { fclose(f); PyErr_NoMemory(); return NULL; }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (r != (size_t)len) { PyMem_Free(buf); PyErr_SetString(PyExc_OSError, "short read"); return NULL; }
    PyVerifiedConstMap *self = (PyVerifiedConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyMem_Free(buf); return NULL; }
    memset(&self->vm, 0, sizeof(self->vm));
    int rc = fcm_verified_constmap_read(&self->vm, buf, (size_t)len);
    PyMem_Free(buf);
    if (rc != FCM_OK) { Py_DECREF(self); fcm_set_python_error(rc); return NULL; }
    return (PyObject *)self;
}

static PyObject *PyVerifiedConstMap_nbytes(PyVerifiedConstMap *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromUnsignedLongLong((unsigned long long)self->vm.data_len * 16ULL);
}

static PyObject *PyVerifiedConstMap_serialized_size(PyVerifiedConstMap *self, PyObject *Py_UNUSED(ignored)) {
    return PyLong_FromSize_t(fcm_verified_constmap_serialized_size(&self->vm));
}

static PyObject *PyVerifiedConstMap_write_into(PyVerifiedConstMap *self, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_WRITABLE) < 0) return NULL;
    size_t need = fcm_verified_constmap_serialized_size(&self->vm);
    if (view.len < 0 || (size_t)view.len < need) {
        PyErr_Format(PyExc_ValueError,
                     "buffer too small: need %zu bytes, got %zd",
                     need, view.len);
        PyBuffer_Release(&view);
        return NULL;
    }
    fcm_verified_constmap_write(&self->vm, view.buf);
    PyBuffer_Release(&view);
    return PyLong_FromSize_t(need);
}

static PyObject *PyVerifiedConstMap_from_buffer(PyTypeObject *type, PyObject *arg) {
    Py_buffer view;
    if (PyObject_GetBuffer(arg, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyVerifiedConstMap *self = (PyVerifiedConstMap *)type->tp_alloc(type, 0);
    if (!self) { PyBuffer_Release(&view); return NULL; }
    int rc = fcm_verified_constmap_view(&self->vm, view.buf, (size_t)view.len);
    if (rc != FCM_OK) {
        PyBuffer_Release(&view);
        Py_DECREF(self);
        if (rc == FCM_E_UNALIGNED) {
            PyErr_SetString(PyExc_ValueError,
                "buffer is not 8-byte aligned; cannot create a zero-copy view "
                "(use from_bytes() to copy instead)");
        } else {
            fcm_set_python_error(rc);
        }
        return NULL;
    }
    self->view = view;
    return (PyObject *)self;
}

static PyMethodDef PyVerifiedConstMap_methods[] = {
    {"get",             (PyCFunction)PyVerifiedConstMap_get,                       METH_VARARGS,
     "Return value for `key`, or `default` (default None) if not present."},
    {"get_many",        (PyCFunction)(void *)PyVerifiedConstMap_get_many,          METH_VARARGS | METH_KEYWORDS,
     "Look up an iterable of keys; missing keys yield `default` (default None)."},
    {"to_bytes",        (PyCFunction)PyVerifiedConstMap_serialize,                 METH_NOARGS,
     "Serialize the map to bytes."},
    {"from_bytes",      (PyCFunction)PyVerifiedConstMap_deserialize,               METH_O | METH_CLASS,
     "Deserialize a map from a bytes-like object (copies the data)."},
    {"serialized_size", (PyCFunction)PyVerifiedConstMap_serialized_size,           METH_NOARGS,
     "Number of bytes that to_bytes()/write_into() will produce."},
    {"write_into",      (PyCFunction)PyVerifiedConstMap_write_into,                METH_O,
     "Serialize directly into a writable buffer (e.g. SharedMemory.buf). Returns bytes written."},
    {"from_buffer",     (PyCFunction)PyVerifiedConstMap_from_buffer,               METH_O | METH_CLASS,
     "Create a zero-copy map that reads directly from a buffer (e.g. shared memory). "
     "The buffer is kept alive by the returned map and must not be modified while in use."},
    {"save",            (PyCFunction)PyVerifiedConstMap_save,                      METH_O,
     "Save the map to a file path."},
    {"load",            (PyCFunction)PyVerifiedConstMap_load,                      METH_O | METH_CLASS,
     "Load a map from a file path."},
    {"nbytes",          (PyCFunction)PyVerifiedConstMap_nbytes,                    METH_NOARGS,
     "Heap size of the data + checks arrays in bytes."},
    {NULL, NULL, 0, NULL}
};

static PyMappingMethods PyVerifiedConstMap_as_mapping = {
    (lenfunc)PyVerifiedConstMap_length,
    (binaryfunc)PyVerifiedConstMap_subscript,
    NULL,
};

static PySequenceMethods PyVerifiedConstMap_as_sequence = {
    .sq_contains = (objobjproc)PyVerifiedConstMap_contains,
};

static PyTypeObject PyVerifiedConstMapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name        = "fastconstmap._fastconstmap.VerifiedConstMap",
    .tp_basicsize   = sizeof(PyVerifiedConstMap),
    .tp_dealloc     = (destructor)PyVerifiedConstMap_dealloc,
    .tp_as_mapping  = &PyVerifiedConstMap_as_mapping,
    .tp_as_sequence = &PyVerifiedConstMap_as_sequence,
    .tp_flags       = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc         = "Immutable map from strings to uint64. Detects keys not in the original mapping.",
    .tp_methods     = PyVerifiedConstMap_methods,
    .tp_init        = (initproc)PyVerifiedConstMap_init,
    .tp_new         = PyType_GenericNew,
};

/* ------------------------------------------------------------------------- */
/* Module                                                                    */
/* ------------------------------------------------------------------------- */

static PyModuleDef fastconstmap_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "fastconstmap._fastconstmap",
    .m_doc  = "C extension for fastconstmap.",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit__fastconstmap(void) {
    if (PyType_Ready(&PyConstMapType)        < 0) return NULL;
    if (PyType_Ready(&PyVerifiedConstMapType) < 0) return NULL;

    PyObject *m = PyModule_Create(&fastconstmap_module);
    if (!m) return NULL;

    Py_INCREF(&PyConstMapType);
    if (PyModule_AddObject(m, "ConstMap", (PyObject *)&PyConstMapType) < 0) {
        Py_DECREF(&PyConstMapType);
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(&PyVerifiedConstMapType);
    if (PyModule_AddObject(m, "VerifiedConstMap", (PyObject *)&PyVerifiedConstMapType) < 0) {
        Py_DECREF(&PyVerifiedConstMapType);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
