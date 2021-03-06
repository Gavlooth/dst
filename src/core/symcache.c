/*
* Copyright (c) 2017 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

/* The symbol cache is an open hashtable with all active symbols in the program
 * stored in it. As the primary use of symbols is table lookups and equality
 * checks, all symbols are interned so that there is a single copy of it in the
 * whole program. Equality is then just a pointer check. */

#include <dst/dst.h>
#include "gc.h"
#include "util.h"

/* Cache state */
const uint8_t **dst_vm_cache = NULL;
uint32_t dst_vm_cache_capacity = 0;
uint32_t dst_vm_cache_count = 0;
uint32_t dst_vm_cache_deleted = 0;

/* Initialize the cache (allocate cache memory) */
void dst_symcache_init() {
    dst_vm_cache_capacity = 1024;
    dst_vm_cache = calloc(1, dst_vm_cache_capacity * sizeof(const uint8_t **));
    if (NULL == dst_vm_cache) {
        DST_OUT_OF_MEMORY;
    }
    dst_vm_cache_count = 0;
    dst_vm_cache_deleted = 0;
}

/* Deinitialize the cache (free the cache memory) */
void dst_symcache_deinit() {
    free(dst_vm_cache);
    dst_vm_cache = NULL;
    dst_vm_cache_capacity = 0;
    dst_vm_cache_count = 0;
    dst_vm_cache_deleted = 0;
}

/* Mark an entry in the table as deleted. */
#define DST_SYMCACHE_DELETED ((const uint8_t *)0 + 1)

/* Find an item in the cache and return its location.
 * If the item is not found, return the location
 * where one would put it. */
static const uint8_t **dst_symcache_findmem(
        const uint8_t *str,
        int32_t len,
        int32_t hash,
        int *success) {
    uint32_t bounds[4];
    uint32_t i, j, index;
    const uint8_t **firstEmpty = NULL;

    /* We will search two ranges - index to the end,
     * and 0 to the index. */
    index = (uint32_t)hash & (dst_vm_cache_capacity - 1);
    bounds[0] = index;
    bounds[1] = dst_vm_cache_capacity;
    bounds[2] = 0;
    bounds[3] = index;
    for (j = 0; j < 4; j += 2)
        for (i = bounds[j]; i < bounds[j+1]; ++i) {
            const uint8_t *test = dst_vm_cache[i];
            /* Check empty spots */
            if (NULL == test) {
                if (NULL == firstEmpty)
                    firstEmpty = dst_vm_cache + i;
                goto notfound;
            }
            /* Check for marked deleted */
            if (DST_SYMCACHE_DELETED == test) {
                if (firstEmpty == NULL)
                    firstEmpty = dst_vm_cache + i;
                continue;
            }
            if (dst_string_equalconst(test, str, len, hash)) {
                /* Replace first deleted */
                *success = 1;
                if (firstEmpty != NULL) {
                    *firstEmpty = test;
                    dst_vm_cache[i] = DST_SYMCACHE_DELETED;
                    return firstEmpty;
                }
                return dst_vm_cache + i;
            }
        }
    notfound:
    *success = 0;
    return firstEmpty;
}

#define dst_symcache_find(str, success) \
    dst_symcache_findmem((str), dst_string_length(str), dst_string_hash(str), (success))

/* Resize the cache. */
static void dst_cache_resize(uint32_t newCapacity) {
    uint32_t i, oldCapacity;
    const uint8_t **oldCache = dst_vm_cache;
    const uint8_t **newCache = calloc(1, newCapacity * sizeof(const uint8_t **));
    if (newCache == NULL) {
        DST_OUT_OF_MEMORY;
    }
    oldCapacity = dst_vm_cache_capacity;
    dst_vm_cache = newCache;
    dst_vm_cache_capacity = newCapacity;
    dst_vm_cache_deleted = 0;
    /* Add all of the old cache entries back */
    for (i = 0; i < oldCapacity; ++i) {
        int status;
        const uint8_t **bucket;
        const uint8_t *x = oldCache[i];
        if (x != NULL && x != DST_SYMCACHE_DELETED) {
            bucket = dst_symcache_find(x, &status);
            if (status || bucket == NULL) {
                /* there was a problem with the algorithm. */
                break;
            }
            *bucket = x;
        }
    }
    /* Free the old cache */
    free(oldCache);
}

/* Add an item to the cache */
static void dst_symcache_put(const uint8_t *x, const uint8_t **bucket) {
    if ((dst_vm_cache_count + dst_vm_cache_deleted) * 2 > dst_vm_cache_capacity) {
        int status;
        dst_cache_resize(dst_tablen((2 * dst_vm_cache_count + 1)));
        bucket = dst_symcache_find(x, &status);
    }
    /* Add x to the cache */
    dst_vm_cache_count++;
    *bucket = x;
}

/* Remove a symbol from the symcache */
void dst_symbol_deinit(const uint8_t *sym) {
    int status = 0;
    const uint8_t **bucket = dst_symcache_find(sym, &status);
    if (status) {
        dst_vm_cache_count--;
        dst_vm_cache_deleted++;
        *bucket = DST_SYMCACHE_DELETED;
    }
}

/* Create a symbol from a byte string */
const uint8_t *dst_symbol(const uint8_t *str, int32_t len) {
    int32_t hash = dst_string_calchash(str, len);
    uint8_t *newstr;
    int success = 0;
    const uint8_t **bucket = dst_symcache_findmem(str, len, hash, &success);
    if (success)
        return *bucket;
    newstr = (uint8_t *) dst_gcalloc(DST_MEMORY_SYMBOL, 2 * sizeof(int32_t) + len + 1)
        + (2 * sizeof(int32_t));
    dst_string_hash(newstr) = hash;
    dst_string_length(newstr) = len;
    memcpy(newstr, str, len);
    newstr[len] = 0;
    dst_symcache_put((const uint8_t *)newstr, bucket);
    return newstr;
}

/* Get a symbol from a cstring */
const uint8_t *dst_csymbol(const char *cstr) {
    int32_t len = 0;
    while (cstr[len]) len++;
    return dst_symbol((const uint8_t *)cstr, len);
}

/* Convert a string to a symbol */
const uint8_t *dst_symbol_from_string(const uint8_t *str) {
    int success = 0;
    const uint8_t **bucket = dst_symcache_find(str, &success);
    if (success)
        return *bucket;
    dst_symcache_put((const uint8_t *)str, bucket);
    dst_gc_settype(dst_string_raw(str), DST_MEMORY_SYMBOL);
    return str;
}

/* Helper for creating a unique string. Increment an integer
 * represented as an array of integer digits. */
static void inc_counter(uint8_t *digits, int base, int len) {
    int i;
    uint8_t carry = 1;
    for (i = len - 1; i >= 0; --i) {
        digits[i] += carry;
        carry = 0;
        if (digits[i] == base) {
            digits[i] = 0;
            carry = 1;
        }
    }
}

/* Generate a unique symbol. This is used in the library function gensym. The
 * symbol will be of the format prefix_XXXXXX, where X is a base64 digit, and
 * prefix is the argument passed.  */
const uint8_t *dst_symbol_gen(const uint8_t *buf, int32_t len) {
    const uint8_t **bucket = NULL;
    int32_t hash = 0;
    uint8_t counter[6] = {63, 63, 63, 63, 63, 63};
    /* Leave spaces for 6 base 64 digits and two dashes. That means 64^6 possible suffixes, which
     * is enough for resolving collisions. */
    int32_t newlen = len + 7;
    int32_t newbufsize = newlen + 2 * sizeof(int32_t) + 1;
    uint8_t *str = (uint8_t *)dst_gcalloc(DST_MEMORY_SYMBOL, newbufsize) + 2 * sizeof(int32_t);
    dst_string_length(str) = newlen;
    memcpy(str, buf, len);
    str[len] = '_';
    str[newlen] = 0;
    uint8_t *saltbuf = str + len + 1;
    int status = 1;
    while (status) {
        int i;
        inc_counter(counter, 64, 6);
        for (i = 0; i < 6; ++i)
            saltbuf[i] = dst_base64[counter[i]];
        hash = dst_string_calchash(str, newlen);
        bucket = dst_symcache_findmem(str, newlen, hash, &status);
    }
    dst_string_hash(str) = hash;
    dst_symcache_put((const uint8_t *)str, bucket);
    return (const uint8_t *)str;
}
