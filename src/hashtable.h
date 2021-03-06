/*= -*- c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *
 * hashtable.h -- a generic hashtable implementation.
 *
 * Copyright (C) 2003 by Donovan Baarda <abo@minkirri.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */
#ifndef _HASHTABLE_H_
#define _HASHTABLE_H_

#include <assert.h>
#include <stdlib.h>

/** Simple hashtable.
 *
 * This is a minimal hashtable containing pointers to arbitrary
 * entries with configurable hashtable size and support for custom
 * hash() and cmp() methods. The cmp() method can either be a simple
 * comparison between two keys, or can be against a special match
 * object containing additional mutable state. This allows for things
 * like deferred and cached evaluation of costly comparison data.
 * The hash() function doesn't need to avoid clustering behaviour.
 *
 * It uses open addressing with quadratic probing for collisions. The
 * MurmurHash3 finalization function is used on the hash() output to
 * avoid clustering. There is no support for removing entries, only
 * adding them. Multiple entries with the same key can be added, and
 * you can use a fancy cmp() function to find particular entries by
 * more than just their key. There is an iterator for iterating
 * through all entries in the hashtable. There are optional
 * hashtable_find() find/match/hashcmp/entrycmp stats counters that
 * can be disabled by defining HASHTABLE_NSTATS.
 *
 * Example:
 *
 *   typedef ... key_t;
 *   int key_hash(const key_t *e);
 *   int key_cmp(key_t *e, const key_t *o);
 *
 *   typedef struct entry {
 *     key_t key;  // Inherit from key_t.
 *     ...extra entry value data...
 *   } entry_t;
 *   void entry_init(entry_t *e, ...);
 *
 *   hashtable_t *t;
 *   entry_t entries[300];
 *   key_t k;
 *   entry_t *e;
 *
 *   t = hashtable_new(300, &key_hash, &key_cmp);
 *   entry_init(&entries[5], ...);
 *   hashtable_add(t, &entries[5]);
 *   k = ...;
 *   e = hashtable_find(t, &k);
 *
 *   hashtable_iter i;
 *   for (e = hashtable_iter(&i, t); e != NULL; e = hashtable_next(&i))
 *     ...
 *
 *   hashtable_free(t);
 *
 * The hash() and cmp() fuctions will typically take pointers to
 * key/entry instances the same as the pointers stored in the
 * hashtable. However it is also possible for them to take "match
 * objects" that are a "subclass" of the entry type that contain
 * additional state for complicated comparision operations.
 *
 * Example:
 *
 *   typedef struct match {
 *     key_t key;  // Inherit from key_t;
 *     ...extra match criteria and state data...
 *   } match_t;
 *   int match_cmp(match_t *m, const entry_t *e);
 *
 *   ...
 *   match_t m;
 *
 *   t = hashtable_new(300, &key_hash, &match_cmp);
 *   ...
 *   m = ...;
 *   e = hashtable_find(t, &m);
 *
 * The cmp() function is only called for finding hashtable entries
 * and can mutate the match_t object for doing things like deferred
 * and cached evaluation of expensive match data. It can also access
 * the whole entry_t object to match against more than just the key. */

/** The hash() function type.
 *
 * Args:
 *   *k - the key or match object to hash.
 *
 * Returns:
 *   An unsigned hash value.
 */
typedef unsigned (*hash_f) (const void *k);

/** The cmp() function type.
 *
 * Args:
 *   *k - the key or match object to try and match to.
 *   *o - the key or entry object to match against.
 *
 * Returns:
 *   -1, 0, or 1 if *e is less, equal, or more that *o. */
typedef int (*cmp_f) (void *k, const void *o);

/** The hashtable type. */
typedef struct _hashtable {
    int size;                   /* Size of allocated hashtable. */
    int count;                  /* Number of entries in hashtable. */
    hash_f hash;                /* Function for hashing entries. */
    cmp_f cmp;                  /* Function for comparing entries. */
#ifndef HASHTABLE_NSTATS
    /* The following are for accumulating hashtable_find() stats. */
    long find_count;            /* The count of finds tried. */
    long match_count;           /* The count of matches found. */
    long hashcmp_count;         /* The count of hash compares done. */
    long entrycmp_count;        /* The count of entry compares done. */
#endif
    void **etable;              /* Table of pointers to entries. */
    unsigned ktable[];          /* Table of hash keys. */
} hashtable_t;

/** The hashtable iterator type. */
typedef struct _hashtable_iter {
    hashtable_t *htable;        /* The hashtable to iterate over. */
    int index;                  /* The index to scan from for the next entry. */
} hashtable_iter_t;

/** Allocate and initialize a hashtable instance.
 *
 * The provided size is used as an indication of the number of
 * entries you wish to add, but the allocated size will probably be
 * larger depending on the implementation to enable optimisations or
 * avoid degraded performance. It may be possible to fill the table
 * beyond the requested size, but performance can start to degrade
 * badly if it is over filled.
 *
 * Args:
 *   size - The desired minimum size of the hash table.
 *   hash - The hash function to use.
 *   cmp - The compare function to use.
 *
 * Returns:
 *   The initialized hashtable instance or NULL if it failed. */
hashtable_t *hashtable_new(int size, hash_f hash, cmp_f cmp);

/** Destroy and free a hashtable instance.
 *
 * This will free the hashtable, but will not free the entries in the
 * hashtable. If you want to free the entries too, use a hashtable
 * iterator to free the the entries first.
 *
 * Args:
 *   *t - The hashtable to destroy and free. */
void hashtable_free(hashtable_t *t);

/** Add an entry to a hashtable.
 *
 * This doesn't use cmp() or do any checks for existing copies or
 * instances, so it will add duplicates. If you want to avoid adding
 * duplicates, use hashtable_find() to check for existing entries
 * first.
 *
 * Args:
 *   *t - The hashtable to add to.
 *   *e - The entry object to add.
 *
 * Returns:
 *   The added entry, or NULL if the table is full. */
void *hashtable_add(hashtable_t *t, void *e);

/** Find an entry in a hashtable.
 *
 * Uses cmp() to find the first matching entry in the table in the
 * same hash() bucket.
 *
 * Args:
 *   *t - The hashtable to search.
 *   *m - The key or match object to search for.
 *
 * Returns:
 *   The first found entry, or NULL if nothing was found. */
void *hashtable_find(hashtable_t *t, void *m);

/** Initialize a hashtable_iter_t and return the first entry.
 *
 * This works together with hashtable_next() for iterating through
 * all entries in a hashtable.
 *
 * Example:
 *   for (e = hashtable_iter(&i, t); e != NULL; e = hashtable_next(&i))
 *     ...
 *
 * Args:
 *   *i - the hashtable iterator to initialize.
 *   *t - the hashtable to iterate over.
 *
 * Returns:
 *   The first entry or NULL if the hashtable is empty. */
void *hashtable_iter(hashtable_iter_t *i, hashtable_t *t);

/** Get the next entry from a hashtable iterator or NULL when finished.
 *
 * Args:
 *   *i - the hashtable iterator to use.
 *
 * Returns:
 *   The next entry or NULL if the iterator is finished. */
void *hashtable_next(hashtable_iter_t *i);

#endif                          /* _HASHTABLE_H_ */
