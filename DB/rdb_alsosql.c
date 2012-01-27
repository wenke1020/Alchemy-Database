/*
 * This file implements saving alsosql datastructures to rdb files
 *

AGPL License

Copyright (c) 2011 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of ALCHEMY_DATABASE

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

#include "redis.h"

#include "luatrigger.h"
#include "find.h"
#include "bt.h"
#include "index.h"
#include "lru.h"
#include "stream.h"
#include "ddl.h"
#include "alsosql.h"
#include "common.h"
#include "rdb_alsosql.h"

/* RDB TODO LIST
    1.) [sk, fk_cmatch, fk_otmatch, fk_ocmatch] -> PERSISTENT
    2.) rdbLoadBT() refactor into newTable() & newIndex() calls
    3.) DirtyStream needs to be saved/loaded w/ ROWs
*/

extern int Num_tbls; extern r_tbl_t *Tbl;   extern dict *TblD;
extern int Num_indx; extern r_ind_t *Index; extern dict *IndD;

extern dictType sdsDictType;

// CONSTANT GLOBALS
static uchar VIRTUAL_INDEX_TYPE = 255;
static uchar LUAT_JUST_ADD      = 0;
static uchar LUAT_WITH_DEL      = 1;

/* PROTOTYPES */
int       rdbSaveLen(FILE *fp, uint32_t len);
int       rdbSaveRawString(FILE *fp, sds s, size_t len);
int       rdbSaveStringObject(FILE *fp, robj *obj);
int       rdbSaveType(FILE *fp, unsigned char type);
uint32_t  rdbLoadLen(FILE *fp, int *isencoded);
int       rdbLoadType(FILE *fp);
robj     *rdbLoadStringObject(FILE *fp);


static int saveLtc(FILE *fp, ltc_t *ltc) {
    if (rdbSaveRawString(fp, ltc->fname, sdslen(ltc->fname)) == -1) return -1;
    if (rdbSaveLen(fp, ltc->tblarg)         == -1)                  return -1;
    if (rdbSaveLen(fp, ltc->ncols)          == -1)                  return -1;
    for (int j = 0; j < ltc->ncols; j++) {
        if (rdbSaveLen(fp, ltc->ics[j].cmatch) == -1)               return -1;
    }
    return 0;
}
int rdbSaveLuaTrigger(FILE *fp, r_ind_t *ri) {
    int     tmatch = ri->tmatch;
    luat_t *luat   = (luat_t *)ri->btr;
    robj *r = createStringObject(ri->name, sdslen(ri->name));
    if (rdbSaveStringObject(fp, r) == -1)                           return -1;
    decrRefCount(r);
    if (rdbSaveLen(fp, tmatch)    == -1)                            return -1;
    if (luat->del.ncols) {
        if (fwrite(&LUAT_WITH_DEL, 1, 1, fp) == 0)                  return -1;
    } else {
        if (fwrite(&LUAT_JUST_ADD, 1, 1, fp) == 0)                  return -1;
    }
    if                    (saveLtc(fp, &luat->add)         == -1)   return -1;
    if (luat->del.ncols && saveLtc(fp, &luat->del)         == -1)   return -1;
    return 0;
}
static bool loadLtc(FILE *fp, ltc_t *ltc) {
    robj *o;
    if (!(o = rdbLoadStringObject(fp)))                             return 0;
    ltc->fname = sdsdup(o->ptr);
    decrRefCount(o);
    uint32 u;
    if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)             return 0;
    ltc->tblarg = (bool)u;
    if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)             return 0;
    ltc->ncols = u;
    for (int j = 0; j < ltc->ncols; j++) {
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0; 
        ltc->ics[j].cmatch = (int)u;
        //TODO FIXME populate "lo"
        ltc->ics[j].lo     = NULL;
    }
    return 1;
}
bool rdbLoadLuaTrigger(FILE *fp) {
    uint32 u;
    luat_t *luat   = init_lua_trigger();
    robj   *trname;
    if (!(trname = rdbLoadStringObject(fp)))                        return 0;
    if ((u     = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
    int     tmatch = (int)u;
    uchar which;
    if (fread(&which, 1, 1, fp) == 0)                               return 0;
    if (loadLtc(fp, &luat->add)              == 0)                  return 0;
    if (which == LUAT_WITH_DEL && loadLtc(fp, &luat->del) == 0)     return 0;
    DECLARE_ICOL(ic, -1)
    if ((newIndex(NULL, trname->ptr, tmatch, ic, NULL, 0, 0, 0,
                  luat, ic,          0,       0, 0)) == -1)         return 0;
    decrRefCount(trname);
    return 1;
}

static int rdbSaveAllRows(FILE *fp, bt *btr, bt_n *x) {
    for (int i = 0; i < x->n; i++) {
        uchar *stream  = (uchar *)KEYS(btr, x, i);
        int    ssize   = getStreamRowSize(btr, stream);
        uchar *wstream = UU(btr) ? (uchar *)&stream : stream;
        if (rdbSaveLen(fp, ssize)        == -1)                     return -1;
        if (fwrite(wstream, ssize, 1, fp) == 0)                     return -1;
    }
    if (!x->leaf) {
        for (int i = 0; i <= x->n; i++) {
            if (rdbSaveAllRows(fp, btr, NODES(btr, x)[i]) == -1)    return -1;
        }
    }
    return 0;
}
#define DEBUG_SAVE_DATA_BT \
  printf("SaveTable: tmatch: %d imatch: %d\n", tmatch, rt->vimatch);
#define DEBUG_SAVE_INDEX_BT \
  printf("SaveIndex: imatch: %d\n", imatch);

int rdbSaveBT(FILE *fp, bt *btr) { //printf("rdbSaveBT\n");
    if (!btr) {
        if (fwrite(&VIRTUAL_INDEX_TYPE, 1, 1, fp) == 0)         return -1;
        return 0;
    }
    if (fwrite(&(btr->s.btype), 1, 1, fp) == 0)                 return -1;
    int tmatch = btr->s.num;
    if (rdbSaveLen(fp, tmatch) == -1)                           return -1;
    if (btr->s.btype == BTREE_TABLE) { /* DATA */
        r_tbl_t *rt = &Tbl[tmatch]; //DEBUG_SAVE_DATA_BT
        if (rdbSaveLen(fp, rt->vimatch) == -1)                  return -1;
        robj *r = createStringObject(rt->name, sdslen(rt->name));
        if (rdbSaveStringObject(fp, r) == -1)                   return -1;
        decrRefCount(r);
        if (rdbSaveLen(fp, rt->col_count) == -1)                return -1;
        for (int i = 0; i < rt->col_count; i++) {
            robj *r = _createStringObject(rt->col[i].name);
            if (rdbSaveStringObject(fp, r) == -1)               return -1;
            decrRefCount(r);
            if (rdbSaveLen(fp, (int)rt->col[i].type) == -1)     return -1;
        }
        if (rdbSaveLen(fp, rt->hashy) == -1)                    return -1;
        if (fwrite(&(btr->s.ktype),    1, 1, fp) == 0)          return -1;

        if (rdbSaveLen(fp, btr->numkeys)       == -1)           return -1;
        if (btr->root && btr->numkeys > 0) {
            if (rdbSaveAllRows(fp, btr, btr->root) == -1)       return -1;
        }
    } else {                           /* INDEX */
        int      imatch = tmatch;
        r_ind_t *ri     = &Index[imatch]; //DEBUG_SAVE_INDEX_BT
        robj    *r      = createStringObject(ri->name, sdslen(ri->name));
        if (rdbSaveStringObject(fp, r) == -1)                   return -1;
        decrRefCount(r);
        if (rdbSaveLen(fp, ri->tmatch) == -1)                   return -1;
        if (ri->clist) {
            if (rdbSaveLen(fp, ri->nclist) == -1)               return -1;
            for (int i = 0; i < ri->nclist; i++) {
                if (rdbSaveLen(fp, ri->bclist[i].cmatch) == -1) return -1;
                //TODO FIXME save "lo"
            }
        } else {
            if (rdbSaveLen(fp, 1) == -1)                        return -1;
            if (rdbSaveLen(fp, ri->icol.cmatch) == -1)          return -1;
            if (rdbSaveLen(fp, ri->icol.nlo)    == -1)          return -1;
            if (ri->icol.nlo) {
                for (uint32 j = 0; j < ri->icol.nlo; j++) {
                    robj *r = _createStringObject(ri->icol.lo[j]);
                    if (rdbSaveStringObject(fp, r) == -1)       return -1;
                    decrRefCount(r);
                }
            }
        }
        if (rdbSaveLen(fp, ri->cnstr) == -1)                    return -1;
        if (rdbSaveLen(fp, ri->lru)   == -1)                    return -1;
        if (rdbSaveLen(fp, ri->lfu)   == -1)                    return -1;
        // NOTE: obc: -1 not handled well, so incr on SAVE, decr on LOAD
        if (rdbSaveLen(fp, (ri->obc.cmatch + 1)) == -1)         return -1;//INCR
        if (fwrite(&(btr->s.ktype),    1, 1, fp) == 0)          return -1;
    }
    return 0;
}

void *UUbuf;         ulk UL_RDBPointer; luk LU_RDBPointer; llk LL_RDBPointer;
uxk   UX_RDBPointer; xuk XU_RDBPointer; lxk LX_RDBPointer; xlk XL_RDBPointer;
xxk   XX_RDBPointer;
static int rdbLoadRow(FILE *fp, bt *btr, int tmatch) {
    uint32  ssize;
    if ((ssize = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)     return -1;
    void *stream = UU(btr) ? &UUbuf :         UL(btr) ? &UL_RDBPointer :
                   LU(btr) ? &LU_RDBPointer : LL(btr) ? &LL_RDBPointer :
                   UX(btr) ? &UX_RDBPointer : XU(btr) ? &XU_RDBPointer :
                   LX(btr) ? &LX_RDBPointer : XL(btr) ? &XL_RDBPointer :
                   XX(btr) ? &XX_RDBPointer : row_malloc(btr, ssize);
    if (fread(stream, ssize, 1, fp) == 0)                       return -1;
#ifndef TEST_WITH_TRANS_ONE_ONLY
    if (btr->numkeys == TRANS_ONE_MAX) btr = abt_resize(btr, TRANS_TWO);
#endif
    uint32 dr = 0; //TODO DirtyStream needs to be saved/loaded w/ ROWs
    if      UU(btr) bt_insert(btr, UUbuf,          dr);
    else if UL(btr) bt_insert(btr, &UL_RDBPointer, dr);
    else if LU(btr) bt_insert(btr, &LU_RDBPointer, dr);
    else if LL(btr) bt_insert(btr, &LL_RDBPointer, dr);
    else if UX(btr) bt_insert(btr, &UX_RDBPointer, dr);
    else if XU(btr) bt_insert(btr, &XU_RDBPointer, dr);
    else if LX(btr) bt_insert(btr, &LX_RDBPointer, dr);
    else if XL(btr) bt_insert(btr, &XL_RDBPointer, dr);
    else if XX(btr) bt_insert(btr, &XX_RDBPointer, dr);
    else            bt_insert(btr, stream,         dr); // ROW
    aobj apk;                   convertStream2Key(stream, &apk, btr);
    r_tbl_t *rt = &Tbl[tmatch]; UPDATE_AUTO_INC(rt->col[0].type, apk);
    releaseAobj(&apk);
    return 0;
}

#define DEBUG_LOAD_DATA_BT \
  printf("LoadTable: tmatch: %d imatch: %d\n", tmatch, imatch);
#define DEBUG_LOAD_INDEX_BT \
  printf("LoadIndex: imatch: %d\n", imatch);

//TODO refactor into newTable() & newIndex() calls
bool rdbLoadBT(FILE *fp) { //printf("rdbLoadBT\n");
    uint32  u; uchar   btype;
    if (fread(&btype, 1, 1, fp) == 0)                               return 0;
    if (btype == VIRTUAL_INDEX_TYPE)                                return 1;
    if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)             return 0;
    int tmatch = (int)u;
    if (btype == BTREE_TABLE) { /* BTree w/ DATA */
        r_tbl_t *rt   = &Tbl[tmatch]; initTable(rt);
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        int imatch    = u;
        //TODO [sk, fk_cmatch, fk_otmatch, fk_ocmatch] -> PERSISTENT
        rt->vimatch   = imatch; //DEBUG_LOAD_DATA_BT
        robj *r;
        if (!(r = rdbLoadStringObject(fp)))                         return 0;
        rt->name = sdsdup(r->ptr);
        decrRefCount(r);
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        rt->col_count = u;
        rt->col       = malloc(sizeof(r_col_t) * rt->col_count); // FREE 081
        bzero(rt->col, sizeof(r_col_t) * rt->col_count);
        rt->cdict     = dictCreate(&sdsDictType, NULL);          // FREE 090
        for (int i = 0; i < rt->col_count; i++) {
            robj *r;
            if (!(r = rdbLoadStringObject(fp)))                     return 0;
            sds       cname   = (sds)r->ptr;
            rt->col[i].name   = sdsdup(cname);                  // DEST 082
            ci_t *ci          = malloc(sizeof(ci_t));           // FREE 147
            bzero(ci, sizeof(ci_t));
            ci->cmatch        = i + 1;
            ASSERT_OK(dictAdd(rt->cdict, sdsdup(cname), ci));
            decrRefCount(r);
            if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)     return 0;
            rt->col[i].type   = (uchar)u;
            rt->col[i].imatch = -1;
        }
        if (fread(&u,    1, 1, fp) == 0)                            return 0;
        rt->hashy = (bool)u;
        if (fread(&u,    1, 1, fp) == 0)                            return 0;
        rt->btr   = createDBT(u, tmatch);
        uint32 bt_nkeys; 
        if ((bt_nkeys = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)  return 0;
        for (uint32 i = 0; i < bt_nkeys; i++) {
            if (rdbLoadRow(fp, rt->btr, tmatch) == -1)              return 0;
        }
        ASSERT_OK(dictAdd(TblD, sdsdup(rt->name), VOIDINT(tmatch + 1)));
        if (Num_tbls < (tmatch + 1)) Num_tbls = tmatch + 1;

        //TODO copy of newIndex() -> refactor
        /* BTREE implies an index on "tbl_pk_index" -> autogenerate */
        r_ind_t *ri       = &Index[imatch]; bzero(ri, sizeof(r_ind_t));
        ri->name          = P_SDS_EMT "%s_%s_%s", rt->name, rt->col[0].name,
                                                  INDEX_DELIM);
        ri->tmatch        =  tmatch; ri->icol.cmatch =  0; /* PK */
        ri->virt          =  1;      ri->cnstr       = CONSTRAINT_NONE;
        ri->obc.cmatch    = -1;
        ri->done          =  1; ri->ofst = -1;

        rt->col[0].imatch = imatch;
        if (!rt->ilist) rt->ilist  = listCreate();       // DEST 088
        listAddNodeTail(rt->ilist, VOIDINT imatch);
        {
            ci_t *ci = dictFetchValue(rt->cdict, rt->col[ri->icol.cmatch].name);
            if (!ci->ilist) ci->ilist = listCreate();        // FREE 148
            listAddNodeTail(ci->ilist, VOIDINT imatch);
        }
        rt->col[0].indxd  = 1;
        rt->vimatch       = imatch;

        ASSERT_OK(dictAdd(IndD, sdsdup(ri->name), VOIDINT(imatch + 1)));
        if (Num_indx < (imatch + 1)) Num_indx = imatch + 1;
    } else {                        /* INDEX */
        int imatch  = tmatch; //DEBUG_LOAD_INDEX_BT
        r_ind_t *ri = &Index[imatch]; bzero(ri, sizeof(r_ind_t));
        robj *r;
        if (!(r = rdbLoadStringObject(fp)))                         return 0;
        ri->name = sdsdup(r->ptr);
        decrRefCount(r);
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        ri->tmatch  = (int)u;
        r_tbl_t *rt = &Tbl[ri->tmatch];
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        if (u == 1) { /* Single Column */
            if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)     return 0;
            int cmatch             = (int)u;
            ri->icol.cmatch        = cmatch;
            if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)     return 0;
            ri->icol.nlo           = u;
            if (ri->icol.nlo) {
                ri->icol.lo = malloc(sizeof(sds) * ri->icol.nlo); // FREE 146
                for (uint32 j = 0; j < ri->icol.nlo; j++) {
                    if (!(r = rdbLoadStringObject(fp)))             return 0;
                    ri->icol.lo[j] = sdsdup(r->ptr);
                    decrRefCount(r);
                }
            }
            rt->col[cmatch].indxd  = 1; /* used in updateRow OVRWR */
            ri->nclist = 0; ri->bclist = NULL; ri->clist  = NULL;
        } else { /* MultipleColumnIndexes */
            rt->nmci++;
            ri->nclist  = u;
            ri->bclist  = malloc(ri->nclist * sizeof(icol_t)); /* FREE ME 053 */
            ri->clist   = listCreate();                  /* DESTROY ME 054 */
            DECLARE_ICOL(ic, -1)
            for (int i = 0; i < ri->nclist; i++) {
                if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return 0;
                int cmatch = (int)u;
                ic.cmatch  = cmatch;
                //TODO FIXME load "lo"
                if (!addC2MCI(NULL, ic, ri->clist))                 return 0;
                if (!i) ri->icol.cmatch = cmatch;
                ri->bclist[i].cmatch    = cmatch;
                //TODO FIXME load "lo"
                rt->col[cmatch].indxd   = 1; /* used in updateRow OVRWR */
            }
        }
        ri->virt    = 0;
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        ri->cnstr   = (int)u;
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        ri->lru     = (int)u;
        if (ri->lru) {
            rt->lrui = imatch; rt->lruc = ri->icol.cmatch;
            rt->lrud = (uint32)getLru(ri->tmatch);
        }
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        ri->lfu     = (int)u;
        if (ri->lfu) {
            rt->lfu = 1; rt->lfui = imatch; rt->lfuc = ri->icol.cmatch;
        }
        ri->luat    = 0;
        if ((u = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR)         return 0;
        // NOTE: obc: -1 not handled well, so incr on SAVE, decr on LOAD
        ri->obc.cmatch = ((int)u) - 1; //DECR
        ri->done       =  1; ri->ofst = -1;
        rt->col[ri->icol.cmatch].imatch = imatch;
        if (!rt->ilist) rt->ilist  = listCreate();       // DEST 088
        listAddNodeTail(rt->ilist, VOIDINT imatch);
        {
            ci_t *ci = dictFetchValue(rt->cdict, rt->col[ri->icol.cmatch].name);
            if (!ci->ilist) ci->ilist = listCreate();        // FREE 148
            listAddNodeTail(ci->ilist, VOIDINT imatch);
        }
        uchar ktype;
        if (fread(&ktype,    1, 1, fp) == 0)                        return 0;
        uchar pktyp = rt->col[0].type;
        ri->btr = ri->clist       ? createMCIndexBT(ri->clist, imatch)        :
                  UNIQ(ri->cnstr) ? createU_S_IBT  (ktype,     imatch, pktyp) :
                                    createIndexBT  (ktype,     imatch);
        ASSERT_OK(dictAdd(IndD, sdsdup(ri->name), VOIDINT(imatch + 1)));

        if (Num_indx < (imatch + 1)) Num_indx = imatch + 1;
    }
    return 1;
}

 // NOTE: Indexes are NEVER STORED, they are BUILT AFTER data is loaded
void rdbLoadFinished() { //printf("rdbLoadFinished\n");
    for (int imatch = 0; imatch < Num_indx; imatch++) {
        r_ind_t *ri  = &Index[imatch];
        if (!ri->name) continue; // previously deleted
        if (ri->virt)  continue;
        bt      *btr = getBtr(ri->tmatch);
        buildIndex(NULL, btr, imatch, -1);
    }
}
