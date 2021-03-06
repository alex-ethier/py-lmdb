/*
 * Copyright 2013 The py-lmdb authors, all rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * OpenLDAP is a registered trademark of the OpenLDAP Foundation.
 *
 * Individual files and/or contributed packages may be copyright by
 * other parties and/or subject to additional restrictions.
 *
 * This work also contains materials derived from public sources.
 *
 * Additional information about OpenLDAP can be obtained at
 * <http://www.openldap.org/>.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <tgmath.h>

#include "Python.h"
#include "structmember.h"

#include "lmdb.h"

#define DEBUG(s, ...) \
    fprintf(stderr, "lmdb.cpython: %s:%d: " s "\n", __func__, __LINE__, \
            ## __VA_ARGS__);

#define NODEBUG

#ifdef NODEBUG
#undef DEBUG
#define DEBUG(s, ...)
#endif

#define NOINLINE __attribute__((noinline))


static PyObject *Error;
static int drop_gil = 0;

enum string_id {
    APPEND_S,
    BUFFERS_S,
    CREATE_S,
    DB_S,
    DEFAULT_S,
    DELETE_S,
    DUPDATA_S,
    DUPSORT_S,
    FORCE_S,
    ITEMS_S,
    ITERITEMS_S,
    KEY_S,
    KEYS_S,
    MAP_ASYNC_S,
    MAP_SIZE_S,
    MAX_DBS_S,
    MAX_READERS_S,
    METASYNC_S,
    MODE_S,
    NAME_S,
    OVERWRITE_S,
    PARENT_S,
    PATH_S,
    READONLY_S,
    REVERSE_S,
    REVERSE_KEY_S,
    SUBDIR_S,
    SYNC_S,
    TXN_S,
    VALUE_S,
    VALUES_S,
    WRITE_S,
    WRITEMAP_S,

    // Must be last.
    STRING_ID_COUNT
};

static const char *strings = (
    "append\0"
    "buffers\0"
    "create\0"
    "db\0"
    "default\0"
    "delete\0"
    "dupdata\0"
    "dupsort\0"
    "force\0"
    "items\0"
    "iteritems\0"
    "key\0"
    "keys\0"
    "map_async\0"
    "map_size\0"
    "max_dbs\0"
    "max_readers\0"
    "metasync\0"
    "mode\0"
    "name\0"
    "overwrite\0"
    "parent\0"
    "path\0"
    "readonly\0"
    "reverse\0"
    "reverse_key\0"
    "subdir\0"
    "sync\0"
    "txn\0"
    "value\0"
    "values\0"
    "write\0"
    "writemap\0"
);
static PyObject **string_tbl;
static PyObject *py_zero;
static PyObject *py_int_max;
static PyObject *py_size_max;

extern PyTypeObject PyDatabase_Type;
extern PyTypeObject PyEnvironment_Type;
extern PyTypeObject PyTransaction_Type;
extern PyTypeObject PyCursor_Type;
extern PyTypeObject PyIterator_Type;

struct EnvObject;

#if PY_MAJOR_VERSION >= 3

// Python 3.3 kindly exports the struct definitions for us.
#   define MOD_RETURN(mod) return mod;
#   define MODINIT_NAME PyInit_cpython
#   define BUFFER_TYPE PyMemoryViewObject
#   define MAKE_BUFFER() PyMemoryView_FromMemory("", 0, PyBUF_READ)
#   define SET_BUFFER(buff, ptr, size) {\
        (buff)->view.buf = (ptr); \
        (buff)->view.len = (size); \
        (buff)->hash = -1; \
    }

#else

// So evil.
typedef struct {
    PyObject_HEAD
    PyObject *b_base;
    void *b_ptr;
    Py_ssize_t b_size;
    Py_ssize_t b_offset;
    int b_readonly;
    long b_hash;
} PyBufferObject;

#   define PyUnicode_InternFromString PyString_InternFromString
#   define PyBytes_AS_STRING PyString_AS_STRING
#   define PyBytes_GET_SIZE PyString_GET_SIZE
#   define PyBytes_CheckExact PyString_CheckExact
#   define PyBytes_FromStringAndSize PyString_FromStringAndSize
#   define MOD_RETURN(mod) return
#   define MODINIT_NAME initcpython
#   define BUFFER_TYPE PyBufferObject
#   define MAKE_BUFFER() PyBuffer_FromMemory("", 0)
#   define SET_BUFFER(buf, ptr, size) {\
        (buf)->b_hash = -1; \
        (buf)->b_ptr = (ptr); \
        (buf)->b_size = (size); \
    }

#endif

struct list_head {
    struct lmdb_object *prev;
    struct lmdb_object *next;
};

#define LmdbObject_HEAD \
    PyObject_HEAD \
    struct list_head siblings; \
    struct list_head children; \
    int valid;

struct lmdb_object {
    LmdbObject_HEAD
};

#define OBJECT_INIT(o) \
    ((struct lmdb_object *)o)->siblings.prev = NULL; \
    ((struct lmdb_object *)o)->siblings.next = NULL; \
    ((struct lmdb_object *)o)->children.prev = NULL; \
    ((struct lmdb_object *)o)->children.next = NULL; \
    ((struct lmdb_object *)o)->valid = 1;


/*
 * Invalidation works by keeping a list of transactions attached to the
 * environment, which in turn have a list of cursors attached to the
 * transaction. The pointers are at the same offset so a generic function is
 * used.
 *
 * To save effort, tp_clear is overloaded to be the object's invalidation
 * function, instead of carrying a separate pointer around. Objects are added
 * to their parent's list during construction and removed only during
 * deallocation.
 *
 * When the environment is closed, it walks its list calling tp_clear on each
 * child, which in turn walk their own lists.
 *
 * Child transactions are added to their parent transaction's list.
 *
 * Iterators keep no significant state, so they are not tracked.
 */

static void link_child(struct lmdb_object *parent, struct lmdb_object *child)
{
    struct lmdb_object *sibling = parent->children.next;
    if(sibling) {
        child->siblings.next = sibling;
        sibling->siblings.prev = child;
    }
    parent->children.next = child;
}

static void unlink_child(struct lmdb_object *parent, struct lmdb_object *child)
{
    if(! parent) {
        return;
    }

    struct lmdb_object *prev = child->siblings.prev;
    struct lmdb_object *next = child->siblings.next;
    if(prev) {
        prev->siblings.next = next;
             // If double unlink_child(), this test my legitimately fail:
    } else if(parent->children.next == child) {
        parent->children.next = next;
    }
    if(next) {
        next->siblings.prev = prev;
    }
    child->siblings.prev = NULL;
    child->siblings.next = NULL;
}

static void invalidate(struct lmdb_object *parent)
{
    struct lmdb_object *child = parent->children.next;
    while(child) {
        struct lmdb_object *next = child->siblings.next;
        DEBUG("invalidating parent=%p child %p", parent, child)
        Py_TYPE(child)->tp_clear((PyObject *) child);
        child = next;
    }
}

#define LINK_CHILD(parent, child) link_child((void *)parent, (void *)child);
#define UNLINK_CHILD(parent, child) unlink_child((void *)parent, (void *)child);
#define INVALIDATE(parent) invalidate((void *)parent);


struct EnvObject;

typedef struct {
    LmdbObject_HEAD
    struct EnvObject *env; // Not refcounted.
    MDB_dbi dbi;
} DbObject;

typedef struct EnvObject {
    LmdbObject_HEAD
    MDB_env *env;
    DbObject *main_db;
    int readonly; // If 1, transactions are always readonly.
} EnvObject;

typedef struct {
    LmdbObject_HEAD
    EnvObject *env;

    MDB_txn *txn;
    int buffers;
    BUFFER_TYPE *key_buf;
} TransObject;

typedef struct {
    LmdbObject_HEAD
    TransObject *trans;

    int positioned;
    MDB_cursor *curs;
    BUFFER_TYPE *key_buf;
    BUFFER_TYPE *val_buf;
    PyObject *item_tup;
    MDB_val key;
    MDB_val val;
} CursorObject;


// Iterator protocol requires 'next' public method, which we want to use for
// MDB. So iterator needs to be a separate object to implement the protocol
// correctly (option #2 was setting .tp_next but exposing MDB next(), which is
// even worse).
typedef struct {
    PyObject_HEAD
    CursorObject *curs;
    int started;
    int op;
    PyObject *(*val_func)(CursorObject *);
} IterObject;




// ----------- helpers
//
//
//

enum field_type { TYPE_EOF, TYPE_UINT, TYPE_SIZE, TYPE_ADDR };

struct dict_field {
    enum field_type type;
    const char *name;
    int offset;
};


/*
 * Convert the structure `o` described by `fields` to a dict and return the new
 * dict.
 */
static PyObject *
dict_from_fields(void *o, const struct dict_field *fields)
{
    PyObject *dict = PyDict_New();
    if(! dict) {
        return NULL;
    }

    while(fields->type != TYPE_EOF) {
        uint8_t *p = ((uint8_t *) o) + fields->offset;
        unsigned PY_LONG_LONG l = 0;
        if(fields->type == TYPE_UINT) {
            l = *(unsigned int *)p;
        } else if(fields->type == TYPE_SIZE) {
            l = *(size_t *)p;
        } else if(fields->type == TYPE_ADDR) {
            l = (intptr_t) *(void **)p;
        }

        PyObject *lo = PyLong_FromUnsignedLongLong(l);
        if(! lo) {
            Py_DECREF(dict);
            return NULL;
        }

        if(PyDict_SetItemString(dict, fields->name, lo)) {
            Py_DECREF(lo);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(lo);
        fields++;
    }
    return dict;
}


static PyObject * NOINLINE
buffer_from_val(BUFFER_TYPE **bufp, MDB_val *val)
{
    BUFFER_TYPE *buf = *bufp;
    if(! buf) {
        buf = (BUFFER_TYPE *) MAKE_BUFFER();
        if(! buf) {
            return NULL;
        }
        *bufp = buf;
    }

    SET_BUFFER(buf, val->mv_data, val->mv_size);
    Py_INCREF(buf);
    return (PyObject *) buf;
}


static PyObject *
string_from_val(MDB_val *val)
{
    return PyBytes_FromStringAndSize(val->mv_data, val->mv_size);
}


static int NOINLINE
val_from_buffer(MDB_val *val, PyObject *buf)
{
    if(PyBytes_CheckExact(buf)) {
        val->mv_data = PyBytes_AS_STRING(buf);
        val->mv_size = PyBytes_GET_SIZE(buf);
        return 0;
    }
#if PY_MAJOR_VERSION >= 3
    if(PyUnicode_CheckExact(buf)) {
        char *data;
        Py_ssize_t size;
        if(! (data = PyUnicode_AsUTF8AndSize(buf, &size))) {
            return -1;
        }
        val->mv_data = data;
        val->mv_size = size;
        return 0;
    }
#endif
    return PyObject_AsReadBuffer(buf,
        (const void **) &val->mv_data,
        (Py_ssize_t *) &val->mv_size);
}


// -------------------
// Concurrency control
// -------------------

static PyThreadState *save_thread(void)
{
    PyThreadState *s = NULL;
    if(drop_gil) {
        s = PyEval_SaveThread();
    }
    return s;
}

static void restore_thread(PyThreadState *state)
{
    if(drop_gil) {
        PyEval_RestoreThread(state);
    }
}

// Like Py_BEGIN_ALLOW_THREADS
#define DROP_GIL \
    { PyThreadState *_save; _save = save_thread();

// Like Py_END_ALLOW_THREADS
#define LOCK_GIL \
    restore_thread(_save); }

#define UNLOCKED(out, e) \
    DROP_GIL \
    out = (e); \
    LOCK_GIL



// ----------
// Exceptions
// ----------

static void * NOINLINE
err_set(const char *what, int rc)
{
    PyErr_Format(Error, "%s: %s", what, mdb_strerror(rc));
    return NULL;
}

static void * NOINLINE
err_invalid(void)
{
    PyErr_Format(Error, "Attempt to operate on closed/deleted/dropped object.");
    return NULL;
}

static void * NOINLINE
type_error(const char *what)
{
    PyErr_Format(PyExc_TypeError, "%s", what);
    return NULL;
}


/// ------------------------------
// argument parsing
//-------------------------------

#define OFFSET(k, y) offsetof(struct k, y)
#define SPECSIZE() (sizeof(argspec) / sizeof(argspec[0]))
enum arg_type {
    ARG_OBJ,
    ARG_DB,
    ARG_TRANS,
    ARG_BOOL,
    ARG_BUF,
    ARG_STR,
    ARG_INT,
    ARG_SIZE
};
struct argspec {
    unsigned char type;
    unsigned char string_id;
    unsigned short offset;
};

static PyTypeObject *type_tbl[] = {
    &PyDatabase_Type,
    &PyTransaction_Type,
};


static int
parse_ulong(PyObject *obj, uint64_t *l, PyObject *max)
{
    int rc = PyObject_RichCompareBool(obj, py_zero, Py_GE);
    if(rc == -1) {
        return -1;
    } else if(! rc) {
        type_error("Integer argument must be >= 0");
        return -1;
    }
    rc = PyObject_RichCompareBool(obj, max, Py_LE);
    if(rc == -1) {
        return -1;
    } else if(! rc) {
        type_error("Integer argument exceeds limit.");
        return -1;
    }
#if PY_MAJOR_VERSION >= 3
    *l = PyLong_AsUnsignedLongLongMask(obj);
#else
    *l = PyInt_AsUnsignedLongLongMask(obj);
#endif
    return 0;
}


static int
parse_arg(const struct argspec *spec, PyObject *val, void *out)
{
    void *dst = ((uint8_t *)out) + spec->offset;
    int ret = 0;
    uint64_t l;

    if(val != Py_None) {
        switch((enum arg_type) spec->type) {
        case ARG_OBJ:
        case ARG_DB:
        case ARG_TRANS:
            if(spec->type && val->ob_type != type_tbl[spec->type - 1]) {
                type_error("invalid type");
                return -1;
            }
            *((PyObject **) dst) = val;
            break;
        case ARG_BOOL:
            *((int *)dst) = val == Py_True;
            break;
        case ARG_BUF:
            ret = val_from_buffer((MDB_val *)dst, val);
            break;
        case ARG_STR: {
            MDB_val mv;
            if(! (ret = val_from_buffer(&mv, val))) {
                *((char **) dst) = mv.mv_data;
            }
            break;
        }
        case ARG_INT:
            if(parse_ulong(val, &l, py_int_max)) {
                ret = -1;
            } else {
                *((int *) dst) = l;
            }
            break;
        case ARG_SIZE:
            if(parse_ulong(val, &l, py_size_max)) {
                ret = -1;
            } else {
                *((size_t *) dst) = l;
            }
            break;
        }
    }
    return ret;
}


/**
 * Like PyArg_ParseTupleAndKeywords except types are specialized for this
 * module, keyword strings aren't dup'd every call and the code is >3x smaller.
 */
static int NOINLINE
parse_args(int valid, int specsize, const struct argspec *argspec,
           PyObject *args, PyObject *kwds, void *out)
{
    if(! valid) {
        err_invalid();
        return -1;
    }

    unsigned set = 0;
    unsigned i;
    if(args) {
        int size = PyTuple_GET_SIZE(args);
        if(size > specsize) {
            type_error("too many positional arguments.");
            return -1;
        }
        size = fmin(specsize, size);
        for(i = 0; i < size; i++) {
            if(parse_arg(argspec + i, PyTuple_GET_ITEM(args, i), out)) {
                return -1;
            }
            set |= 1 << i;
        }
    }

    if(kwds) {
        int size = PyDict_Size(kwds);
        int c = 0;

        for(i = 0; i < specsize && c != size; i++) {
            const struct argspec *spec = argspec + i;
            PyObject *kwd = string_tbl[spec->string_id];
            PyObject *val = PyDict_GetItem(kwds, kwd);
            if(val) {
                if(set & (1 << i)) {
                    PyErr_Format(PyExc_TypeError, "duplicate argument: %s",
                                 PyBytes_AS_STRING(kwd));
                    return -1;
                }
                if(parse_arg(spec, val, out)) {
                    return -1;
                }
                c++;
            }
        }

        if(c != size) {
            type_error("unrecognized keyword argument");
            return -1;
        }
    }
    return 0;
}


// --------------------------------------------------------
// Functionality shared between Transaction and Environment
// --------------------------------------------------------


static PyObject *
generic_get(int valid, MDB_txn *txn, DbObject *db, int buffers,
            BUFFER_TYPE **bptr, PyObject *args, PyObject *kwds)
{
    struct generic_get {
        MDB_val key;
        PyObject *default_;
        DbObject *db;
    } arg = {{0, 0}, Py_None, db};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(generic_get, key)},
        {ARG_OBJ, DEFAULT_S, OFFSET(generic_get, default_)},
        {ARG_DB, DB_S, OFFSET(generic_get, db)}
    };

    if(parse_args(valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.key.mv_data) {
        return type_error("key must be given.");
    }

    MDB_val val;
    int rc;
    UNLOCKED(rc, mdb_get(txn, arg.db->dbi, &arg.key, &val));
    if(rc) {
        if(rc == MDB_NOTFOUND) {
            Py_INCREF(arg.default_);
            return arg.default_;
        }
        return err_set("mdb_get", rc);
    }
    if(buffers) {
        return buffer_from_val(bptr, &val);
    }
    return string_from_val(&val);
}

static PyObject *
generic_put(int valid, MDB_txn *txn, DbObject *db,
            PyObject *args, PyObject *kwds)
{
    struct generic_put {
        MDB_val key;
        MDB_val value;
        int dupdata;
        int overwrite;
        int append;
        DbObject *db;
    } arg = {{0, 0}, {0, 0}, 0, 1, 0, db};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(generic_put, key)},
        {ARG_BUF, VALUE_S, OFFSET(generic_put, value)},
        {ARG_BOOL, DUPDATA_S, OFFSET(generic_put, dupdata)},
        {ARG_BOOL, OVERWRITE_S, OFFSET(generic_put, overwrite)},
        {ARG_BOOL, APPEND_S, OFFSET(generic_put, append)},
        {ARG_DB, DB_S, OFFSET(generic_put, db)}
    };

    if(parse_args(valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    int flags = 0;
    if(! arg.dupdata) {
        flags |= MDB_NODUPDATA;
    }
    if(! arg.overwrite) {
        flags |= MDB_NOOVERWRITE;
    }
    if(arg.append) {
        flags |= MDB_APPEND;
    }

    DEBUG("inserting '%.*s' (%d) -> '%.*s' (%d)",
        (int)arg.key.mv_size, (char *)arg.key.mv_data,
        (int)arg.key.mv_size,
        (int)arg.value.mv_size, (char *)arg.value.mv_data,
        (int)arg.value.mv_size)

    int rc;
    UNLOCKED(rc, mdb_put(txn, (arg.db)->dbi, &arg.key, &arg.value, flags));
    if(rc) {
        if(rc == MDB_KEYEXIST) {
            Py_RETURN_FALSE;
        }
        return err_set("mdb_put", rc);
    }
    Py_RETURN_TRUE;
}

static PyObject *
generic_delete(int valid, MDB_txn *txn, DbObject *db,
               PyObject *args, PyObject *kwds)
{
    struct generic_delete {
        MDB_val key;
        MDB_val val;
        DbObject *db;
    } arg = {{0, 0}, {0, 0}, db};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(generic_delete, key)},
        {ARG_BUF, VALUE_S, OFFSET(generic_delete, val)},
        {ARG_DB, DB_S, OFFSET(generic_delete, db)}
    };

    if(parse_args(valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }
    MDB_val *val_ptr = arg.val.mv_size ? &arg.val : NULL;
    int rc;
    UNLOCKED(rc, mdb_del(txn, arg.db->dbi, &arg.key, val_ptr));
    if(rc) {
        if(rc == MDB_NOTFOUND) {
             Py_RETURN_FALSE;
        }
        return err_set("mdb_del", rc);
    }
    Py_RETURN_TRUE;
}

static PyObject *
make_trans(EnvObject *env, TransObject *parent, int write, int buffers)
{
    DEBUG("make_trans(env=%p, parent=%p, write=%d, buffers=%d)",
        env, parent, write, buffers)
    if(! env->valid) {
        return err_invalid();
    }

    MDB_txn *parent_txn = NULL;
    if(parent) {
        if(! parent->valid) {
            return err_invalid();
        }
        parent_txn = parent->txn;
    }

    if(write && env->readonly) {
        return err_set("Cannot start write transaction with read-only env", 0);
    }

    TransObject *self = PyObject_New(TransObject, &PyTransaction_Type);
    if(! self) {
        return NULL;
    }

    int flags = (write && !env->readonly) ? 0 : MDB_RDONLY;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(env->env, parent_txn, flags, &self->txn));
    if(rc) {
        PyObject_Del(self);
        return err_set("mdb_txn_begin", rc);
    }

    OBJECT_INIT(self)
    LINK_CHILD(env, self)
    self->env = env;
    Py_INCREF(env);
    self->buffers = buffers;
    self->key_buf = NULL;
    return (PyObject *)self;
}

static PyObject *
make_cursor(DbObject *db, TransObject *trans)
{
    if(! trans->valid) {
        return err_invalid();
    }
    if(! db) {
        db = trans->env->main_db;
    }

    CursorObject *self = PyObject_New(CursorObject, &PyCursor_Type);
    int rc;
    UNLOCKED(rc, mdb_cursor_open(trans->txn, db->dbi, &self->curs));
    if(rc) {
        PyObject_Del(self);
        return err_set("mdb_cursor_open", rc);
    }

    DEBUG("sizeof cursor = %d", (int) sizeof *self)
    OBJECT_INIT(self)
    LINK_CHILD(trans, self)
    self->positioned = 0;
    self->key_buf = NULL;
    self->val_buf = NULL;
    self->key.mv_size = 0;
    self->val.mv_size = 0;
    self->item_tup = NULL;
    self->trans = trans;
    Py_INCREF(self->trans);
    return (PyObject *) self;
}

// ----------------------------
// Database
// ----------------------------

static DbObject *
db_from_name(EnvObject *env, MDB_txn *txn, const char *name,
             unsigned int flags)
{
    MDB_dbi dbi;
    int rc;

    UNLOCKED(rc, mdb_dbi_open(txn, name, flags, &dbi));
    if(rc) {
        err_set("mdb_dbi_open", rc);
        return NULL;
    }

    DbObject *dbo = PyObject_New(DbObject, &PyDatabase_Type);
    if(! dbo) {
        return NULL;
    }

    OBJECT_INIT(dbo)
    LINK_CHILD(env, dbo)
    dbo->env = env; // no refcount
    dbo->dbi = dbi;
    DEBUG("DbObject '%s' opened at %p", name, dbo)
    return dbo;
}


static DbObject *
txn_db_from_name(EnvObject *env, const char *name,
                 unsigned int flags)
{
    int rc;
    MDB_txn *txn;

    int begin_flags = (name == NULL || env->readonly) ? MDB_RDONLY : 0;
    UNLOCKED(rc, mdb_txn_begin(env->env, NULL, begin_flags, &txn));
    if(rc) {
        err_set("mdb_txn_begin", rc);
        return NULL;
    }

    DbObject *dbo = db_from_name(env, txn, name, flags);
    if(! dbo) {
        DROP_GIL
        mdb_txn_abort(txn);
        LOCK_GIL
        return NULL;
    }

    UNLOCKED(rc, mdb_txn_commit(txn));
    if(rc) {
        Py_DECREF(dbo);
        return err_set("mdb_txn_commit", rc);
    }
    return dbo;
}

static int
db_clear(DbObject *self)
{
    if(self->env) {
        UNLINK_CHILD(self->env, self)
        self->env = NULL;
    }
    self->valid = 0;
    return 0;
}

static void
db_dealloc(DbObject *self)
{
    db_clear(self);
    PyObject_Del(self);
}

PyTypeObject PyDatabase_Type = {
    PyObject_HEAD_INIT(NULL)
    .tp_basicsize = sizeof(DbObject),
    .tp_dealloc = (destructor) db_dealloc,
    .tp_clear = (inquiry) db_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_name = "_Database"
};


// -------------------------
// Environment.
// -------------------------

static int
env_clear(EnvObject *self)
{
    DEBUG("killing env..")
    if(self->env) {
        INVALIDATE(self)
        DEBUG("Closing env")
        DROP_GIL
        mdb_env_close(self->env);
        LOCK_GIL
        self->env = NULL;
    }
    if(self->main_db) {
        Py_CLEAR(self->main_db);
    }
    return 0;
}

static void
env_dealloc(EnvObject *self)
{
    env_clear(self);
    PyObject_Del(self);
}


static PyObject *
env_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    struct env_new {
        char *path;
        size_t map_size;
        int subdir;
        int readonly;
        int metasync;
        int sync;
        int map_async;
        int mode;
        int create;
        int writemap;
        int max_readers;
        int max_dbs;
    } arg = {NULL, 10485760, 1, 0, 1, 1, 0, 0644, 1, 0, 126, 0};

    static const struct argspec argspec[] = {
        {ARG_STR, PATH_S, OFFSET(env_new, path)},
        {ARG_SIZE, MAP_SIZE_S, OFFSET(env_new, map_size)},
        {ARG_BOOL, SUBDIR_S, OFFSET(env_new, subdir)},
        {ARG_BOOL, READONLY_S, OFFSET(env_new, readonly)},
        {ARG_BOOL, METASYNC_S, OFFSET(env_new, metasync)},
        {ARG_BOOL, SYNC_S, OFFSET(env_new, sync)},
        {ARG_BOOL, MAP_ASYNC_S, OFFSET(env_new, map_async)},
        {ARG_INT, MODE_S, OFFSET(env_new, mode)},
        {ARG_BOOL, CREATE_S, OFFSET(env_new, create)},
        {ARG_BOOL, WRITEMAP_S, OFFSET(env_new, writemap)},
        {ARG_INT, MAX_READERS_S, OFFSET(env_new, max_readers)},
        {ARG_INT, MAX_DBS_S, OFFSET(env_new, max_dbs)},
    };

    if(parse_args(1, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.path) {
        return type_error("'path' argument required");
    }

    EnvObject *self = PyObject_New(EnvObject, type);
    if(! self) {
        return NULL;
    }

    OBJECT_INIT(self)
    self->main_db = NULL;
    self->env = NULL;

    int rc;
    if((rc = mdb_env_create(&self->env))) {
        err_set("mdb_env_create", rc);
        goto fail;
    }

    if((rc = mdb_env_set_mapsize(self->env, arg.map_size))) {
        err_set("mdb_env_set_mapsize", rc);
        goto fail;
    }

    if((rc = mdb_env_set_maxreaders(self->env, arg.max_readers))) {
        err_set("mdb_env_set_maxreaders", rc);
        goto fail;
    }

    if((rc = mdb_env_set_maxdbs(self->env, arg.max_dbs))) {
        err_set("mdb_env_set_maxdbs", rc);
        goto fail;
    }

    if(arg.create && arg.subdir) {
        struct stat st;
        errno = 0;
        stat(arg.path, &st);
        if(errno == ENOENT) {
            if(mkdir(arg.path, 0700)) {
                PyErr_SetFromErrnoWithFilename(PyExc_OSError, arg.path);
                goto fail;
            }
        }
    }

    int flags = MDB_NOTLS;
    if(! arg.subdir) {
        flags |= MDB_NOSUBDIR;
    }
    if(arg.readonly) {
        flags |= MDB_RDONLY;
    }
    self->readonly = arg.readonly;
    if(! arg.metasync) {
        flags |= MDB_NOMETASYNC;
    }
    if(! arg.sync) {
        flags |= MDB_NOSYNC;
    }
    if(arg.map_async) {
        flags |= MDB_MAPASYNC;
    }
    if(arg.writemap) {
        flags |= MDB_WRITEMAP;
    }

    DEBUG("mdb_env_open(%p, '%s', %d, %o);", self->env, arg.path, flags, arg.mode)
    UNLOCKED(rc, mdb_env_open(self->env, arg.path, flags, arg.mode));
    if(rc) {
        err_set(arg.path, rc);
        goto fail;
    }

    self->main_db = txn_db_from_name(self, NULL, 0);
    if(self->main_db) {
        self->valid = 1;
        DEBUG("EnvObject '%s' opened at %p", arg.path, self)
        return (PyObject *) self;
    }

fail:
    DEBUG("initialization failed")
    if(self) {
        env_dealloc(self);
    }
    return NULL;
}

static PyObject *
env_begin(EnvObject *self, PyObject *args, PyObject *kwds)
{
    struct env_begin {
        int buffers;
        int write;
        TransObject *parent;
    } arg = {0, 0, NULL};

    static const struct argspec argspec[] = {
        {ARG_BOOL, BUFFERS_S, OFFSET(env_begin, buffers)},
        {ARG_BOOL, WRITE_S, OFFSET(env_begin, write)},
        {ARG_TRANS, PARENT_S, OFFSET(env_begin, parent)},
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }
    return make_trans(self, arg.parent, arg.write, arg.buffers);
}

static PyObject *
env_close(EnvObject *self)
{
    if(self->valid) {
        INVALIDATE(self)
        self->valid = 0;
        DEBUG("Closing env")
        DROP_GIL
        mdb_env_close(self->env);
        LOCK_GIL
        self->env = NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
env_copy(EnvObject *self, PyObject *args)
{
    if(! self->valid) {
        return err_invalid();
    }

    PyObject *path;
    if(! PyArg_ParseTuple(args, "|O:copy", &path)) {
        return NULL;
    }
    return NULL;
}

static PyObject *
env_info(EnvObject *self)
{
    static const struct dict_field fields[] = {
        { TYPE_ADDR, "map_addr",        offsetof(MDB_envinfo, me_mapaddr) },
        { TYPE_SIZE, "map_size",        offsetof(MDB_envinfo, me_mapsize) },
        { TYPE_SIZE, "last_pgno",       offsetof(MDB_envinfo, me_last_pgno) },
        { TYPE_SIZE, "last_txnid",      offsetof(MDB_envinfo, me_last_txnid) },
        { TYPE_UINT, "max_readers",     offsetof(MDB_envinfo, me_maxreaders) },
        { TYPE_UINT, "num_readers",     offsetof(MDB_envinfo, me_numreaders) },
        { TYPE_EOF, NULL, 0 }
    };

    if(! self->valid) {
        return err_invalid();
    }

    MDB_envinfo info;
    int rc;
    UNLOCKED(rc, mdb_env_info(self->env, &info));
    if(rc) {
        err_set("mdb_env_info", rc);
        return NULL;
    }
    return dict_from_fields(&info, fields);
}


static PyObject *
env_open_db(EnvObject *self, PyObject *args, PyObject *kwds)
{
    struct env_open_db {
        const char *name;
        TransObject *txn;
        int reverse_key;
        int dupsort;
        int create;
    } arg = {NULL, NULL, 0, 0, 1};

    static const struct argspec argspec[] = {
        {ARG_STR, NAME_S, OFFSET(env_open_db, name)},
        {ARG_TRANS, TXN_S, OFFSET(env_open_db, txn)},
        {ARG_BOOL, REVERSE_KEY_S, OFFSET(env_open_db, reverse_key)},
        {ARG_BOOL, DUPSORT_S, OFFSET(env_open_db, dupsort)},
        {ARG_BOOL, CREATE_S, OFFSET(env_open_db, create)},
    };

    if(parse_args(1, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    int flags = 0;
    if(arg.reverse_key) {
        flags |= MDB_REVERSEKEY;
    }
    if(arg.dupsort) {
        flags |= MDB_DUPSORT;
    }
    if(arg.create) {
        flags |= MDB_CREATE;
    }

    if(arg.txn) {
        return (PyObject *) db_from_name(self, arg.txn->txn, arg.name, flags);
    } else {
        return (PyObject *) txn_db_from_name(self, arg.name, flags);
    }
}


static PyObject *
env_path(EnvObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }

    const char *path;
    int rc;
    if((rc = mdb_env_get_path(self->env, &path))) {
        return err_set("mdb_env_get_path", rc);
    }
    return PyUnicode_FromString(path);
}


static PyObject *
env_stat(EnvObject *self)
{
    static const struct dict_field fields[] = {
        { TYPE_UINT, "psize",           offsetof(MDB_stat, ms_psize) },
        { TYPE_UINT, "depth",           offsetof(MDB_stat, ms_depth) },
        { TYPE_SIZE, "branch_pages",    offsetof(MDB_stat, ms_branch_pages) },
        { TYPE_SIZE, "leaf_pages",      offsetof(MDB_stat, ms_leaf_pages) },
        { TYPE_SIZE, "overflow_pages",  offsetof(MDB_stat, ms_overflow_pages) },
        { TYPE_SIZE, "entries",         offsetof(MDB_stat, ms_entries) },
        { TYPE_EOF, NULL, 0 }
    };

    if(! self->valid) {
        return err_invalid();
    }

    MDB_stat st;
    int rc;
    UNLOCKED(rc, mdb_env_stat(self->env, &st));
    if(rc) {
        err_set("mdb_env_stat", rc);
        return NULL;
    }
    return dict_from_fields(&st, fields);
}

static PyObject *
env_sync(EnvObject *self, PyObject *args)
{
    struct env_sync {
        int force;
    } arg = {0};

    static const struct argspec argspec[] = {
        {ARG_BOOL, FORCE_S, OFFSET(env_sync, force)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, NULL, &arg)) {
        return NULL;
    }

    int rc;
    UNLOCKED(rc, mdb_env_sync(self->env, arg.force));
    if(rc) {
        return err_set("mdb_env_sync", rc);
    }
    Py_RETURN_NONE;
}

static PyObject *
env_get(EnvObject *self, PyObject *args, PyObject *kwds)
{
    if(! self->valid) {
        return err_invalid();
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, MDB_RDONLY, &txn));
    if(rc) {
        return err_set("mdb_txn_begin", rc);
    }

    PyObject *ret = generic_get(1, txn, self->main_db, 0, NULL, args, kwds);
    DROP_GIL
    mdb_txn_abort(txn);
    LOCK_GIL
    return ret;
}


static PyObject *
env_gets(EnvObject *self, PyObject *args, PyObject *kwds)
{
    struct env_gets {
        PyObject *keys;
        DbObject *db;
    } arg = {NULL, self->main_db};

    static const struct argspec argspec[] = {
        {ARG_OBJ, KEYS_S, OFFSET(env_gets, keys)},
        {ARG_DB, DB_S, OFFSET(env_gets, db)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.keys) {
        return type_error("keys must be given");
    }

    PyObject *iter = PyObject_GetIter(arg.keys);
    if(! iter) {
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if(! dict) {
        Py_DECREF(iter);
        return NULL;
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, MDB_RDONLY, &txn));
    if(rc) {
        Py_DECREF(iter);
        Py_DECREF(dict);
        return err_set("mdb_txn_begin", rc);
    }

    PyObject *key_obj;
    MDB_val key;
    MDB_val val;

    while((key_obj = PyIter_Next(iter)) != NULL) {
        if(val_from_buffer(&key, key_obj)) {
            break;
        }

        UNLOCKED(rc, mdb_get(txn, arg.db->dbi, &key, &val));
        if(rc == 0) {
            PyObject *val_obj = string_from_val(&val);
            if(! val_obj) {
                break;
            }
            rc = PyDict_SetItem(dict, key_obj, val_obj);
            Py_DECREF(val_obj);
            if(rc) {
                break;
            }
        } else if(rc != MDB_NOTFOUND) {
            err_set("mdb_get", rc);
            break;
        }
        Py_DECREF(key_obj);
    }

    DROP_GIL
    mdb_txn_abort(txn);
    LOCK_GIL
    Py_DECREF(iter);
    Py_XDECREF(key_obj);
    if(PyErr_Occurred()) {
        Py_CLEAR(dict);
    }
    return dict;
}

static PyObject *
env_put(EnvObject *self, PyObject *args, PyObject *kwds)
{
    if(! self->valid) {
        return err_invalid();
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, 0, &txn));
    if(rc) {
        return err_set("mdb_txn_begin", rc);
    }

    PyObject *ret = generic_put(1, txn, self->main_db, args, kwds);
    if(ret) {
        UNLOCKED(rc, mdb_txn_commit(txn));
        if(rc) {
            Py_DECREF(ret);
            ret = err_set("mdb_txn_commit", rc);
        }
    } else {
        DROP_GIL
        mdb_txn_abort(txn);
        LOCK_GIL
    }
    return ret;
}

static PyObject *
env_puts(EnvObject *self, PyObject *args, PyObject *kwds)
{
    struct env_puts {
        PyObject *items;
        int dupdata;
        int overwrite;
        int append;
        DbObject *db;
    } arg = {NULL, 0, 1, 0, self->main_db};

    static const struct argspec argspec[] = {
        {ARG_OBJ, ITEMS_S, OFFSET(env_puts, items)},
        {ARG_BOOL, DUPDATA_S, OFFSET(env_puts, dupdata)},
        {ARG_BOOL, OVERWRITE_S, OFFSET(env_puts, overwrite)},
        {ARG_BOOL, APPEND_S, OFFSET(env_puts, append)},
        {ARG_DB, DB_S, OFFSET(env_puts, db)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.items) {
        return type_error("items must be given");
    }

    PyObject *iter;
    if(Py_TYPE(arg.items) == &PyDict_Type) {
        iter = PyObject_CallMethodObjArgs(
            arg.items, string_tbl[ITERITEMS_S], NULL);
    } else {
        iter = PyObject_GetIter(arg.items);
    }
    if(! iter) {
        return NULL;
    }

    PyObject *list = PyList_New(0);
    if(! list) {
        Py_DECREF(iter);
        return NULL;
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, 0, &txn));
    if(rc) {
        Py_DECREF(iter);
        Py_DECREF(list);
        return err_set("mdb_txn_begin", rc);
    }

    int flags = 0;
    if(! arg.dupdata) {
        flags |= MDB_NODUPDATA;
    }
    if(! arg.overwrite) {
        flags |= MDB_NOOVERWRITE;
    }
    if(arg.append) {
        flags |= MDB_APPEND;
    }

    PyObject *item;
    MDB_val key;
    MDB_val val;

    while((item = PyIter_Next(iter)) != NULL) {
        if(! (PyTuple_Check(item) && PyTuple_GET_SIZE(item) == 2)) {
            Py_DECREF(item);
            type_error("puts() element type must be a 2-tuple.");
            break;
        }

        if(val_from_buffer(&key, PyTuple_GET_ITEM(item, 0)) ||
           val_from_buffer(&val, PyTuple_GET_ITEM(item, 1))) {
            Py_DECREF(item);
            break;
        }

        DEBUG("inserting '%.*s' (%d) -> '%.*s' (%d)",
            (int)key.mv_size, (char *)key.mv_data, (int)key.mv_size,
            (int)val.mv_size, (char *)val.mv_data, (int)val.mv_size)
        UNLOCKED(rc, mdb_put(txn, arg.db->dbi, &key, &val, flags));
        Py_DECREF(item);

        PyObject *res;
        if(rc == 0) {
            res = Py_True;
        } else if(rc == MDB_KEYEXIST) {
            res = Py_False;
        } else {
            err_set("mdb_put", rc);
            break;
        }

        if(PyList_Append(list, res)) {
            break;
        }
    }

    DEBUG("got this far; list size now %d", (int) PyList_GET_SIZE(list))
    Py_DECREF(iter);
    if(PyErr_Occurred()) {
        DEBUG("abort")
        DROP_GIL
        mdb_txn_abort(txn);
        LOCK_GIL
        Py_CLEAR(list);
    } else {
        DEBUG("commit")
        UNLOCKED(rc, mdb_txn_commit(txn));
        if(rc) {
            err_set("mdb_txn_commit", rc);
            Py_CLEAR(list);
        }
    }
    return list;
}

static PyObject *
env_delete(EnvObject *self, PyObject *args, PyObject *kwds)
{
    if(! self->valid) {
        return err_invalid();
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, 0, &txn));
    if(rc) {
        return err_set("mdb_txn_begin", rc);
    }

    PyObject *ret = generic_delete(1, txn, self->main_db, args, kwds);
    if(ret) {
        UNLOCKED(rc, mdb_txn_commit(txn));
        if(rc) {
            Py_DECREF(ret);
            ret = err_set("mdb_txn_commit", rc);
        }
    } else {
        DROP_GIL
        mdb_txn_abort(txn);
        LOCK_GIL
    }
    return ret;
}

static PyObject *
env_deletes(EnvObject *self, PyObject *args, PyObject *kwds)
{
    struct env_deletes {
        PyObject *keys;
        DbObject *db;
    } arg = {NULL, self->main_db};

    static const struct argspec argspec[] = {
        {ARG_OBJ, KEYS_S, OFFSET(env_deletes, keys)},
        {ARG_DB, DB_S, OFFSET(env_deletes, db)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.keys) {
        return type_error("keys must be given");
    }

    PyObject *iter = PyObject_GetIter(arg.keys);
    if(! iter) {
        return NULL;
    }

    PyObject *list = PyList_New(0);
    if(! list) {
        Py_DECREF(iter);
        return NULL;
    }

    MDB_txn *txn;
    int rc;
    UNLOCKED(rc, mdb_txn_begin(self->env, NULL, 0, &txn));
    if(rc) {
        return err_set("mdb_txn_begin", rc);
    }

    PyObject *key_obj;
    MDB_val key;
    while((key_obj = PyIter_Next(iter)) != NULL) {
        if(val_from_buffer(&key, key_obj)) {
            break;
        }

        UNLOCKED(rc, mdb_del(txn, arg.db->dbi, &key, NULL));
        Py_DECREF(key_obj);

        PyObject *res;
        if(rc == 0) {
            res = Py_True;
        } else if(rc == MDB_NOTFOUND) {
            res = Py_False;
        } else {
            err_set("mdb_del", rc);
            break;
        }
        if(PyList_Append(list, res)) {
            break;
        }
    }

    if(PyErr_Occurred()) {
        DROP_GIL
        mdb_txn_abort(txn);
        LOCK_GIL
        Py_CLEAR(list);
    } else {
        UNLOCKED(rc, mdb_txn_commit(txn));
        if(rc) {
            Py_CLEAR(list);
            err_set("mdb_txn_commit", rc);
        }
    }
    return list;
}

static PyObject *
env_cursor(EnvObject *self, PyObject *args, PyObject *kwds)
{
    // TODO: there is no benefit to this implementation. Once reader freelist
    // is supported, should be possible to make meaningful optimizaiton here.
    struct env_cursor {
        int buffers;
        DbObject *db;
    } arg = { 0, self->main_db };

    static const struct argspec argspec[] = {
        {ARG_BOOL, BUFFERS_S, OFFSET(env_cursor, buffers)},
        {ARG_DB, DB_S, OFFSET(env_cursor, db)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    PyObject *trans = make_trans(self, NULL, 0, arg.buffers);
    if(! trans) {
        return NULL;
    }

    PyObject *cursor = make_cursor(arg.db, (TransObject *) trans);
    Py_DECREF(trans);
    return cursor;
}

static struct PyMethodDef env_methods[] = {
    {"begin", (PyCFunction)env_begin, METH_VARARGS|METH_KEYWORDS},
    {"close", (PyCFunction)env_close, METH_NOARGS},
    {"copy", (PyCFunction)env_copy, METH_VARARGS},
    {"info", (PyCFunction)env_info, METH_NOARGS},
    {"open_db", (PyCFunction)env_open_db, METH_VARARGS|METH_KEYWORDS},
    {"path", (PyCFunction)env_path, METH_NOARGS},
    {"stat", (PyCFunction)env_stat, METH_NOARGS},
    {"sync", (PyCFunction)env_sync, METH_VARARGS},
    {"get", (PyCFunction)env_get, METH_VARARGS|METH_KEYWORDS},
    {"gets", (PyCFunction)env_gets, METH_VARARGS|METH_KEYWORDS},
    {"put", (PyCFunction)env_put, METH_VARARGS|METH_KEYWORDS},
    {"puts", (PyCFunction)env_puts, METH_VARARGS|METH_KEYWORDS},
    {"delete", (PyCFunction)env_delete, METH_VARARGS|METH_KEYWORDS},
    {"deletes", (PyCFunction)env_deletes, METH_VARARGS|METH_KEYWORDS},
    {"cursor", (PyCFunction)env_cursor, METH_VARARGS|METH_KEYWORDS},
    {NULL, NULL}
};


PyTypeObject PyEnvironment_Type = {
    PyObject_HEAD_INIT(0)
    .tp_basicsize = sizeof(EnvObject),
    .tp_dealloc = (destructor) env_dealloc,
    .tp_clear = (inquiry) env_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = env_methods,
    .tp_name = "Environment",
    .tp_new = env_new,
};


// ==================================
/// cursors
// ==================================

static int
cursor_clear(CursorObject *self)
{
    if(self->valid) {
        INVALIDATE(self)
        UNLINK_CHILD(self->trans, self)
        DROP_GIL
        mdb_cursor_close(self->curs);
        LOCK_GIL
        self->valid = 0;
    }
    if(self->key_buf) {
        SET_BUFFER(self->key_buf, "", 0);
        Py_CLEAR(self->key_buf);
    }
    if(self->val_buf) {
        SET_BUFFER(self->val_buf, "", 0);
        Py_CLEAR(self->val_buf);
    }
    if(self->item_tup) {
        Py_CLEAR(self->item_tup);
    }
    Py_CLEAR(self->trans);
    return 0;
}

static void
cursor_dealloc(CursorObject *self)
{
    DEBUG("destroying cursor")
    cursor_clear(self);
    PyObject_Del(self);
}

static PyObject *
cursor_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    struct cursor_new {
        DbObject *db;
        TransObject *trans;
    } arg = {NULL, NULL};

    static const struct argspec argspec[] = {
        {ARG_DB, DB_S, OFFSET(cursor_new, db)},
        {ARG_TRANS, TXN_S, OFFSET(cursor_new, trans)}
    };

    if(parse_args(1, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! (arg.db && arg.trans)) {
        return type_error("db and transaction parameters required.");
    }
    return make_cursor(arg.db, arg.trans);
}

static PyObject *
cursor_count(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }

    size_t count;
    int rc;
    UNLOCKED(rc, mdb_cursor_count(self->curs, &count));
    if(rc) {
        return err_set("mdb_cursor_count", rc);
    }
    return PyLong_FromUnsignedLongLong(count);
}


static int
_cursor_get_c(CursorObject *self, enum MDB_cursor_op op)
{
    int rc;
    UNLOCKED(rc, mdb_cursor_get(self->curs, &self->key, &self->val, op));
    self->positioned = rc == 0;
    if(rc) {
        self->key.mv_size = 0;
        self->val.mv_size = 0;
        if(rc != MDB_NOTFOUND) {
            if(! (rc == EINVAL && op == MDB_GET_CURRENT)) {
                err_set("mdb_cursor_get", rc);
                return -1;
            }
        }
    }
    return 0;
}


static PyObject *
_cursor_get(CursorObject *self, enum MDB_cursor_op op)
{
    if(_cursor_get_c(self, op)) {
        return NULL;
    }
    PyObject *res = self->positioned ? Py_True : Py_False;
    Py_INCREF(res);
    return res;
}


static PyObject *
cursor_delete(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    PyObject *ret = Py_False;
    if(self->positioned) {
        DEBUG("deleting key '%.*s'",
              (int) self->key.mv_size,
              (char*) self->key.mv_data)
        int rc;
        UNLOCKED(rc, mdb_cursor_del(self->curs, 0));
        if(rc) {
            return err_set("mdb_cursor_del", rc);
        }
        ret = Py_True;
        _cursor_get_c(self, MDB_GET_CURRENT);
    }
    Py_INCREF(ret);
    return ret;
}


static PyObject *
cursor_first(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    return _cursor_get(self, MDB_FIRST);
}


static PyObject *
cursor_value(CursorObject *self);


static PyObject *
cursor_get(CursorObject *self, PyObject *args, PyObject *kwds)
{
    if(! self->valid) {
        return err_invalid();
    }

    struct cursor_get {
        MDB_val key;
        PyObject *default_;
    } arg = {{0, 0}, Py_None};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(cursor_get, key)},
        {ARG_OBJ, DEFAULT_S, OFFSET(cursor_get, default_)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! arg.key.mv_data) {
        return type_error("key must be given.");
    }

    self->key = arg.key;
    if(_cursor_get_c(self, MDB_SET_KEY)) {
        return NULL;
    }
    if(! self->positioned) {
        Py_INCREF(arg.default_);
        return arg.default_;
    }
    return cursor_value(self);
}


static PyObject *
cursor_item(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(self->trans->buffers) {
        if(! buffer_from_val(&self->key_buf, &self->key)) {
            return NULL;
        }
        if(! buffer_from_val(&self->val_buf, &self->val)) {
            return NULL;
        }
        if(! self->item_tup) {
            self->item_tup = PyTuple_Pack(2, self->key_buf, self->val_buf);
        }
        Py_INCREF(self->item_tup);
        return self->item_tup;
    }

    PyObject *key = string_from_val(&self->key);
    if(! key) {
        return NULL;
    }
    PyObject *val = string_from_val(&self->val);
    if(! val) {
        Py_DECREF(key);
        return NULL;
    }
    PyObject *tup = PyTuple_Pack(2, key, val);
    if(! tup) {
        Py_DECREF(key);
        Py_DECREF(val);
        return NULL;
    }
    return tup;
}

static PyObject *
cursor_key(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(self->trans->buffers) {
        if(! buffer_from_val(&self->key_buf, &self->key)) {
            return NULL;
        }
        Py_INCREF(self->key_buf);
        return (PyObject *) self->key_buf;
    }
    return string_from_val(&self->key);
}

static PyObject *
cursor_last(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    return _cursor_get(self, MDB_LAST);
}

static PyObject *
cursor_next(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    return _cursor_get(self, MDB_NEXT);
}

static PyObject *
cursor_prev(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    return _cursor_get(self, MDB_PREV);
}

static PyObject *
cursor_put(CursorObject *self, PyObject *args, PyObject *kwds)
{
    struct cursor_put {
        MDB_val key;
        MDB_val val;
        int dupdata;
        int overwrite;
        int append;
    } arg = {{0, 0}, {0, 0}, 0, 1, 0};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(cursor_put, key)},
        {ARG_BUF, VALUE_S, OFFSET(cursor_put, val)},
        {ARG_BOOL, DUPDATA_S, OFFSET(cursor_put, dupdata)},
        {ARG_BOOL, OVERWRITE_S, OFFSET(cursor_put, overwrite)},
        {ARG_BOOL, APPEND_S, OFFSET(cursor_put, append)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    int flags = 0;
    if(! arg.dupdata) {
        flags |= MDB_NODUPDATA;
    }
    if(! arg.overwrite) {
        flags |= MDB_NOOVERWRITE;
    }
    if(! arg.append) {
        flags |= MDB_APPEND;
    }

    int rc;
    UNLOCKED(rc, mdb_cursor_put(self->curs, &arg.key, &arg.val, flags));
    if(rc) {
        if(rc == MDB_KEYEXIST) {
            Py_RETURN_FALSE;
        }
        return err_set("mdb_put", rc);
    }
    Py_RETURN_TRUE;
}


static PyObject *
cursor_set_key(CursorObject *self, PyObject *arg)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(val_from_buffer(&self->key, arg)) {
        return NULL;
    }
    return _cursor_get(self, MDB_SET_KEY);
}

static PyObject *
cursor_set_range(CursorObject *self, PyObject *arg)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(val_from_buffer(&self->key, arg)) {
        return NULL;
    }
    if(self->key.mv_size) {
        return _cursor_get(self, MDB_SET_RANGE);
    }
    return _cursor_get(self, MDB_FIRST);
}

static PyObject *
cursor_value(CursorObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(self->trans->buffers) {
        if(! buffer_from_val(&self->val_buf, &self->val)) {
            return NULL;
        }
        Py_INCREF(self->val_buf);
        return (PyObject *) self->val_buf;
    }
    return string_from_val(&self->val);
}

// ==================================
// Cursor iteration
// ==================================

static PyObject *
iter_from_args(CursorObject *self, PyObject *args, PyObject *kwds,
               enum MDB_cursor_op pos_op, enum MDB_cursor_op op)
{
    struct iter_from_args {
        int keys;
        int values;
    } arg = {1, 1};

    static const struct argspec argspec[] = {
        {ARG_BOOL, KEYS_S, OFFSET(iter_from_args, keys)},
        {ARG_BOOL, VALUES_S, OFFSET(iter_from_args, values)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }

    if(! self->positioned) {
        if(_cursor_get_c(self, pos_op)) {
            return NULL;
        }
    }

    IterObject *iter = PyObject_New(IterObject, &PyIterator_Type);
    if(! iter) {
        return NULL;
    }

    if(! arg.values) {
        iter->val_func = (void *)cursor_key;
    } else if(! arg.keys) {
        iter->val_func = (void *)cursor_value;
    } else {
        iter->val_func = (void *)cursor_item;
    }

    iter->curs = self;
    Py_INCREF(self);
    iter->started = 0;
    iter->op = op;
    return (PyObject *) iter;
}

static PyObject *
cursor_iter(CursorObject *self)
{
    return iter_from_args(self, NULL, NULL, MDB_FIRST, MDB_NEXT);
}

static PyObject *
cursor_iternext(CursorObject *self, PyObject *args, PyObject *kwargs)
{
    return iter_from_args(self, args, kwargs, MDB_FIRST, MDB_NEXT);
}

static PyObject *
cursor_iterprev(CursorObject *self, PyObject *args, PyObject *kwargs)
{
    return iter_from_args(self, args, kwargs, MDB_LAST, MDB_PREV);
}

static PyObject *
cursor_iter_from(CursorObject *self, PyObject *args)
{
    struct cursor_iter_from {
        MDB_val key;
        int reverse;
    } arg = {{0, 0}, 0};

    static const struct argspec argspec[] = {
        {ARG_BUF, KEY_S, OFFSET(cursor_iter_from, key)},
        {ARG_BOOL, REVERSE_S, OFFSET(cursor_iter_from, reverse)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, NULL, &arg)) {
        return NULL;
    }

    int rc;
    if((! arg.key.mv_size) && (! arg.reverse)) {
        rc = _cursor_get_c(self, MDB_FIRST);
    } else {
        self->key = arg.key;
        rc = _cursor_get_c(self, MDB_SET_RANGE);
    }

    if(rc) {
        return NULL;
    }

    enum MDB_cursor_op op = MDB_NEXT;
    if(arg.reverse) {
        op = MDB_PREV;
        if(! self->positioned) {
            if(_cursor_get_c(self, MDB_LAST)) {
                return NULL;
            }
        }
    }

    DEBUG("positioned? %d", self->positioned)
    IterObject *iter = PyObject_New(IterObject, &PyIterator_Type);
    if(iter) {
        iter->val_func = (void *)cursor_item;
        iter->curs = self;
        Py_INCREF(self);
        iter->started = 0;
        iter->op = op;
    }
    return (PyObject *) iter;
}

static struct PyMethodDef cursor_methods[] = {
    {"count", (PyCFunction)cursor_count, METH_NOARGS},
    {"delete", (PyCFunction)cursor_delete, METH_NOARGS},
    {"first", (PyCFunction)cursor_first, METH_NOARGS},
    {"get", (PyCFunction)cursor_get, METH_VARARGS|METH_KEYWORDS},
    {"item", (PyCFunction)cursor_item, METH_NOARGS},
    {"iternext", (PyCFunction)cursor_iternext, METH_VARARGS|METH_KEYWORDS},
    {"iterprev", (PyCFunction)cursor_iterprev, METH_VARARGS|METH_KEYWORDS},
    {"key", (PyCFunction)cursor_key, METH_NOARGS},
    {"last", (PyCFunction)cursor_last, METH_NOARGS},
    {"next", (PyCFunction)cursor_next, METH_NOARGS},
    {"prev", (PyCFunction)cursor_prev, METH_NOARGS},
    {"put", (PyCFunction)cursor_put, METH_VARARGS|METH_KEYWORDS},
    {"set_key", (PyCFunction)cursor_set_key, METH_O},
    {"set_range", (PyCFunction)cursor_set_range, METH_O},
    {"value", (PyCFunction)cursor_value, METH_NOARGS},
    {"_iter_from", (PyCFunction)cursor_iter_from, METH_VARARGS},
    {NULL, NULL}
};

PyTypeObject PyCursor_Type = {
    PyObject_HEAD_INIT(0)
    .tp_basicsize = sizeof(CursorObject),
    .tp_dealloc = (destructor) cursor_dealloc,
    .tp_clear = (inquiry) cursor_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_iter = (getiterfunc)cursor_iter,
    .tp_methods = cursor_methods,
    .tp_name = "Cursor",
    .tp_new = cursor_new
};


// ---------
// Iterators
// ---------

static void
iter_dealloc(IterObject *self)
{
    DEBUG("destroying iterator")
    Py_CLEAR(self->curs);
    PyObject_Del(self);
}


static PyObject *
iter_iter(IterObject *self)
{
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
iter_next(IterObject *self)
{
    if(! self->curs->valid) {
        return err_invalid();
    }
    if(! self->curs->positioned) {
        return NULL;
    }
    if(self->started) {
        if(_cursor_get_c(self->curs, self->op)) {
            return NULL;
        }
        if(! self->curs->positioned) {
            return NULL;
        }
    }
    PyObject *val = self->val_func(self->curs);
    self->started = 1;
    return val;
}

static struct PyMethodDef iter_methods[] = {
    {"next", (PyCFunction)cursor_next, METH_NOARGS},
    {NULL, NULL}
};

PyTypeObject PyIterator_Type = {
    PyObject_HEAD_INIT(0)
    .tp_basicsize = sizeof(IterObject),
    .tp_dealloc = (destructor) iter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_iter = (getiterfunc)iter_iter,
    .tp_iternext = (iternextfunc)iter_next,
    .tp_methods = iter_methods,
    .tp_name = "Iterator"
};


// ------------
// Transactions
// ------------

static int
trans_clear(TransObject *self)
{
    if(self->valid) {
        INVALIDATE(self)
        if(self->txn) {
            DEBUG("aborting")
            DROP_GIL
            mdb_txn_abort(self->txn);
            LOCK_GIL
            self->txn = NULL;
        }
        self->valid = 0;
    }
    UNLINK_CHILD(self->env, self)
    Py_CLEAR(self->env);
    return 0;
}


static void
trans_dealloc(TransObject *self)
{
    DEBUG("deleting trans")
    trans_clear(self);
    PyObject_Del(self);
}


static PyObject *
trans_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    struct trans_new {
        EnvObject *env;
        TransObject *parent;
        int write;
        int buffers;
    } arg = {NULL, NULL, 0, 0};

    static const struct argspec argspec[] = {
        {ARG_TRANS, TXN_S, OFFSET(trans_new, env)},
        {ARG_TRANS, PARENT_S, OFFSET(trans_new, parent)},
        {ARG_BOOL, WRITE_S, OFFSET(trans_new, write)},
        {ARG_BOOL, BUFFERS_S, OFFSET(trans_new, buffers)}
    };

    if(! arg.env) {
        return type_error("'env' argument required");
    }
    if(parse_args(1, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }
    return make_trans(arg.env, arg.parent, arg.write, arg.buffers);
}


static PyObject *
trans_abort(TransObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    DEBUG("aborting")
    INVALIDATE(self)
    DROP_GIL
    mdb_txn_abort(self->txn);
    LOCK_GIL
    self->txn = NULL;
    self->valid = 0;
    Py_RETURN_NONE;
}

static PyObject *
trans_commit(TransObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    DEBUG("committing")
    INVALIDATE(self)
    int rc;
    UNLOCKED(rc, mdb_txn_commit(self->txn));
    self->txn = NULL;
    self->valid = 0;
    if(rc) {
        return err_set("mdb_txn_commit", rc);
    }
    Py_RETURN_NONE;
}


static PyObject *
trans_cursor(TransObject *self, PyObject *args, PyObject *kwds)
{
    struct trans_cursor {
        DbObject *db;
    } arg = { 0 };

    static const struct argspec argspec[] = {
        {ARG_DB, DB_S, OFFSET(trans_cursor, db)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }
    if(! arg.db) {
        arg.db = self->env->main_db;
    }
    return make_cursor(arg.db, self);
}


static PyObject *
trans_delete(TransObject *self, PyObject *args, PyObject *kwds)
{
    return generic_delete(self->valid, self->txn, self->env->main_db,
                          args, kwds);
}


static PyObject *
trans_drop(TransObject *self, PyObject *args, PyObject *kwds)
{
    struct trans_drop {
        DbObject *db;
        int delete;
    } arg = { NULL, 1 };

    static const struct argspec argspec[] = {
        {ARG_DB, DB_S, OFFSET(trans_drop, db)},
        {ARG_BOOL, DELETE_S, OFFSET(trans_drop, delete)}
    };

    if(parse_args(self->valid, SPECSIZE(), argspec, args, kwds, &arg)) {
        return NULL;
    }
    if(! arg.db) {
        return type_error("'db' argument required.");
    }

    int rc;
    UNLOCKED(rc, mdb_drop(self->txn, arg.db->dbi, arg.delete));
    if(rc) {
        return err_set("mdb_drop", rc);
    }
    Py_RETURN_NONE;
}

static PyObject *
trans_get(TransObject *self, PyObject *args, PyObject *kwds)
{
    return generic_get(self->valid, self->txn, self->env->main_db,
                       self->buffers, &self->key_buf, args, kwds);
}

static PyObject *
trans_put(TransObject *self, PyObject *args, PyObject *kwds)
{
    return generic_put(self->valid, self->txn, self->env->main_db,
                       args, kwds);
}

static PyObject *trans_enter(TransObject *self)
{
    if(! self->valid) {
        return err_invalid();
    }
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *trans_exit(TransObject *self, PyObject *args)
{
    if(! self->valid) {
        return err_invalid();
    }
    if(PyTuple_GET_ITEM(args, 0) == Py_None) {
        return trans_commit(self);
    } else {
        return trans_abort(self);
    }
}


static struct PyMethodDef trans_methods[] = {
    {"__enter__", (PyCFunction)trans_enter, METH_NOARGS},
    {"__exit__", (PyCFunction)trans_exit, METH_VARARGS},
    {"abort", (PyCFunction)trans_abort, METH_NOARGS},
    {"commit", (PyCFunction)trans_commit, METH_NOARGS},
    {"cursor", (PyCFunction)trans_cursor, METH_VARARGS|METH_KEYWORDS},
    {"delete", (PyCFunction)trans_delete, METH_VARARGS|METH_KEYWORDS},
    {"drop", (PyCFunction)trans_drop, METH_VARARGS|METH_KEYWORDS},
    {"get", (PyCFunction)trans_get, METH_VARARGS|METH_KEYWORDS},
    {"put", (PyCFunction)trans_put, METH_VARARGS|METH_KEYWORDS},
    {NULL, NULL}
};

PyTypeObject PyTransaction_Type = {
    PyObject_HEAD_INIT(0)
    .tp_basicsize = sizeof(TransObject),
    .tp_dealloc = (destructor) trans_dealloc,
    .tp_clear = (inquiry) trans_clear,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = trans_methods,
    .tp_name = "Transaction",
    .tp_new = trans_new,
};


static int add_type(PyObject *mod, PyTypeObject *type)
{
    if(PyType_Ready(type)) {
        return -1;
    }
    return PyObject_SetAttrString(mod, type->tp_name, (PyObject *)type);
}


static PyObject *
enable_drop_gil(void)
{
    drop_gil = 1;
    Py_RETURN_NONE;
}


static struct PyMethodDef module_methods[] = {
    {"enable_drop_gil", (PyCFunction) enable_drop_gil, METH_NOARGS, ""},
    {0, 0, 0, 0}
};


#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "cpython",
    NULL,
    -1,
    module_methods,
    NULL,
    NULL,
    NULL,
    NULL
};
#endif


PyMODINIT_FUNC
MODINIT_NAME(void)
{
#if PY_MAJOR_VERSION >= 3
    PyObject *mod = PyModule_Create(&moduledef);
#else
    PyObject *mod = Py_InitModule3("cpython", module_methods, "");
#endif
    if(! mod) {
        MOD_RETURN(NULL);
    }

    static PyTypeObject *types[] = {
        &PyEnvironment_Type,
        &PyCursor_Type,
        &PyTransaction_Type,
        &PyIterator_Type,
        &PyDatabase_Type,
        NULL
    };
    int i;
    for(i = 0; types[i]; i++) {
        if(add_type(mod, types[i])) {
            MOD_RETURN(NULL);
        }
    }

    string_tbl = malloc(sizeof(PyObject *) * STRING_ID_COUNT);
    if(! string_tbl) {
        MOD_RETURN(NULL);
    }

    const char *cur = strings;
    for(i = 0; i < STRING_ID_COUNT; i++) {
        if(! ((string_tbl[i] = PyUnicode_InternFromString(cur)))) {
            MOD_RETURN(NULL);
        }
        cur += strlen(cur) + 1;
    }

    if(! ((py_zero = PyLong_FromSize_t(0)))) {
        MOD_RETURN(NULL);
    }
    if(! ((py_int_max = PyLong_FromSize_t(INT_MAX)))) {
        MOD_RETURN(NULL);
    }
    if(! ((py_size_max = PyLong_FromSize_t(SIZE_MAX)))) {
        MOD_RETURN(NULL);
    }

    Error = PyErr_NewException("lmdb.Error", NULL, NULL);
    if(! Error) {
        MOD_RETURN(NULL);
    }
    if(PyObject_SetAttrString(mod, "Error", Error)) {
        MOD_RETURN(NULL);
    }
    if(PyObject_SetAttrString(mod, "open", (PyObject *)&PyEnvironment_Type)) {
        MOD_RETURN(NULL);
    }
    MOD_RETURN(mod);
}
