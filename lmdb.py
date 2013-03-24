# Copyright 2013 The Python-lmdb authors, all rights reserved.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted only as authorized by the OpenLDAP
# Public License.
# 
# A copy of this license is available in the file LICENSE in the
# top-level directory of the distribution or, alternatively, at
# <http://www.OpenLDAP.org/license.html>.
# 
# OpenLDAP is a registered trademark of the OpenLDAP Foundation.
# 
# Individual files and/or contributed packages may be copyright by
# other parties and/or subject to additional restrictions.
# 
# This work also contains materials derived from public sources.
# 
# Additional information about OpenLDAP can be obtained at
# <http://www.openldap.org/>.

"""
cffi wrapper for OpenLDAP's "Lightning" MDB database.

Please see http://lmdb.readthedocs.org/
"""

from __future__ import absolute_import

import functools
import os
import shutil
import tempfile
import warnings
import weakref

import cffi

__all__ = ['Environment', 'Database', 'Cursor', 'Transaction', 'connect',
           'Error']

_ffi = cffi.FFI()
_ffi.cdef('''
    typedef int mode_t;
    typedef ... MDB_env;
    typedef struct MDB_txn MDB_txn;
    typedef struct MDB_cursor MDB_cursor;
    typedef unsigned int MDB_dbi;
    enum MDB_cursor_op {
        MDB_FIRST,
        MDB_FIRST_DUP,
        MDB_GET_BOTH,
        MDB_GET_BOTH_RANGE,
        MDB_GET_CURRENT,
        MDB_GET_MULTIPLE,
        MDB_LAST,
        MDB_LAST_DUP,
        MDB_NEXT,
        MDB_NEXT_DUP,
        MDB_NEXT_MULTIPLE,
        MDB_NEXT_NODUP,
        MDB_PREV,
        MDB_PREV_DUP,
        MDB_PREV_NODUP,
        MDB_SET,
        MDB_SET_KEY,
        MDB_SET_RANGE,
        ...
    };
    typedef enum MDB_cursor_op MDB_cursor_op;

    struct MDB_val {
        size_t mv_size;
        char *mv_data;
        ...;
    };
    typedef struct MDB_val MDB_val;

    struct MDB_stat {
        unsigned int ms_psize;
        unsigned int ms_depth;
        size_t ms_branch_pages;
        size_t ms_leaf_pages;
        size_t ms_overflow_pages;
        size_t ms_entries;
        ...;
    };
    typedef struct MDB_stat MDB_stat;

    struct MDB_envinfo {
        void *me_mapaddr;
        size_t me_mapsize;
        size_t me_last_pgno;
        size_t me_last_txnid;
        unsigned int me_maxreaders;
        unsigned int me_numreaders;
        ...;
    };
    typedef struct MDB_envinfo MDB_envinfo;

    typedef int (*MDB_cmp_func)(const MDB_val *a, const MDB_val *b);
    typedef void (*MDB_rel_func)(MDB_val *item, void *oldptr, void *newptr,
                   void *relctx);

    char *mdb_strerror(int err);
    int mdb_env_create(MDB_env **env);
    int mdb_env_open(MDB_env *env, const char *path, unsigned int flags,
                     mode_t mode);
    int mdb_env_copy(MDB_env *env, const char *path);
    int mdb_env_stat(MDB_env *env, MDB_stat *stat);
    int mdb_env_info(MDB_env *env, MDB_envinfo *stat);
    int mdb_env_sync(MDB_env *env, int force);
    void mdb_env_close(MDB_env *env);
    int mdb_env_set_flags(MDB_env *env, unsigned int flags, int onoff);
    int mdb_env_get_flags(MDB_env *env, unsigned int *flags);
    int mdb_env_get_path(MDB_env *env, const char **path);
    int mdb_env_set_mapsize(MDB_env *env, size_t size);
    int mdb_env_set_maxreaders(MDB_env *env, unsigned int readers);
    int mdb_env_get_maxreaders(MDB_env *env, unsigned int *readers);
    int mdb_env_set_maxdbs(MDB_env *env, MDB_dbi dbs);
    int mdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags,
                      MDB_txn **txn);
    int mdb_txn_commit(MDB_txn *txn);
    void mdb_txn_abort(MDB_txn *txn);
    int mdb_dbi_open(MDB_txn *txn, const char *name, unsigned int flags,
                     MDB_dbi *dbi);
    int mdb_stat(MDB_txn *txn, MDB_dbi dbi, MDB_stat *stat);
    void mdb_dbi_close(MDB_env *env, MDB_dbi dbi);
    int mdb_drop(MDB_txn *txn, MDB_dbi dbi, int del_);
    int mdb_get(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data);
    int mdb_put(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data,
                unsigned int flags);
    int mdb_del(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data);
    int mdb_cursor_open(MDB_txn *txn, MDB_dbi dbi, MDB_cursor **cursor);
    void mdb_cursor_close(MDB_cursor *cursor);
    int mdb_cursor_get(MDB_cursor *cursor, MDB_val *key, MDB_val *data, int op);
    int mdb_cursor_put(MDB_cursor *cursor, MDB_val *key, MDB_val *data,
                       unsigned int flags);
    int mdb_cursor_del(MDB_cursor *cursor, unsigned int flags);
    int mdb_cursor_count(MDB_cursor *cursor, size_t *countp);

    #define MDB_APPEND ...
    #define MDB_APPENDDUP ...
    #define MDB_CORRUPTED ...
    #define MDB_CREATE ...
    #define MDB_CURRENT ...
    #define MDB_CURSOR_FULL ...
    #define MDB_DBS_FULL ...
    #define MDB_DUPFIXED ...
    #define MDB_DUPSORT ...
    #define MDB_FIXEDMAP ...
    #define MDB_INTEGERDUP ...
    #define MDB_INTEGERKEY ...
    #define MDB_INVALID ...
    #define MDB_KEYEXIST ...
    #define MDB_MAPASYNC ...
    #define MDB_MAP_FULL ...
    #define MDB_MULTIPLE ...
    #define MDB_NODUPDATA ...
    #define MDB_NOMETASYNC ...
    #define MDB_NOOVERWRITE ...
    #define MDB_NOSUBDIR ...
    #define MDB_NOSYNC ...
    #define MDB_NOTFOUND ...
    #define MDB_PAGE_FULL ...
    #define MDB_PAGE_NOTFOUND ...
    #define MDB_PANIC ...
    #define MDB_RDONLY ...
    #define MDB_READERS_FULL ...
    #define MDB_RESERVE ...
    #define MDB_REVERSEDUP ...
    #define MDB_REVERSEKEY ...
    #define MDB_SUCCESS ...
    #define MDB_TLS_FULL ...
    #define MDB_TXN_FULL ...
    #define MDB_VERSION_DATE ...
    #define MDB_VERSION_MAJOR ...
    #define MDB_VERSION_MINOR ...
    #define MDB_VERSION_MISMATCH ...
    #define MDB_VERSION_PATCH ...
    #define MDB_WRITEMAP ...

    // Helpers below inline MDB_vals. Avoids key alloc/dup on CPython, where
    // cffi will use PyString_AS_STRING when passed as an argument.
    static int mdb_del_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              char *val_s, size_t vallen);
    static int mdb_put_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              char *val_s, size_t vallen,
                              unsigned int flags);
    static int mdb_get_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              MDB_val *val_out);
    static int mdb_cursor_get_helper(MDB_cursor *cursor,
                                     char *key_s, size_t keylen,
                                     MDB_val *key, MDB_val *data, int op);
''')

_lib = _ffi.verify('''
    #include <sys/stat.h>
    #include "lmdb.h"

    // Helpers below inline MDB_vals. Avoids key alloc/dup on CPython, where
    // cffi will use PyString_AS_STRING when passed as an argument.
    static int mdb_get_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              MDB_val *val_out)
    {
        MDB_val key = {keylen, key_s};
        return mdb_get(txn, dbi, &key, val_out);
    }

    static int mdb_put_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              char *val_s, size_t vallen,
                              unsigned int flags)
    {
        MDB_val key = {keylen, key_s};
        MDB_val val = {vallen, val_s};
        return mdb_put(txn, dbi, &key, &val, flags);
    }

    static int mdb_del_helper(MDB_txn *txn, MDB_dbi dbi,
                              char *key_s, size_t keylen,
                              char *val_s, size_t vallen)
    {
        MDB_val key = {keylen, key_s};
        MDB_val val = {vallen, val_s};
        MDB_val *valptr;
        if(vallen == 0) {
            valptr = NULL;
        } else {
            valptr = &val;
        }
        return mdb_del(txn, dbi, &key, valptr);
    }

    static int mdb_cursor_get_helper(MDB_cursor *cursor,
                                     char *key_s, size_t keylen,
                                     MDB_val *key, MDB_val *data, int op)
    {
        MDB_val tmpkey = {keylen, key_s};
        int rc = mdb_cursor_get(cursor, &tmpkey, data, op);
        if(rc == 0) {
            *key = tmpkey;
        }
        return rc;
    }
''',
    sources=['lib/mdb.c', 'lib/midl.c'])

globals().update((k, getattr(_lib, k))
                 for k in dir(_lib) if k[:4].upper().startswith('MDB_'))


class Error(Exception):
    """Raised when any MDB error occurs."""
    HINTS = {
        MDB_MAP_FULL:
            "Please use a larger Environment(map_size=) parameter",
        MDB_DBS_FULL:
            "Please use a larger Environment(max_dbs=) parameter",
        MDB_READERS_FULL:
            "Please use a larger Environment(max_readers=) parameter",
        MDB_TXN_FULL:
            "Please do less work within your transaction",
    }

    def __init__(self, what, code=0):
        self.what = what
        self.code = code
        self.reason = _ffi.string(mdb_strerror(code))
        msg = what
        if code:
            msg = '%s: %s' % (what, self.reason)
            if code in self.HINTS:
                msg += ' (%s)' % (self.HINTS[code],)
        Exception.__init__(self, msg)


class Invalid(object):
    """Dummy class substituted for an instance's type when a dependent object
    (transaction, database, environment) is rendered invalid (deleted, closed,
    dropped).

    This allows avoiding a set of expensive checks on every single operation
    (e.g. during cursor iteration).
    """
    def __getattribute__(self, k):
        raise Error('Invalid state: you closed/deleted/dropped a parent object')

_deps = {}

def _invalidate_id(parid):
    pardeps = _deps.pop(parid, None)
    while pardeps:
        chid, chref = pardeps.popitem()
        obj = chref()
        if obj:
            _invalidate_id(chid)
            obj.__dict__.clear()
            obj.__type__ = Invalid

def _invalidate(obj):
    _invalidate_id(id(obj))

def _depend(parent, child):
    parid = id(parent)
    pardeps = _deps.get(parid)
    if not pardeps:
        weakref.ref(parent, functools.partial(_invalidate_id, parid))
        pardeps = {}
        _deps[parid] = pardeps
    chid = id(child)
    if chid not in pardeps:
        chref = weakref.ref(child, lambda _: pardeps.pop(chid))
        pardeps[chid] = chref


def _throw(what, rc):
    if rc:
        raise Error(what, rc)

def _mvbuf(mv):
    return buffer(_ffi.buffer(mv.mv_data, mv.mv_size))

def _mvstr(mv):
    return _ffi.buffer(mv.mv_data, mv.mv_size)[:]

def _mvset(mv, s):
    buf = _ffi.new('char[]', s)
    mv.mv_data = buf
    mv.mv_size = len(s)
    return buf

def connect(path=None, **kwargs):
    """Shorthand for ``lmdb.Environment(path, **kwargs)``"""
    return Environment(path, **kwargs)



class Environment(object):
    """
    Structure for a database environment.

    A DB environment supports multiple databases, all residing in the same
    shared-memory map.
    """
    def __init__(self, path=None, map_size=10485760, subdir=True,
            readonly=False, metasync=True, sync=True, map_async=False,
            mode=0644, create=True, max_readers=126, max_dbs=0):
        """Create and open an environment.

        ``path``
            Location of directory (if ``subdir=True``) or file prefix to store
            the database. If unspecified, an Environment will be created in the
            system TEMP and deleted when closed.
        ``map_size``
            Maximum size database may grow to; used to size the memory mapping.
            If database grows larger than ``map_size``, exception will be
            raised and Environment must be recreated. On 64-bit there is no
            penalty for making this huge (say 1TB). Must be <2GB on 32-bit.
        ``subdir``
            If True, `path` refers to a subdirectory to store the data and lock
            files within, otherwise it refers to a filename prefix.
        ``readonly``
            If True, disallow any write operations. Note the lock file is still
            modified.
        ``metasync``
            If False, never explicitly flush metadata pages to disk. OS will
            flush at its disgression, or user can flush with
            Environment.sync().
        ``sync``
            If False, never explicitly fluh data pages to disk. OS will flush
            at its disgression, or user can flush with ``Environment.sync()``.
            This optimization means a system crash can corrupt the database or
            lose the last transactions if buffers are not yet flushed to disk.
        ``mode``
            File creation mode.
        ``create``
            If False, do not create the directory `path` if it is missing.
        ``max_readers``
            Slots to allocate in lock file for read threads; attempts to open
            the environment by more than this many clients simultaneously will
            fail. only meaningful for environments that aren't already open.
        ``max_dbs``
            Maximum number of databases available. If 0, assume environment
            will be used as a single database.
        """
        envpp = _ffi.new('MDB_env **')

        _throw("Creating environment",
               mdb_env_create(envpp))
        self._env = envpp[0]

        _throw("Setting map size",
               mdb_env_set_mapsize(self._env, map_size))
        _throw("Setting max readers",
               mdb_env_set_maxreaders(self._env, max_readers))
        _throw("Setting max DBs",
               mdb_env_set_maxdbs(self._env, max_dbs))

        if path is None:
            path = tempfile.mkdtemp(prefix='lmdb')
            self.temp = 1
            subdir = True
        if create and subdir and not os.path.exists(path):
            os.mkdir(path)

        flags = 0
        if not subdir:
            flags |= MDB_NOSUBDIR
        if readonly:
            flags |= MDB_RDONLY
        if not metasync:
            flags |= MDB_NOMETASYNC
        if not sync:
            flags |= MDB_NOSYNC
        if map_async:
            flags |= MDB_MAPASYNC

        _throw(path, mdb_env_open(self._env, path, flags, mode))
        with self.begin() as txn:
            self._default_db = Database(self, txn)

    def close(self):
        _invalidate(self)
        self.__del__()

    def __del__(self):
        if self._env:
            path = self.path
            mdb_env_close(self._env)
            if self.temp:
                shutil.rmtree(path)

    @property
    def path(self):
        """Directory path or file name prefix where this environment is
        stored."""
        path = _ffi.new('char **')
        mdb_env_get_path(self._env, path)
        return _ffi.string(path[0])

    @property
    def max_readers(self):
        """Maximum number of client threads that may read this environment
        simultaneously."""
        readers = _ffi.new('unsigned int *')
        mdb_env_get_maxreaders(self._env, readers)
        return readers[0]

    def copy(self, path):
        """Make a consistent copy of the environment in the given destination
        directory.
        """
        _throw("Copying environment",
               mdb_env_copy(self._env, path))

    def sync(self, force=False):
        """Flush the data buffers to disk.

        Data is always written to disk when ``Transaction.commit()`` is called,
        but the operating system may keep it buffered. MDB always flushes the
        OS buffers upon commit as well, unless the environment was opened with
        ``sync=False`` or ``metasync=False``.
     
        ``force``
            If non-zero, force a synchronous flush. Otherwise if the
            environment was opened with ``sync=False`` the flushes will be
            omitted, and with #MDB_MAPASYNC they will be asynchronous.
        """
        _throw("Flushing",
               mdb_env_sync(self._env, force))

    def stat(self):
        """stat()

        Return some nice environment statistics as a dict:

        +--------------------+---------------------------------------+
        | ``psize``          | Size of a database page in bytes.     |
        +--------------------+---------------------------------------+
        | ``depth``          | Height of the B-tree.                 |
        +--------------------+---------------------------------------+
        | ``branch_pages``   | Number of internal (non-leaf) pages.  |
        +--------------------+---------------------------------------+
        | ``leaf_pages``     | Number of leaf pages.                 |
        +--------------------+---------------------------------------+
        | ``overflow_pages`` | Number of overflow pages.             |
        +--------------------+---------------------------------------+
        | ``entries``        | Number of data items.                 |
        +--------------------+---------------------------------------+
        """
        st = _ffi.new('MDB_stat *')
        _throw("Getting environment statistics",
               mdb_env_stat(self._env, st))
        return {
            "psize": st.ms_psize,
            "depth": st.ms_depth,
            "branch_pages": st.ms_branch_pages,
            "leaf_pages": st.ms_leaf_pages,
            "overflow_pages": st.ms_overflow_pages,
            "entries": st.ms_entries
        }

    def info(self):
        """Return some nice environment information as a dict:

        +--------------------+---------------------------------------+
        | map_addr           | Address of database map in RAM.       |
        +--------------------+---------------------------------------+
        | map_size           | Size of database map in RAM.          |
        +--------------------+---------------------------------------+
        | last_pgno          | ID of last used page.                 |
        +--------------------+---------------------------------------+
        | last_txnid         | ID of last committed transaction.     |
        +--------------------+---------------------------------------+
        | max_readers        | Maximum number of threads.            |
        +--------------------+---------------------------------------+
        | num_readers        | Number of threads in use.             |
        +--------------------+---------------------------------------+
        """
        info = _ffi.new('MDB_envinfo *')
        rc = mdb_env_info(self._env, info)
        if rc:
            raise Error('Getting environment info', rc)
        return {
            "map_size": info.me_mapsize,
            "last_pgno": info.me_last_pgno,
            "last_txnid": info.me_last_txnid,
            "max_readers": info.me_maxreaders,
            "num_readers": info.me_numreaders
        }

    def open(self, **kwargs):
        """Shorthand for ``lmdb.Database(self, **kwargs)``"""
        if not kwargs.get('name'):
            return self._default_db
        with self.begin() as txn:
            return Database(self, txn, **kwargs)

    def begin(self, **kwargs):
        """Shortcut for ``lmdb.Transaction(self, **kwargs)``"""
        return Transaction(self, **kwargs)


class Database(object):
    """
    Handle for an individual database in the DB environment.
    """
    def __init__(self, env, txn, name=None, reverse_key=False,
                 dupsort=False, create=True, buffers=False):
        """Get a reference to or create a database within an environment.

            txn: Transaction.
            reverse_key: If True, keys are compared from right to left (e.g. DNS
                names)
            dupsort: Duplicate keys may be used in the database. (Or, from
                another perspective, keys may have multiple data items, stored
                in sorted order.) By default keys must be unique and may have
                only a single data item.
            create: If True, create the database if it doesn't exist, otherwise
                raise an exception.

        The Python module does not yet fully support dupsort.
        """
        _depend(env, self)
        self.env = env

        self._key = _ffi.new('MDB_val *')
        self._val = _ffi.new('MDB_val *')
        self._to_py = _mvbuf if buffers else _mvstr
        flags = 0
        if reverse_key:
            flags |= MDB_REVERSEKEY
        if dupsort:
            flags |= MDB_DUPSORT
        if create:
            flags |= MDB_CREATE
        dbipp = _ffi.new('MDB_dbi *')
        _throw("Opening database",
               mdb_dbi_open(txn._txn, name or _ffi.NULL, flags, dbipp))
        self._dbi = dbipp[0]

    def __del__(self):
        _invalidate(self)
        mdb_dbi_close(self.env._env, self._dbi)

    def drop(self, delete=True):
        """Delete all keys and optionally delete the database itself. Deleting
        the database causes it to become unavailable, and invalidates existing
        cursors.
        """
        mdb_drop(self.txn._txn, self._dbi, delete)
        _invalidate(self)


class Transaction(object):
    """
    A transaction handle.

    All database operations require a transaction handle. Transactions may be
    read-only or read-write.
    """
    def __init__(self, env, parent=None, readonly=False):
        """Start a new transaction.

            env: Environment the transaction should be on.
            parent: None, or a parent transaction (see lmdb.h).
            readonly: Read-only?
        """
        _depend(env, self)
        self._env = env._env
        self.readonly = readonly
        if readonly:
            what = "Beginning read-only transaction"
            flags = MDB_RDONLY
        else:
            what = "Beginning write transaction"
            flags = 0
        txnpp = _ffi.new('MDB_txn **')
        parent_txn = parent._txn if parent else _ffi.NULL
        _throw(what,
            mdb_txn_begin(self._env, parent_txn, flags, txnpp))
        self._txn = txnpp[0]

    def __del__(self):
        if self._txn:
            mdb_txn_abort(self._txn)
            self._txn = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.__del__()

    def commit(self):
        """Commit the pending transaction."""
        _throw("Committing transaction",
               mdb_txn_commit(self._txn))
        self._txn = None
        _invalidate(self)

    def abort(self):
        """Abort the pending transaction."""
        self.__del__()
        _invalidate(self)

    def get(self, key, default=None, db=None):
        """Fetch the first value matching `key`, otherwise return `default`. A
        cursor must be used to fetch all values for a key in a `dupsort=True`
        database.
        """
        rc = mdb_get_helper(self.txn._txn, self._dbi, key, len(key), self._val)
        if rc:
            if rc == MDB_NOTFOUND:
                return default
            raise Error(repr(key), rc)
        return self._to_py(self._val)

    def put(self, key, value, dupdata=False, overwrite=True, append=False):
        """Store key/value pairs in the database.

            key: String key to store.
            value: String value to store.
            dupdata: If True and database was opened with dupsort=True, add
                pair as a duplicate if the given key already exists. Otherwise
                overwrite any existing matching key.
            overwrite: If False, do not overwrite any existing matching key.
            append: If True, append the pair to the end of the database without
                comparing its order first. Appending a key that is not greater
                than the highest existing key will cause corruption.

        Returns True if the pair was written or False to indicate the key was
        already present and override=False.
        """
        flags = 0
        if not dupdata:
            flags |= MDB_NODUPDATA
        if not overwrite:
            flags |= MDB_NOOVERWRITE
        if append:
            flags |= MDB_APPEND

        rc = mdb_put_helper(self.txn._txn, self._dbi, key, len(key),
                            value, len(value), flags)
        if rc:
            if rc == MDB_KEYEXIST:
                return False
            raise Error("Setting key", rc)
        return True

    def delete(self, key, value=''):
        """Delete a key from the database.

            key: The key to delete.
            value: If the database was opened with dupsort=True and value is
                not the empty string, then delete elements matching only this
                (key, value) pair, otherwise all values for key are deleted.

        Returns True if at least one key was deleted.
        """
        rc = mdb_del_helper(self.txn._txn, self._dbi,
                            key, len(key), value, len(value))
        if rc:
            if rc == MDB_NOTFOUND:
                return False
            raise Error("Deleting key", rc)
        return True

    def cursor(self, **kwargs):
        """Shorthand for ``lmdb.Cursor(self, **kwargs)``"""
        return Cursor(self, **kwargs)


class Cursor(object):
    """
    Structure for navigating a database.

    Cursors by default are positioned on the first element in the database.
    """
    def __init__(self, db, txn, buffers=False):
        """Create a new cursor. If both `keys` and `values` are True, the
        Python iterator protocol applied to this object will yield ``(key,
        value)`` tuples, if only `value` is True then a sequence of values will
        be generated, otherwise a sequence of keys will be generated.
        """
        _depend(db, self)
        _depend(txn, self)
        self._dbi = db._dbi
        self._txn = txn._txn

        self._to_py = _mvbuf if buffers else _mvstr
        self._key = _ffi.new('MDB_val *')
        self._val = _ffi.new('MDB_val *')
        curpp = _ffi.new('MDB_cursor **')
        _throw("Creating cursor",
               mdb_cursor_open(self._txn, self._dbi, curpp))
        self._cur = curpp[0]

    def __del__(self):
        if self._cur:
            mdb_cursor_close(self._cur)

    def _get_key(self):
        """The current key."""
        return self._to_py(self._key)
    key = property(_get_key)

    def _get_value(self):
        """The current value."""
        return self._to_py(self._val)
    value = property(_get_value)

    def _get_item(self):
        """The current (key, value) pair."""
        return self._to_py(self._key), self._to_py(self._val)
    item = property(_get_item)

    def _iter(self, advance, keys, values):
        if not values:
            get = self._get_key
        elif not keys:
            get = self._get_value
        else:
            get = self._get_item

        go = True
        while go:
            yield get()
            go = advance()

    def forward(self, keys=True, values=True):
        """Return an iterator that repeatedly returns the current key then
        advances the cursor, until the end of the database is reached."""
        return self._iter(self.next, keys, values)
    __iter__ = forward

    def reverse(self, keys=True, values=True):
        """Return an iterator that repeatedly returns the current key then
        steps the cursor back, until the start of the database is reached."""
        return self._iter(self.prev, keys, values)

    def _cursor_get(self, op, k):
        rc = mdb_cursor_get_helper(self._cur, k, len(k),
                                   self._key, self._val, op)
        if rc:
            if rc == MDB_NOTFOUND:
                return False
            raise Error("Advancing cursor", rc)
        return True

    def first(self):
        """Reposition the cursor on the first element in the database."""
        self._cursor_get(MDB_FIRST, '')

    def last(self):
        """Reposition the cursor on the last element in the database."""
        self._cursor_get(MDB_LAST, '')

    def prev(self):
        """Move the cursor to the previous element, returning ``True`` on
        success or ``False`` if there is no previous element."""
        return self._cursor_get(MDB_PREV, '')

    def next(self):
        """Move the cursor to the next element, returning ``True`` on success
        or ``False`` if there is no next element."""
        return self._cursor_get(MDB_NEXT, '')

    def set_key(self, k):
        """Seek exactly to `k`, returning ``True`` on success or ``False`` if
        the exact key was not found."""
        return self._cursor_get(MDB_SET_KEY, k)

    def set_range(self, k):
        """Seek to the first key greater than or equal to the specified key.
        Returns ``True`` on success, or ``False`` to indicate key was past end
        of database."""
        return self._cursor_get(MDB_SET_RANGE, k)

    def delete(self):
        """Delete the current key, and position the cursor on the next element
        in the database, if any."""
        rc = mdb_cursor_del(self._cur, 0)
        if rc:
            raise Error("Deleting current key", rc)
        self._cursor_get(MDB_GET_CURRENT)

    @property
    def count(self):
        """Count of duplicates for the current key. Raises an exception if the
        cursor is invalid."""
        countp = _ffi.new('size_t *')
        rc = mdb_cursor_count(self._cur, countp)
        if rc:
            raise Error("Getting duplicate count", rc)
        return countp[0]
