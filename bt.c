/* B-tree Implementation.
 *
 * Implements in memory b-tree tables with insert/del/replace/find/ ops

GPL License

Copyright (c) 2010 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of AlchemyDatabase

    AlchemyDatabase is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AlchemyDatabase is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with AlchemyDatabase.  If not, see <http://www.gnu.org/licenses/>.

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <strings.h>

#include "redis.h"
#include "zmalloc.h"
#include "btree.h"
#include "row.h"
#include "common.h"
#include "bt_iterator.h"
#include "bt.h"

/* GLOBALS */
#define RL4 redisLog(4,
extern char *COLON;

/* PROTOTYPES */
static uint32 skipToVal(uchar **stream);

/* Abstract-BTREE Prototypes */
static bt *abt_create(uchar ktype, int num, uchar is_index);
static void abt_destroy(bt *nbtr, bt *btr);

bt *btCreate(uchar ktype, int num, uchar is_index) {
    return abt_create(ktype, num, is_index);
}

robj *createBtreeObject(uchar ktype, int num, uchar is_index) { /*Data & Index*/
    bt *btr = btCreate(ktype, num, is_index);
    return createObject(REDIS_BTREE, btr);
}
robj *createEmptyBtreeObject() {                           /* Virtual indices */
    return createObject(REDIS_BTREE, NULL);
}
bt *createIndexNode(uchar pktype) {                       /* Nodes of Indices */
    return btCreate(pktype, -1, BTREE_INDEX_NODE);
}

void btDestroy(bt *nbtr, bt *btr) {
    abt_destroy(nbtr, btr);
}

void freeBtreeObject(robj *o) {
    bt *btr = (bt *)(o->ptr);
    if (!btr) return; /* virtual indices have a NULL here */
    btDestroy(btr, NULL);
}

//TODO the following 3 functions should go into bt_code.c
static void destroy_bt_node(bt *btr, bt_n *n, uchar vtype) {
    for (int i = 0; i < n->n; i++) {
        void *be    = KEYS(btr, n)[i];
        int   ssize = getStreamMallocSize(be, vtype, btr->is_index);
        if (btr->is_index == BTREE_INDEX) { /* Index is BT of IndexNodeBTs */
            uchar *stream = be;
            skipToVal(&stream);
            bt    **nbtr  = (bt **)stream;
            destroy_bt_node(*nbtr, (*nbtr)->root, BTREE_INDEX_NODE);
            bt_free_btree(*nbtr, btr);      /* memory management in btr(Index)*/
        }
        bt_free(be, btr, ssize);
    }
    if (!n->leaf) {
        for (int i = 0; i <= n->n; i++) {
            destroy_bt_node(btr, NODES(btr, n)[i], vtype);
        }
    }
    bt_free_btreenode(n, btr); /* memory management in btr */
}

/* bt_release means dont destroy data, just btree */
static void bt_release(bt *btr, bt_n *n) {
    if (!n->leaf) {
        for (int i = 0; i <= n->n; i++) {
            bt_release(btr, NODES(btr, n)[i]);
        }
    }
    bt_free_btreenode(n, btr); /* memory management in btr */
}

static void bt_to_bt_insert(bt *nbtr, bt *obtr, bt_n *n) {
    for (int i = 0; i < n->n; i++) {
        char *be = KEYS(obtr, n)[i];
        bt_insert(nbtr, be);
    }
    if (!n->leaf) {
        for (int i = 0; i <= n->n; i++) {
            bt_to_bt_insert(nbtr, obtr, NODES(obtr, n)[i]);
        }
    }
}
          
/* STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM */
/* STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM STREAM */

/* TODO these flags should be #defines */
//TODO this should be a one-liner, not a nested if-else
static uchar getSflag(uchar b1) {
    if      (b1 & 1)  return 1;
    else if (b1 & 2)  return 2;
    else if (b1 & 4)  return 4;
    else if (b1 & 8)  return 8;
    else if (b1 & 16) return 16;
    else if (b1 & 32) return 32;
    else {
        RL4 "b1: %d", b1);
        assert(!"getSflag programming error");
    }
}

static inline uint32 get14BitInt(uchar *s) {
    uint32 key   = (uint32)(*((unsigned short *)s));
    key         -= 2;
    key         /= 4;
    return key;
}
static inline uint32 get28BitInt(uchar *s) {
    uint32 key  = *((uint32 *)s);
    key        -= 8;
    key        /= 16;
    return key;
}
static inline uint32 getInt(uchar **s) {
    *s         = *s + 1;
    uint32 key = *((uint32 *)*s);
    return key;
}
static inline uchar *getTinyString(uchar *s, uint32 *slen) {
    *slen = ((int)*s / 2);
    s++;
    return s;
}
static inline uchar *getString(uchar *s, uint32 *slen) {
    s++;
    *slen  = *((uint32 *)s);
    s     += 4;
    return s;
}
static inline float getFloat(uchar *s) {
    s++;
    float f;
    memcpy(&f, s, 4);
    return f;
}

int btStreamCmp(void *a, void *b) {
    if (!a || !b) return -1;
    uchar *s1     = (uchar *)a;
    uchar *s2     = (uchar *)b;
    uchar  sflag1 = getSflag(*s1);
    uchar  sflag2 = getSflag(*s2);

    if (sflag1 == 1 || sflag1 == 4) { // STRING
        uint32 slen1, slen2;
        slen1 = slen2 = 0; /* compiler warning */
        if (sflag1 == 1)      s1 = getTinyString(s1, &slen1);
        else if (sflag1 == 4) s1 = getString(    s1, &slen1);
        if (sflag2 == 1)      s2 = getTinyString(s2, &slen2);
        else if (sflag2 == 4) s2 = getString(    s2, &slen2);

        if (slen1 == slen2) return strncmp((char *)s1, (char *)s2, slen1);
        else {
            int i   = (slen1 < slen2) ? slen1 : slen2;
            int ret = strncmp((char *)s1, (char *)s2, i); 
            return (ret == 0) ? ((slen1 < slen2) ? -1 : 1) : ret;
        }
    } else if (sflag1 <= 16) {        // INT
        uint32 key1, key2;
        if      (sflag1 == 2)   key1  = get14BitInt(s1);
        else if (sflag1 == 8)   key1  = get28BitInt(s1);
        else  /* sflag == 16 */ key1  = getInt(&s1); 

        if      (sflag2 == 2)   key2  = get14BitInt(s2);
        else if (sflag2 == 8)   key2  = get28BitInt(s2);
        else  /* sflag == 16 */ key2  = getInt(&s2);
        return (key1 == key2) ? 0 : ((key1 > key2) ? 1 : -1);
    } else {                          // FLOAT
        float key1 = getFloat(s1);
        float key2 = getFloat(s2);
        float f    = key1 - key2;
        return (f == 0.0) ? 0 : ((f > 0.0) ? 1: -1);
    }
    return 0;
}

#define SIMKEY_BUFFER_SIZE 2048
static char SimKeyBuffer[SIMKEY_BUFFER_SIZE]; /*avoid malloc()s */

void destroySimKey(char *simkey, bool med) {
    if (med) free(simkey);
}

char *createSimKeyFromRaw(void    *key_ptr,
                          int      ktype, 
                          bool    *med,
                          uchar   *sflag,
                          uint32  *ksize) {
    assert(key_ptr || ktype != COL_TYPE_STRING); /* INT & FLOAT can be 0 */
    *med           = 0;
    char   *simkey = NULL; /* compiler warning */
    uint32  data   = 0;
    if (ktype == COL_TYPE_STRING) {
        assert(sdslen(key_ptr) < TWO_POW_32);
        if (sdslen(key_ptr) < TWO_POW_7) { // tiny STRING
            *sflag     = 1;
            *ksize     = sdslen(key_ptr) + 1;
            data       = sdslen(key_ptr) * 2 + 1;
            if (sdslen(key_ptr) + 1 >= SIMKEY_BUFFER_SIZE) {
                simkey = malloc(sdslen(key_ptr) + 1); /* MUST be freed soon */
                *med   = 1;
            } else {
                simkey = SimKeyBuffer;
            }
            *simkey    = (char)data;
            memcpy(simkey + 1, key_ptr, sdslen(key_ptr));
        } else {                           // STRING
            uint32 len = sdslen(key_ptr);
            *sflag     = 4;
            *ksize     = sdslen(key_ptr) + 5;
            if (sdslen(key_ptr) + 5 >= SIMKEY_BUFFER_SIZE) {
                simkey = malloc(sdslen(key_ptr) + 5); /* MUST be freed soon */
                *med   = 1;
            } else {
                simkey = SimKeyBuffer;
            }
            *simkey    = 4;
            data       = len;
            memcpy(simkey + 1, &data, 4);
            memcpy(simkey + 5, key_ptr, sdslen(key_ptr));
        }
    } else if (ktype == COL_TYPE_INT) {
        ulong i = (ulong)key_ptr;
        simkey  = SimKeyBuffer;
        if (i >= TWO_POW_32) {
            //redisLog(REDIS_WARNING, "column value > UINT_MAX");
            return NULL;
        }
        if (i < TWO_POW_14) {        // 14bit INT
            ushort m = (ushort)(i * 4 + 2);
            memcpy(simkey, &m, 2);
            *sflag    = 2;
            *ksize    = 2;
        } else if (i < TWO_POW_28) { // 28bit INT
            data      = (i * 16 + 8);
            memcpy(simkey, &data, 4);
            *sflag    = 8;
            *ksize    = 4;
        } else {                     // INT
            *simkey   = 16;
            data      = i;
            memcpy(simkey + 1, &data, 4);
            *sflag    = 16;
            *ksize    = 5;
        }
    } else if (ktype == COL_TYPE_FLOAT) {
        *sflag  = 32;
        *ksize  = 5;
        float f = atof(key_ptr);
        simkey  = SimKeyBuffer;
        *simkey = 32;
        memcpy(simkey + 1, &f, 4);
    }
    return simkey; /* MUST be freed soon */
}

//TODO rename CreateBtreeKey()
char *createSimKey(const robj *key,
                   int         ktype,
                   bool       *med,
                   uchar      *sflag,
                   uint32     *ksize) {
    void *ptr = NULL; /* compiler warning */
    if (ktype == COL_TYPE_INT) {
        if (key->encoding == REDIS_ENCODING_INT) ptr = (key->ptr);
        else                                     ptr = (void *)atol(key->ptr);
    } else if (ktype == COL_TYPE_STRING) {
        ptr = key->ptr;
    } else if (ktype == COL_TYPE_FLOAT) {
        ptr = key->ptr;
    }
    return createSimKeyFromRaw(ptr, ktype, med, sflag, ksize);
}

void destroyAssignKeyRobj(robj *key) {
   if (key->encoding == REDIS_ENCODING_RAW) {
        sdsfree(key->ptr); /* free from assignKeyRobj sflag[1,4] */
        key->ptr = NULL;
    }
}
//TODO rename assignKeyToRobj()
void assignKeyRobj(uchar *stream, robj *key) {
    uint32  k, slen;
    uchar   b1     = *stream;
    uchar   sflag  = getSflag(b1);
    if (sflag == 1) {         // tiny STRING
        stream        = getTinyString(stream, &slen);
        key->encoding = REDIS_ENCODING_RAW;
        key->ptr      = sdsnewlen(stream, slen); /* must be freed */
    } else if (sflag == 2) {  // tiny short
        k             = get14BitInt(stream);
        key->encoding = REDIS_ENCODING_INT;
        key->ptr      = (void*)((long)k);
    } else if (sflag == 4) {  // STRING
        stream        = getString(stream, &slen);
        key->encoding = REDIS_ENCODING_RAW;
        key->ptr      = sdsnewlen(stream, slen); /* must be freed */
    } else if (sflag == 8) {  // 28bit INT
        k             = get28BitInt(stream);
        key->encoding = REDIS_ENCODING_INT;
        key->ptr      = (void*)((long)k);
    } else if (sflag == 16) { // INT
        k             = getInt(&stream);
        key->encoding = REDIS_ENCODING_INT;
        key->ptr      = (void*)((long)k);
    } else if (sflag == 32) { // FLOAT
        char buf[32];
        double f      = getFloat(stream);
        sprintfOutputFloat(buf, 32, f);
        key->encoding = REDIS_ENCODING_RAW;
        key->ptr      = sdsnewlen(buf, strlen(buf)); /* must be freed */
    }
    key->type     = REDIS_STRING;
    key->refcount = 1;
}

static uint32 skipToVal(uchar **stream) {
    uchar   sflag = getSflag(**stream);
    uint32  klen  = 0;
    uint32  slen  = 0;
    if (sflag == 1) {         // TINY STRING
        getTinyString(*stream, &slen);
        klen = slen + 1;
    } else if (sflag == 2) {  // 14bit INT
        klen = 2;
    } else if (sflag == 4) {  // STRING
        getString(*stream, &slen);
        klen = slen + 5;
    } else if (sflag == 8) {  // 28bit INT
        klen = 4;
    } else if (sflag == 16) { // INT
        klen = 5;
    } else if (sflag == 32) { // FLOAT
        klen = 5;
    }
    *stream += klen;
    return klen;
}

void assignValRobj(uchar *stream, int vtype, robj *val, uchar is_index) {
    skipToVal(&stream);
    val->type = vtype;

    if (vtype == REDIS_ROW) {
        val->ptr     = stream;
    } else { /* REDIS_BTREE */
        if (is_index == BTREE_INDEX_NODE) {
            val->ptr     = NULL;
        } else { /* INDEX */
            char **p_ptr = (char **)stream;
            val->ptr     = *p_ptr;
        }
    }
}

uint32 getStreamMallocSize(uchar *stream,
                           int    vtype,
                           uchar  is_index) {
    uint32 vlen;
    uint32 klen = skipToVal(&stream);

    if (vtype == REDIS_ROW) {
        vlen = getRowMallocSize(stream);
    } else { /* REDIS_BTREE */
        if (is_index == BTREE_INDEX_NODE) {
            vlen = 0;
        } else { /* INDEX */
            char **p_ptr = (char **)stream;
            bt    *btr   = (bt *)*p_ptr;
            vlen = sizeof(void *);
            if (btr) vlen += (uint32)btr->malloc_size;
        }
    }
    return klen + vlen;
}


/* ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE */
/* ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE */
static void *abt_access_raw_val(bt *btr, const robj *key, int ktype, bool del) {
    bool  med; uchar sflag; uint32 ksize;
    char *simkey = createSimKey(key, ktype, &med, &sflag, &ksize); /* FREE ME */
    if (!simkey) return NULL;
    uchar *stream = del ? bt_delete(btr, simkey) : bt_find(btr, simkey);
    destroySimKey(simkey, med);                                    /* freeD */
    return stream;
}

static bt *abt_create(uchar ktype, int num, uchar is_index) {
    bt *btr         = bt_create(btStreamCmp, TRANSITION_ONE_BTREE_BYTES);
    btr->ktype      = ktype;
    btr->is_index   = is_index;
    btr->num        = num;
    return btr;
}

static void abt_destroy(bt *nbtr, bt *btr) {
    if (nbtr->root) {
        uchar t = (nbtr->is_index == BTREE_TABLE) ? REDIS_ROW : REDIS_BTREE;
        destroy_bt_node(nbtr, nbtr->root, t);
        nbtr->root  = NULL;
    }
    bt_free_btree(nbtr, btr); /* memory management in btr */
}

/* NOTE: this function can NOT be used in nested loops
          - it relies on a single GLOBAL variable (BtRobj[2]) */
#define NUM_ROBJ_NESTING 2
/* ALL ALSOSQL routines MUST copy robj's when replying */
static robj BtRobj[NUM_ROBJ_NESTING]; /*avoid malloc()s */
static robj* abt_find_val(bt         *btr,
                          const robj *key,
                          int         ktype,
                          int         vtype,
                          int         nesting) {
    uchar *stream            = abt_access_raw_val(btr, key, ktype, 0);
    if (!stream) return NULL;
    BtRobj[nesting].type     = ktype;
    BtRobj[nesting].encoding = REDIS_ENCODING_RAW;
    BtRobj[nesting].refcount = 1;
    assignValRobj(stream, vtype, &BtRobj[nesting], btr->is_index);
    return &BtRobj[nesting];
}

static int abt_del(bt *btr, const robj *key, int ktype, int vtype) {
    uchar *stream = abt_access_raw_val(btr, key, ktype, 1);
    if (!stream) return 0;
    uint32  ssize = getStreamMallocSize(stream, vtype, btr->is_index);
    //RL4 "is_index: %d vtype: %d free: %p size: %u", btr->is_index, vtype, stream, ssize);
    bt_free(stream, btr, ssize); /* memory bookkeeping in btr */
    return 1;
}

static uint32 abt_insert(bt *btr, robj *key, robj *val, int ktype, int vtype) {
    if (btr->numkeys == TRANSITION_ONE_MAX) {
        btr = abt_resize(btr, TRANSITION_TWO_BTREE_BYTES);
    }

    bool  med; uchar sflag; uint32 ksize;
    char  *simkey  = createSimKey(key, ktype, &med, &sflag, &ksize);
    if (!simkey) return 0;
    uint32   ssize   = ksize;
    void    *val_ptr = val ? val->ptr : NULL;
    uint32   vlen    = 0;;
    if (vtype == REDIS_ROW) vlen = getRowMallocSize(val_ptr);
    else if (val_ptr)       vlen = sizeof(void *);
    ssize += vlen;

    char *bt_val   = bt_malloc(ssize, btr); /* mem bookkeeping done in BT */
    char *o_bt_val = bt_val;

    memcpy(bt_val, simkey, ksize);
    bt_val += ksize;
    destroySimKey(simkey, med);                                    /* freeD */

    if (vtype == REDIS_ROW) memcpy(bt_val, val_ptr, vlen);
    else if (val_ptr)       memcpy(bt_val, &val_ptr, sizeof(void *));
    bt_val += vlen;

    //RL4 "bt_insert: size: %u ksize: %u vlen: %u", ssize, ksize, vlen);
    bt_insert(btr, o_bt_val);
    return ssize;
}

bt *abt_resize(bt *obtr, int new_size) {
     bt *nbtr         = bt_create(btStreamCmp, new_size);
     nbtr->ktype      = obtr->ktype;
     nbtr->is_index   = obtr->is_index;
     nbtr->num        = obtr->num;
     nbtr->data_size  = obtr->data_size;
    if (obtr->root) {
        /* newBT copied from old, then new's head copied over olds */
        bt_to_bt_insert(nbtr, obtr, obtr->root); /* 1.) copy from old to new */
        bt_release(obtr, obtr->root);            /* 2.) release old */
        memcpy(obtr, nbtr, sizeof(bt));          /* 3.) overwrite old w/ new */
        free(nbtr);                              /* 4.) free new */
    }
    //bt_dump_info(obtr, obtr->ktype, REDIS_ROW);
    return obtr;
}

/* API API API  API API API  API API API  API API API  API API API  */
/* API API API  API API API  API API API  API API API  API API API  */
int btAdd(robj *o, void *key, void *val, int ktype) {
    bt   *btr = (bt *)(o->ptr);
    robj *v   = abt_find_val(btr, key, ktype, REDIS_ROW, 0);
    if (v) return DICT_ERR;
    else   return abt_insert(btr, key, val, ktype, REDIS_ROW);
}

//TODO need a native BT bt_replace, no need to mess w/ the btree for a replace
//                         just replace pointer
int btReplace(robj *o, void *key, void *val, int ktype) {
    bt  *btr = (bt *)(o->ptr);
    int  del = abt_del(btr, key, ktype, REDIS_ROW);
    if (!del) return DICT_ERR;
    abt_insert(btr, key, val, ktype, REDIS_ROW);
    return DICT_OK;
}

robj *btFindVal(robj *o, const void *key, int ktype) {
    if (!key) return NULL;
    bt *btr = (bt *)(o->ptr);
    return abt_find_val(btr, key, ktype, REDIS_ROW, 0);
}

int btDelete(robj *o, const void *key, int ktype) {
    bt *btr = (bt *)(o->ptr);
    int del = abt_del(btr, key, ktype, REDIS_ROW);
    if (!del) return DICT_ERR;
    else      return DICT_OK;
}

/* INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX */
/* INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX */
robj BtIndVal;
int btIndAdd(bt *ibtr, void *key, bt *nbtr, int ktype) {
    BtIndVal.ptr = nbtr;
    if (abt_find_val(ibtr, key, ktype, REDIS_BTREE, 1)) return DICT_ERR;
    abt_insert(ibtr, key, &BtIndVal, ktype, REDIS_BTREE);
    return DICT_OK;
}
robj *btIndFindVal(bt *ibtr, const void *key, int ktype) {
    return abt_find_val(ibtr, key, ktype, REDIS_BTREE, 1);
}
int btIndDelete(bt *ibtr, const void *key, int ktype) {
    abt_del(ibtr, key, ktype, REDIS_BTREE);
    return ibtr->numkeys;
}

/* INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE */
/* INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE */
int btIndNodeAdd(bt *nbtr, void *key, int ktype) {
    if (abt_find_val(nbtr, key, ktype, REDIS_BTREE, 1)) return DICT_ERR;
    abt_insert(nbtr, key, NULL, ktype, REDIS_BTREE);
    return DICT_OK;
}

int btIndNodeDelete(bt *nbtr, const void *key, int ktype) {
    abt_del(nbtr, key, ktype, REDIS_BTREE);
    return nbtr->numkeys;
}

/* JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT */
/* JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT JOIN_BT */
#define INIT_JOIN_BTREE_BYTES 1024
void btReleaseJoinRangeIterator(btIterator *iter);

static int intJoinRowCmp(void *a, void *b) {
    joinRowEntry *ja = (joinRowEntry *)a;
    joinRowEntry *jb = (joinRowEntry *)b;
    robj         *ra = ja->key;
    robj         *rb = jb->key;
    int ia, ib;
    if (ra->encoding == REDIS_ENCODING_RAW) ia = atoi(ra->ptr);
    else                                    ia = (int)(long)ra->ptr;
    if (rb->encoding == REDIS_ENCODING_RAW) ib = atoi(rb->ptr);
    else                                    ib = (int)(long)rb->ptr;
    return (ia == ib) ? 0 : (ia < ib) ? -1 : 1;
}

static int strJoinRowCmp(void *a, void *b) {
    joinRowEntry *ja = (joinRowEntry *)a;
    joinRowEntry *jb = (joinRowEntry *)b;
    robj         *ra = ja->key;
    robj         *rb = jb->key;
    return strcmp(ra->ptr, rb->ptr);
}

static int floatJoinRowCmp(void *a, void *b) {
    joinRowEntry *ja = (joinRowEntry *)a;
    joinRowEntry *jb = (joinRowEntry *)b;
    robj         *ra = ja->key;
    robj         *rb = jb->key;
    float         fa = atof(ra->ptr);
    float         fb = atof(rb->ptr);
    float         f  = fa - fb;
    return (f == 0.0) ? 0 : ((f > 0.0) ? 1: -1);
}

bt *createJoinResultSet(uchar pkt) {
    bt *btr = NULL; /* compiler warning */
    if (       pkt == COL_TYPE_INT) {
        btr = bt_create(intJoinRowCmp,   INIT_JOIN_BTREE_BYTES);
    } else if (pkt == COL_TYPE_STRING) {
        btr = bt_create(strJoinRowCmp,   INIT_JOIN_BTREE_BYTES);
    } else if (pkt == COL_TYPE_FLOAT) {
        btr = bt_create(floatJoinRowCmp, INIT_JOIN_BTREE_BYTES);
    }
    return btr;
}

void *btJoinFindVal(bt *jbtr, joinRowEntry *key) {
    return bt_find(jbtr, key);
}
int btJoinAddRow(bt *jbtr, joinRowEntry *key) {
    if (bt_find(jbtr, key)) return DICT_ERR;
    bt_insert(jbtr, key);
    return DICT_OK;
}

int btJoinDeleteRow(bt *jbtr, joinRowEntry *key) {
    bt_delete(jbtr, key);
    return jbtr->numkeys;
}

static void emptyJoinBtNode(bt   *jbtr,
                            bt_n *n,
                            int   ncols,
                            bool  is_ob,
                            void (*freer)(list *s, int ncols, bool is_ob)) {
    for (int i = 0; i < n->n; i++) {
        joinRowEntry *be  = KEYS(jbtr, n)[i];
        list         *val = be->val;
        freer(val, ncols, is_ob);      /* free list of ind_rows (cols,sizes) */
        decrRefCount(be->key);         /* free jk */
        free(be);                      /* free jre */
    }
    if (!n->leaf) {
        for (int i = 0; i <= n->n; i++) {
            emptyJoinBtNode(jbtr, NODES(jbtr, n)[i], ncols, is_ob, freer);
        }
    }
    bt_free_btreenode(n, jbtr); /* memory management in btr */
}

void btJoinRelease(bt   *jbtr,
                   int   ncols,
                   bool  is_ob,
                   void (*freer)(list *s, int ncols, bool is_ob)) {
    if (jbtr->root) {
        emptyJoinBtNode(jbtr, jbtr->root, ncols, is_ob, freer);
        jbtr->root = NULL;
        bt_free_btree(jbtr, NULL);
    }
}
