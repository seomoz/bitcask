// -------------------------------------------------------------------
//
// bitcask: Eric Brewer-inspired key/value store
//
// Copyright (c) 2010 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

#include "erl_nif.h"
#include "erl_driver.h"
#include "erl_nif_compat.h"
#include "erl_nif_util.h"

#include "khash.h"
#include "murmurhash.h"

static ErlNifResourceType* bitcask_keydir_RESOURCE;

static ErlNifResourceType* bitcask_lock_RESOURCE;

typedef struct
{
    uint32_t file_id;
    uint32_t total_sz;
    uint64_t offset;
    uint32_t tstamp;
    uint16_t key_sz;
    char     key[0];
} bitcask_keydir_entry;

static khint_t keydir_entry_hash(bitcask_keydir_entry* entry);
static khint_t keydir_entry_equal(bitcask_keydir_entry* lhs,
                                  bitcask_keydir_entry* rhs);
KHASH_INIT(entries, bitcask_keydir_entry*, char, 0, keydir_entry_hash, keydir_entry_equal);

typedef struct
{
    uint32_t file_id;
    uint32_t live_keys;
    uint32_t total_keys;
    uint64_t live_bytes;
    uint64_t total_bytes;
} bitcask_fstats_entry;

KHASH_MAP_INIT_INT(fstats, bitcask_fstats_entry*);

typedef struct
{
    khash_t(entries)* entries;
    khash_t(fstats)*  fstats;
    size_t        key_count;
    size_t        key_bytes;
    unsigned int  refcount;
    unsigned int  keyfolders;
    ErlNifMutex*  mutex;
    char          is_ready;
    char          name[0];
} bitcask_keydir;

typedef struct
{
    bitcask_keydir* keydir;
    int             iterating;
    khiter_t        iterator;
} bitcask_keydir_handle;

typedef struct
{
    int   fd;
    int   is_write_lock;
    char  filename[0];
} bitcask_lock_handle;

KHASH_INIT(global_keydirs, char*, bitcask_keydir*, 1, kh_str_hash_func, kh_str_hash_equal);

typedef struct
{
    khash_t(global_keydirs)* global_keydirs;
    ErlNifMutex*             global_keydirs_lock;
} bitcask_priv_data;

#define kh_put2(name, h, k, v) {                        \
        int itr_status;                                 \
        khiter_t itr = kh_put(name, h, k, &itr_status); \
        kh_val(h, itr) = v; }                           \

#define kh_put_set(name, h, k) {                        \
        int itr_status;                                 \
        kh_put(name, h, k, &itr_status); }


// Handle lock helper functions
#define LOCK(keydir)      { if (keydir->mutex) enif_mutex_lock(keydir->mutex); }
#define UNLOCK(keydir)    { if (keydir->mutex) enif_mutex_unlock(keydir->mutex); }

// Atoms (initialized in on_load)
static ERL_NIF_TERM ATOM_ALLOCATION_ERROR;
static ERL_NIF_TERM ATOM_ALREADY_EXISTS;
static ERL_NIF_TERM ATOM_BITCASK_ENTRY;
static ERL_NIF_TERM ATOM_ERROR;
static ERL_NIF_TERM ATOM_FALSE;
static ERL_NIF_TERM ATOM_FSTAT_ERROR;
static ERL_NIF_TERM ATOM_FTRUNCATE_ERROR;
static ERL_NIF_TERM ATOM_GETFL_ERROR;
static ERL_NIF_TERM ATOM_ILT_CREATE_ERROR; /* Iteration lock thread creation error */
static ERL_NIF_TERM ATOM_ITERATION_IN_PROCESS;
static ERL_NIF_TERM ATOM_ITERATION_NOT_PERMITTED;
static ERL_NIF_TERM ATOM_ITERATION_NOT_STARTED;
static ERL_NIF_TERM ATOM_LOCK_NOT_WRITABLE;
static ERL_NIF_TERM ATOM_NOT_FOUND;
static ERL_NIF_TERM ATOM_NOT_READY;
static ERL_NIF_TERM ATOM_OK;
static ERL_NIF_TERM ATOM_PREAD_ERROR;
static ERL_NIF_TERM ATOM_PWRITE_ERROR;
static ERL_NIF_TERM ATOM_READY;
static ERL_NIF_TERM ATOM_SETFL_ERROR;
static ERL_NIF_TERM ATOM_TRUE;

// Prototypes
ERL_NIF_TERM bitcask_nifs_keydir_new0(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_new1(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_mark_ready(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_get_int(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_put_int(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_remove(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_copy(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_itr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_itr_next(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_itr_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_info(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_keydir_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

ERL_NIF_TERM bitcask_nifs_create_file(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_create_tmp_file(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

ERL_NIF_TERM bitcask_nifs_set_osync(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

ERL_NIF_TERM bitcask_nifs_lock_acquire(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_lock_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_lock_readdata(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM bitcask_nifs_lock_writedata(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

ERL_NIF_TERM errno_atom(ErlNifEnv* env, int error);
ERL_NIF_TERM errno_error_tuple(ErlNifEnv* env, ERL_NIF_TERM key, int error);

static void lock_release(bitcask_lock_handle* handle);

static void bitcask_nifs_keydir_resource_cleanup(ErlNifEnv* env, void* arg);

static ErlNifFunc nif_funcs[] =
{
    {"keydir_new", 0, bitcask_nifs_keydir_new0},
    {"keydir_new", 1, bitcask_nifs_keydir_new1},
    {"keydir_mark_ready", 1, bitcask_nifs_keydir_mark_ready},
    {"keydir_put_int", 6, bitcask_nifs_keydir_put_int},
    {"keydir_get_int", 2, bitcask_nifs_keydir_get_int},
    {"keydir_remove", 2, bitcask_nifs_keydir_remove},
    {"keydir_remove_int", 5, bitcask_nifs_keydir_remove},
    {"keydir_copy", 1, bitcask_nifs_keydir_copy},
    {"keydir_itr", 1, bitcask_nifs_keydir_itr},
    {"keydir_itr_next_int", 1, bitcask_nifs_keydir_itr_next},
    {"keydir_itr_release", 1, bitcask_nifs_keydir_itr_release},
    {"keydir_info", 1, bitcask_nifs_keydir_info},
    {"keydir_release", 1, bitcask_nifs_keydir_release},

    {"create_file", 1, bitcask_nifs_create_file},

    {"set_osync", 1, bitcask_nifs_set_osync},

    {"lock_acquire",   2, bitcask_nifs_lock_acquire},
    {"lock_release",   1, bitcask_nifs_lock_release},
    {"lock_readdata",  1, bitcask_nifs_lock_readdata},
    {"lock_writedata", 2, bitcask_nifs_lock_writedata}
};

ERL_NIF_TERM bitcask_nifs_keydir_new0(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    // First, setup a resource for our handle
    bitcask_keydir_handle* handle = enif_alloc_resource_compat(env,
                                                        bitcask_keydir_RESOURCE,
                                                        sizeof(bitcask_keydir_handle));
    memset(handle, '\0', sizeof(bitcask_keydir_handle));

    // Now allocate the actual keydir instance. Because it's unnamed/shared, we'll
    // leave the name and lock portions null'd out
    bitcask_keydir* keydir = enif_alloc_compat(env, sizeof(bitcask_keydir));
    memset(keydir, '\0', sizeof(bitcask_keydir));
    keydir->entries  = kh_init(entries);
    keydir->fstats   = kh_init(fstats);

    // Assign the keydir to our handle and hand it back
    handle->keydir = keydir;
    ERL_NIF_TERM result = enif_make_resource(env, handle);
    enif_release_resource_compat(env, handle);
    return enif_make_tuple2(env, ATOM_OK, result);
}

ERL_NIF_TERM bitcask_nifs_keydir_new1(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    char name[4096];
    size_t name_sz;
    if (enif_get_string(env, argv[0], name, sizeof(name), ERL_NIF_LATIN1))
    {
        name_sz = strlen(name);

        // Get our private stash and check the global hash table for this entry
        bitcask_priv_data* priv = (bitcask_priv_data*)enif_priv_data(env);
        enif_mutex_lock(priv->global_keydirs_lock);

        bitcask_keydir* keydir;
        khiter_t itr = kh_get(global_keydirs, priv->global_keydirs, name);
        if (itr != kh_end(priv->global_keydirs))
        {
            keydir = kh_val(priv->global_keydirs, itr);
            // Existing keydir is available. Check the is_ready flag to determine if
            // the original creator is ready for other processes to use it.
            if (!keydir->is_ready)
            {
                // Notify the caller that while the requested keydir exists, it's not
                // ready for public usage.
                enif_mutex_unlock(priv->global_keydirs_lock);
                return enif_make_tuple2(env, ATOM_ERROR, ATOM_NOT_READY);
            }
            else
            {
                keydir->refcount++;
            }
        }
        else
        {
            // No such keydir, create a new one and add to the globals list. Make sure
            // to allocate enough room for the name.
            keydir = enif_alloc_compat(env, sizeof(bitcask_keydir) + name_sz + 1);
            memset(keydir, '\0', sizeof(bitcask_keydir) + name_sz + 1);
            strncpy(keydir->name, name, name_sz + 1);

            // Initialize hash tables
            keydir->entries  = kh_init(entries);
            keydir->fstats   = kh_init(fstats);

            // Be sure to initialize the mutex and set our refcount
            keydir->mutex = enif_mutex_create(name);
            keydir->refcount = 1;

            // Finally, register this new keydir in the globals
            kh_put2(global_keydirs, priv->global_keydirs, keydir->name, keydir);
        }

        enif_mutex_unlock(priv->global_keydirs_lock);

        // Setup a resource for the handle
        bitcask_keydir_handle* handle = enif_alloc_resource_compat(env,
                                                            bitcask_keydir_RESOURCE,
                                                            sizeof(bitcask_keydir_handle));
        memset(handle, '\0', sizeof(bitcask_keydir_handle));
        handle->keydir = keydir;
        ERL_NIF_TERM result = enif_make_resource(env, handle);
        enif_release_resource_compat(env, handle);

        // Return to the caller a tuple with the reference and an atom
        // indicating if the keydir is ready or not.
        ERL_NIF_TERM is_ready_atom = keydir->is_ready ? ATOM_READY : ATOM_NOT_READY;
        return enif_make_tuple2(env, is_ready_atom, result);
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_mark_ready(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);
        keydir->is_ready = 1;
        UNLOCK(keydir);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

static void update_fstats(ErlNifEnv* env, bitcask_keydir* keydir,
                          uint32_t file_id,
                          int32_t live_increment, int32_t total_increment,
                          int32_t live_bytes_increment, int32_t total_bytes_increment)
{
    bitcask_fstats_entry* entry = 0;
    khiter_t itr = kh_get(fstats, keydir->fstats, file_id);
    if (itr == kh_end(keydir->fstats))
    {
        // Need to initialize new entry and add to the table
        entry = enif_alloc_compat(env, sizeof(bitcask_fstats_entry));
        memset(entry, '\0', sizeof(bitcask_fstats_entry));
        entry->file_id = file_id;

        kh_put2(fstats, keydir->fstats, file_id, entry);
    }
    else
    {
        entry = kh_val(keydir->fstats, itr);
    }

    entry->live_keys   += live_increment;
    entry->total_keys  += total_increment;
    entry->live_bytes  += live_bytes_increment;
    entry->total_bytes += total_bytes_increment;
}

static khint_t keydir_entry_hash(bitcask_keydir_entry* entry)
{
    return MURMUR_HASH(entry->key, entry->key_sz, 42);
}

static khint_t keydir_entry_equal(bitcask_keydir_entry* lhs,
                                  bitcask_keydir_entry* rhs)
{
    if (lhs->key_sz != rhs->key_sz)
    {
        return 0;
    }
    else
    {
        return (memcmp(lhs->key, rhs->key, lhs->key_sz) == 0);
    }
}

static khiter_t find_keydir_entry(ErlNifEnv* env, bitcask_keydir* keydir, ErlNifBinary* key)
{
    if (key->size < (4096 - sizeof(bitcask_keydir_entry)))
    {
        char buf[4096];
        bitcask_keydir_entry* e = (bitcask_keydir_entry*)buf;
        e->key_sz = key->size;
        memcpy(e->key, key->data, key->size);
        return kh_get(entries, keydir->entries, e);
    }
    else
    {
        bitcask_keydir_entry* e = enif_alloc_compat(env, sizeof(bitcask_keydir_entry) +
                                                    key->size);
        e->key_sz = key->size;
        memcpy(e->key, key->data, key->size);
        khiter_t itr = kh_get(entries, keydir->entries, e);
        enif_free_compat(env, e);
        return itr;
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_put_int(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;
    bitcask_keydir_entry entry;
    ErlNifBinary key;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle) &&
        enif_inspect_binary(env, argv[1], &key) &&
        enif_get_uint(env, argv[2], (unsigned int*)&(entry.file_id)) &&
        enif_get_uint(env, argv[3], &(entry.total_sz)) &&
        enif_get_uint64_bin(env, argv[4], &(entry.offset)) &&
        enif_get_uint(env, argv[5], &(entry.tstamp)))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);

        if (keydir->keyfolders > 0)
        {
            UNLOCK(keydir);
            return ATOM_ITERATION_IN_PROCESS;
        }

        // Now that we've marshalled everything, see if the tstamp for this key is >=
        // to what's already in the hash. Otherwise, we don't bother with the update.
        khiter_t itr = find_keydir_entry(env, keydir, &key);
        if (itr == kh_end(keydir->entries))
        {
            // No entry exists at all yet; add one
            bitcask_keydir_entry* new_entry = enif_alloc_compat(env,
                                                                sizeof(bitcask_keydir_entry) +
                                                                key.size);
            new_entry->file_id = entry.file_id;
            new_entry->total_sz = entry.total_sz;
            new_entry->offset = entry.offset;
            new_entry->tstamp = entry.tstamp;
            new_entry->key_sz = key.size;
            memcpy(new_entry->key, key.data, key.size);
            kh_put_set(entries, keydir->entries, new_entry);

            // Update the stats
            keydir->key_count++;
            keydir->key_bytes += key.size;

            // First entry for this key -- increment both live and total counters
            update_fstats(env, keydir, entry.file_id, 1, 1,
                          entry.total_sz, entry.total_sz);

            UNLOCK(keydir);
            return ATOM_OK;
        }

        bitcask_keydir_entry* old_entry = kh_key(keydir->entries, itr);
        if ((old_entry->tstamp < entry.tstamp) ||

            ((old_entry->tstamp == entry.tstamp) &&
             (old_entry->file_id < entry.file_id)) ||

            ((old_entry->tstamp == entry.tstamp) &&
             ((old_entry->file_id == entry.file_id) &&
              (old_entry->offset < entry.offset))))
        {
            // Entry already exists. Decrement live counter on the fstats entry
            // for the old file ID and update both counters for new file. Note
            // that this only needs to be done if the file_ids are not the
            // same.
            if (old_entry->file_id != entry.file_id)
            {
                update_fstats(env, keydir, old_entry->file_id, -1, 0,
                              -1 * old_entry->total_sz, 0);
                update_fstats(env, keydir, entry.file_id, 1, 1,
                              entry.total_sz, entry.total_sz);
            }
            else // file_id is same, change live/total in one entry
            {
                update_fstats(env, keydir, entry.file_id, 0, 1,
                        entry.total_sz - old_entry->total_sz, entry.total_sz);

            }

            // Update the entry info. Note that if you do multiple updates in a
            // second, the last one in wins!
            // TODO: Safe?
            old_entry->file_id = entry.file_id;
            old_entry->total_sz = entry.total_sz;
            old_entry->offset = entry.offset;
            old_entry->tstamp = entry.tstamp;

            UNLOCK(keydir);
            return ATOM_OK;
        }
        else
        {
            // If the keydir is in the process of being loaded, it's safe to update
            // fstats on a failed put. Once the keydir is live, any attempts to put
            // in old data would just be ignored to avoid double-counting problems.
            if (!keydir->is_ready)
            {
                // Increment the total # of keys and total size for the entry that
                // was NOT stored in the keydir.
                update_fstats(env, keydir, entry.file_id, 0, 1,
                              0, entry.total_sz);
            }

            UNLOCK(keydir);
            return ATOM_ALREADY_EXISTS;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_get_int(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;
    ErlNifBinary key;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle) &&
        enif_inspect_binary(env, argv[1], &key))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);

        khiter_t itr = find_keydir_entry(env, keydir, &key);
        if (itr != kh_end(keydir->entries))
        {
            bitcask_keydir_entry* entry = kh_key(keydir->entries, itr);
            ERL_NIF_TERM result = enif_make_tuple6(env,
                                                   ATOM_BITCASK_ENTRY,
                                                   argv[1], /* Key */
                                                   enif_make_uint(env, entry->file_id),
                                                   enif_make_uint(env, entry->total_sz),
                                                   enif_make_uint64_bin(env, entry->offset),
                                                   enif_make_uint(env, entry->tstamp));
            UNLOCK(keydir);
            return result;
        }
        else
        {
            UNLOCK(keydir);
            return ATOM_NOT_FOUND;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}


ERL_NIF_TERM bitcask_nifs_keydir_remove(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;
    ErlNifBinary key;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle) &&
        enif_inspect_binary(env, argv[1], &key))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);

        khiter_t itr = find_keydir_entry(env, keydir, &key);
        if (itr != kh_end(keydir->entries))
        {
            bitcask_keydir_entry* entry = kh_key(keydir->entries, itr);

            // If this call has 5 arguments, this is a conditional removal. We
            // only want to actually remove the entry if the tstamp, fileid and
            // offset matches the one provided. A sort of poor-man's CAS.
            if (argc == 5)
            {
                uint32_t tstamp;
                uint32_t file_id;
                uint64_t offset;
                if (enif_get_uint(env, argv[2], (unsigned int*)&tstamp) &&
                    enif_get_uint(env, argv[3], (unsigned int*)&file_id) &&
                    enif_get_uint64_bin(env, argv[4], (uint64_t*)&offset))
                {
                    if (entry->tstamp != tstamp || entry->file_id != file_id ||
                        entry->offset != offset)
                    {
                        // Either tstamp or file_id didn't match precisely. Ignore
                        // this attempt to delete the record.
                        UNLOCK(keydir);
                        return ATOM_OK;
                    }
                }
                else
                {
                    UNLOCK(keydir);
                    return enif_make_badarg(env);
                }
            }

            // Update fstats for the current file id -- one less live
            // key is the assumption here.
            update_fstats(env, keydir, entry->file_id, -1, 0,
                          -1 * entry->total_sz, 0);

            // Update the stats
            keydir->key_count--;
            keydir->key_bytes -= entry->key_sz;

            kh_del(entries, keydir->entries, itr);

            enif_free_compat(env, entry);
        }

        UNLOCK(keydir);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_copy(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);

        bitcask_keydir_handle* new_handle = enif_alloc_resource_compat(env,
                                                                bitcask_keydir_RESOURCE,
                                                                sizeof(bitcask_keydir_handle));
        memset(handle, '\0', sizeof(bitcask_keydir_handle));

        // Now allocate the actual keydir instance. Because it's unnamed/shared, we'll
        // leave the name and lock portions null'd out
        bitcask_keydir* new_keydir = enif_alloc_compat(env, sizeof(bitcask_keydir));
        new_handle->keydir = new_keydir;
        memset(new_keydir, '\0', sizeof(bitcask_keydir));
        new_keydir->entries  = kh_init(entries);
        new_keydir->fstats   = kh_init(fstats);

        // Deep copy each item from the existing handle
        khiter_t itr;
        for (itr = kh_begin(keydir->entries); itr != kh_end(keydir->entries); ++itr)
        {
            // Allocate our entry to be inserted into the new table and copy the record
            // over.
            if (kh_exist(keydir->entries, itr))
            {
                bitcask_keydir_entry* curr = kh_key(keydir->entries, itr);
                size_t new_sz = sizeof(bitcask_keydir_entry) + curr->key_sz;
                bitcask_keydir_entry* new = enif_alloc_compat(env, new_sz);
                memcpy(new, curr, new_sz);
                kh_put_set(entries, new_keydir->entries, new);
            }
        }

        // Deep copy fstats info
        for (itr = kh_begin(keydir->fstats); itr != kh_end(keydir->fstats); ++itr)
        {
            if (kh_exist(keydir->fstats, itr))
            {
                bitcask_fstats_entry* curr_f = kh_val(keydir->fstats, itr);
                bitcask_fstats_entry* new_f = enif_alloc_compat(env,
                                                                sizeof(bitcask_fstats_entry));
                memcpy(new_f, curr_f, sizeof(bitcask_fstats_entry));
                kh_put2(fstats, new_keydir->fstats, new_f->file_id, new_f);
            }
        }

        UNLOCK(keydir);

        ERL_NIF_TERM result = enif_make_resource(env, new_handle);
        enif_release_resource_compat(env, new_handle);
        return enif_make_tuple2(env, ATOM_OK, result);
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_itr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        LOCK(handle->keydir);

        // If a iterator thread is already active for this keydir, bail
        if (handle->iterating)
        {
            UNLOCK(handle->keydir);
            return enif_make_tuple2(env, ATOM_ERROR, ATOM_ITERATION_IN_PROCESS);
        }
        handle->iterating = 1;
        handle->keydir->keyfolders++;
        handle->iterator = kh_begin(handle->keydir->entries);

        UNLOCK(handle->keydir);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_itr_next(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        bitcask_keydir* keydir = handle->keydir;

        if (handle->iterating != 1)
        {
            // Iteration not started!
            return enif_make_tuple2(env, ATOM_ERROR, ATOM_ITERATION_NOT_STARTED);
        }

        while (handle->iterator != kh_end(keydir->entries))
        {
            if (kh_exist(keydir->entries, handle->iterator))
            {
                bitcask_keydir_entry* entry = kh_key(keydir->entries, handle->iterator);
                ErlNifBinary key;

                // Alloc the binary and make sure it succeeded
                if (!enif_alloc_binary_compat(env, entry->key_sz, &key))
                {
                    return ATOM_ALLOCATION_ERROR;
                }

                // Copy the data from our key to the new allocated binary
                // TODO: If we maintained a ErlNifBinary in the original entry, could we
                // get away with not doing a copy here?
                memcpy(key.data, entry->key, entry->key_sz);
                ERL_NIF_TERM curr = enif_make_tuple6(env,
                                                     ATOM_BITCASK_ENTRY,
                                                     enif_make_binary(env, &key),
                                                     enif_make_uint(env, entry->file_id),
                                                     enif_make_uint(env, entry->total_sz),
                                                     enif_make_uint64_bin(env, entry->offset),
                                                     enif_make_uint(env, entry->tstamp));

                // Update the iterator to the next entry
                (handle->iterator)++;
                return curr;
            }
            else
            {
                // No item in this slot; increment the iterator and keep looping
                (handle->iterator)++;
            }
        }

        // The iterator is at the end of the table
        return ATOM_NOT_FOUND;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_itr_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        LOCK(handle->keydir);
        if (handle->iterating != 1)
        {
            // Iteration not started!
            UNLOCK(handle->keydir);
            return enif_make_tuple2(env, ATOM_ERROR, ATOM_ITERATION_NOT_STARTED);
        }

        handle->keydir->keyfolders--;
        UNLOCK(handle->keydir);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}


ERL_NIF_TERM bitcask_nifs_keydir_info(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        bitcask_keydir* keydir = handle->keydir;
        LOCK(keydir);

        // Dump fstats info into a list of [{file_id, live_keys, total_keys,
        //                                   live_bytes, total_bytes}]
        ERL_NIF_TERM fstats_list = enif_make_list(env, 0);
        khiter_t itr;
        bitcask_fstats_entry* curr_f;
        for (itr = kh_begin(keydir->fstats); itr != kh_end(keydir->fstats); ++itr)
        {
            if (kh_exist(keydir->fstats, itr))
            {
                curr_f = kh_val(keydir->fstats, itr);
                ERL_NIF_TERM fstat = enif_make_tuple5(env,
                                                      enif_make_uint(env, curr_f->file_id),
                                                      enif_make_uint(env, curr_f->live_keys),
                                                      enif_make_uint(env, curr_f->total_keys),
                                                      enif_make_ulong(env, curr_f->live_bytes),
                                                      enif_make_ulong(env, curr_f->total_bytes));
                fstats_list = enif_make_list_cell(env, fstat, fstats_list);
            }
        }

        ERL_NIF_TERM result = enif_make_tuple3(env,
                                               enif_make_ulong(env, keydir->key_count),
                                               enif_make_ulong(env, keydir->key_bytes),
                                               fstats_list);
        UNLOCK(keydir);
        return result;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_keydir_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_keydir_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_keydir_RESOURCE, (void**)&handle))
    {
        bitcask_nifs_keydir_resource_cleanup(env, handle);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_create_file(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    char filename[4096];
    if (enif_get_string(env, argv[0], filename, sizeof(filename), ERL_NIF_LATIN1) > 0)
    {
        // Try to open the provided filename exclusively.
        int fd = open(filename, O_CREAT | O_EXCL, S_IREAD | S_IWRITE);
        if (fd > -1)
        {
            close(fd);
            return ATOM_TRUE;
        }
        else
        {
            return ATOM_FALSE;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}


ERL_NIF_TERM bitcask_nifs_set_osync(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int fd;
    if (enif_get_int(env, argv[0], &fd))
    {
        // Get the current flags for the file handle in question
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1)
        {
            if (fcntl(fd, F_SETFL, flags | O_SYNC) != -1)
            {
                return ATOM_OK;
            }
            else
            {
                ERL_NIF_TERM error = enif_make_tuple2(env, ATOM_SETFL_ERROR,
                                                      enif_make_atom(env, erl_errno_id(errno)));
                return enif_make_tuple2(env, ATOM_ERROR, error);
            }
        }
        else
        {
            ERL_NIF_TERM error = enif_make_tuple2(env, ATOM_GETFL_ERROR,
                                                  enif_make_atom(env, erl_errno_id(errno)));
            return enif_make_tuple2(env, ATOM_ERROR, error);
        }

    }
    else
    {
        return enif_make_badarg(env);
    }
}


ERL_NIF_TERM bitcask_nifs_lock_acquire(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    char filename[4096];
    int is_write_lock = 0;
    if (enif_get_string(env, argv[0], filename, sizeof(filename), ERL_NIF_LATIN1) > 0 &&
        enif_get_int(env, argv[1], &is_write_lock))
    {
        // Setup the flags for the lock file
        int flags = O_RDONLY;
        if (is_write_lock)
        {
            // Use O_SYNC (in addition to other flags) to ensure that when we write
            // data to the lock file it is immediately (or nearly) available to any
            // other reading processes
            flags = O_CREAT | O_EXCL | O_RDWR | O_SYNC;
        }

        // Try to open the lock file -- allocate a resource if all goes well.
        int fd = open(filename, flags, 0600);
        if (fd > -1)
        {
            // Successfully opened the file -- setup a resource to track the FD.
            unsigned int filename_sz = strlen(filename) + 1;
            bitcask_lock_handle* handle = enif_alloc_resource_compat(env, bitcask_lock_RESOURCE,
                                                              sizeof(bitcask_lock_handle) +
                                                              filename_sz);
            handle->fd = fd;
            handle->is_write_lock = is_write_lock;
            strncpy(handle->filename, filename, filename_sz);
            ERL_NIF_TERM result = enif_make_resource(env, handle);
            enif_release_resource_compat(env, handle);

            return enif_make_tuple2(env, ATOM_OK, result);
        }
        else
        {
            return enif_make_tuple2(env, ATOM_ERROR, errno_atom(env, errno));
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_lock_release(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_lock_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_lock_RESOURCE, (void**)&handle))
    {
        lock_release(handle);
        return ATOM_OK;
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_lock_readdata(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_lock_handle* handle;

    if (enif_get_resource(env, argv[0], bitcask_lock_RESOURCE, (void**)&handle))
    {
        // Stat the filehandle so we can read the entire contents into memory
        struct stat sinfo;
        if (fstat(handle->fd, &sinfo) != 0)
        {
            return errno_error_tuple(env, ATOM_FSTAT_ERROR, errno);
        }

        // Allocate a binary to hold the contents of the file
        ErlNifBinary data;
        if (!enif_alloc_binary_compat(env, sinfo.st_size, &data))
        {
            return enif_make_tuple2(env, ATOM_ERROR, ATOM_ALLOCATION_ERROR);
        }

        // Read the whole file into our binary
        if (pread(handle->fd, data.data, data.size, 0) == -1)
        {
            return errno_error_tuple(env, ATOM_PREAD_ERROR, errno);
        }

        return enif_make_tuple2(env, ATOM_OK, enif_make_binary(env, &data));
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM bitcask_nifs_lock_writedata(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    bitcask_lock_handle* handle;
    ErlNifBinary data;

    if (enif_get_resource(env, argv[0], bitcask_lock_RESOURCE, (void**)&handle) &&
        enif_inspect_binary(env, argv[1], &data))
    {
        if (handle->is_write_lock)
        {
            // Truncate the file first, to ensure that the lock file only contains what
            // we're about to write
            if (ftruncate(handle->fd, 0) == -1)
            {
                return errno_error_tuple(env, ATOM_FTRUNCATE_ERROR, errno);
            }

            // Write the new blob of data to the lock file. Note that we use O_SYNC to
            // ensure that the data is available ASAP to reading processes.
            if (pwrite(handle->fd, data.data, data.size, 0) == -1)
            {
                return errno_error_tuple(env, ATOM_PWRITE_ERROR, errno);
            }

            return ATOM_OK;
        }
        else
        {
            // Tried to write data to a read lock
            return enif_make_tuple2(env, ATOM_ERROR, ATOM_LOCK_NOT_WRITABLE);
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}

ERL_NIF_TERM errno_atom(ErlNifEnv* env, int error)
{
    return enif_make_atom(env, erl_errno_id(error));
}

ERL_NIF_TERM errno_error_tuple(ErlNifEnv* env, ERL_NIF_TERM key, int error)
{
    // Construct a tuple of form: {error, {Key, ErrnoAtom}}
    return enif_make_tuple2(env, ATOM_ERROR,
                            enif_make_tuple2(env, key, errno_atom(env, error)));
}

static void lock_release(bitcask_lock_handle* handle)
{
    if (handle->fd > 0)
    {
        // If this is a write lock, we need to delete the file as part of cleanup. But be
        // sure to do this BEFORE letting go of the file handle so as to ensure consistency
        // with other readers.
        if (handle->is_write_lock)
        {
            // TODO: Come up with some way to complain/error log if this unlink failed for some
            // reason!!
            unlink(handle->filename);
        }

        close(handle->fd);
        handle->fd = -1;
    }
}

static void free_keydir(ErlNifEnv* env, bitcask_keydir* keydir)
{
    // Delete all the entries in the hash table, which also has the effect of
    // freeing up all resources associated with the table.
    khiter_t itr;
    bitcask_keydir_entry* current_entry;
    for (itr = kh_begin(keydir->entries); itr != kh_end(keydir->entries); ++itr)
    {
        if (kh_exist(keydir->entries, itr))
        {
            current_entry = kh_key(keydir->entries, itr);
            enif_free_compat(env, current_entry);
        }
    }

    kh_destroy(entries, keydir->entries);

    bitcask_fstats_entry* curr_f;

    for (itr = kh_begin(keydir->fstats); itr != kh_end(keydir->fstats); ++itr)
    {
        if (kh_exist(keydir->fstats, itr))
        {
            curr_f = kh_val(keydir->fstats, itr);
            enif_free_compat(env, curr_f);
        }
    }

    kh_destroy(fstats, keydir->fstats);
}


static void bitcask_nifs_keydir_resource_cleanup(ErlNifEnv* env, void* arg)
{
    bitcask_keydir_handle* handle = (bitcask_keydir_handle*)arg;
    bitcask_keydir* keydir = handle->keydir;

    // First, check that there is even a keydir available. If keydir_release
    // was invoked manually, we might have already cleaned up the keydir
    // and this round of cleanup can noop. Otherwise, clear out the handle's
    // reference to the keydir so that repeat calls function as expected
    if (!handle->keydir)
    {
        return;
    }
    else
    {
        handle->keydir = 0;
    }

    // If the keydir has a lock, we need to decrement the refcount and
    // potentially release it
    if (keydir->mutex)
    {
        bitcask_priv_data* priv = (bitcask_priv_data*)enif_priv_data(env);
        enif_mutex_lock(priv->global_keydirs_lock);
        keydir->refcount--;
        if (keydir->refcount == 0)
        {
            // This is the last reference to the named keydir. As such,
            // remove it from the hashtable so no one else tries to use it
            khiter_t itr = kh_get(global_keydirs, priv->global_keydirs, keydir->name);
            kh_del(global_keydirs, priv->global_keydirs, itr);
        }
        else
        {
            // At least one other reference; just throw away our keydir pointer
            // so the check below doesn't release the memory.
            keydir = 0;
        }

        // Unlock ASAP. Wanted to avoid holding this mutex while we clean up the
        // keydir, since it may take a while to walk a large keydir and free each
        // entry.
        enif_mutex_unlock(priv->global_keydirs_lock);
    }

    // If keydir is still defined, it's either privately owner or has a
    // refcount of 0. Either way, we want to release it.
    if (keydir)
    {
        if (keydir->mutex)
        {
            enif_mutex_destroy(keydir->mutex);
        }

        free_keydir(env, keydir);
    }
}

static void bitcask_nifs_lock_resource_cleanup(ErlNifEnv* env, void* arg)
{
    bitcask_lock_handle* handle = (bitcask_lock_handle*)arg;
    lock_release(handle);
}

static int on_load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
{
    bitcask_keydir_RESOURCE = enif_open_resource_type_compat(env, "bitcask_keydir_resource",
                                                      &bitcask_nifs_keydir_resource_cleanup,
                                                      ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER,
                                                      0);

    bitcask_lock_RESOURCE = enif_open_resource_type_compat(env, "bitcask_lock_resource",
                                                    &bitcask_nifs_lock_resource_cleanup,
                                                    ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER,
                                                    0);
    // Initialize shared keydir hashtable
    bitcask_priv_data* priv = enif_alloc_compat(env, sizeof(bitcask_priv_data));
    priv->global_keydirs = kh_init(global_keydirs);
    priv->global_keydirs_lock = enif_mutex_create("bitcask_global_handles_lock");
    *priv_data = priv;

    // Initialize atoms that we use throughout the NIF.
    ATOM_ALLOCATION_ERROR = enif_make_atom(env, "allocation_error");
    ATOM_ALREADY_EXISTS = enif_make_atom(env, "already_exists");
    ATOM_BITCASK_ENTRY = enif_make_atom(env, "bitcask_entry");
    ATOM_ERROR = enif_make_atom(env, "error");
    ATOM_FALSE = enif_make_atom(env, "false");
    ATOM_FSTAT_ERROR = enif_make_atom(env, "fstat_error");
    ATOM_FTRUNCATE_ERROR = enif_make_atom(env, "ftruncate_error");
    ATOM_GETFL_ERROR = enif_make_atom(env, "getfl_error");
    ATOM_ILT_CREATE_ERROR = enif_make_atom(env, "ilt_create_error");
    ATOM_ITERATION_IN_PROCESS = enif_make_atom(env, "iteration_in_process");
    ATOM_ITERATION_NOT_PERMITTED = enif_make_atom(env, "iteration_not_permitted");
    ATOM_ITERATION_NOT_STARTED = enif_make_atom(env, "iteration_not_started");
    ATOM_LOCK_NOT_WRITABLE = enif_make_atom(env, "lock_not_writable");
    ATOM_NOT_FOUND = enif_make_atom(env, "not_found");
    ATOM_NOT_READY = enif_make_atom(env, "not_ready");
    ATOM_OK = enif_make_atom(env, "ok");
    ATOM_PREAD_ERROR = enif_make_atom(env, "pread_error");
    ATOM_PWRITE_ERROR = enif_make_atom(env, "pwrite_error");
    ATOM_READY = enif_make_atom(env, "ready");
    ATOM_SETFL_ERROR = enif_make_atom(env, "setfl_error");
    ATOM_TRUE = enif_make_atom(env, "true");

    return 0;
}

ERL_NIF_INIT(bitcask_nifs, nif_funcs, &on_load, NULL, NULL, NULL);


