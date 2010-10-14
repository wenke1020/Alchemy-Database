/*
 * Implements istore and iselect
 *

MIT License

Copyright (c) 2010 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "alsosql.h"
#include "redis.h"
#include "bt.h"
#include "bt_iterator.h"
#include "row.h"
#include "index.h"
#include "zmalloc.h"
#include "common.h"
#include "store.h"

// FROM redis.c
#define RL4 redisLog(4,
extern struct sharedObjectsStruct shared;
extern struct redisServer server;

// GLOBALS
extern int Num_tbls[MAX_NUM_DB];

extern char  CCOMMA;
extern char  CEQUALS;
extern char  CMINUS;
extern char *EQUALS;
extern char *COLON;
extern char *COMMA;
extern char *PERIOD;

extern char *Col_type_defs[];

extern r_tbl_t  Tbl   [MAX_NUM_DB][MAX_NUM_TABLES];
extern r_ind_t  Index [MAX_NUM_DB][MAX_NUM_INDICES];

#define MAX_TBL_DEF_SIZE     1024

stor_cmd StorageCommands[NUM_STORAGE_TYPES];

// STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS
// STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS STORE_COMMANDS

void legacyInsertCommand(redisClient *c) {
    TABLE_CHECK_OR_REPLY(c->argv[1]->ptr,)
    int ncols = Tbl[server.dbid][tmatch].col_count;
    MATCH_INDICES(tmatch)

    char *vals   = c->argv[2]->ptr;
    insertCommitReply(c, vals, ncols, tmatch, matches, indices);
}

void legacyTableCommand(redisClient *c) {
    if (Num_tbls[server.dbid] >= MAX_NUM_TABLES) {
        addReply(c,shared.toomanytables);
        return;
    }

    char *tname = c->argv[1]->ptr;
    if (find_table(tname) != -1) {
        addReply(c,shared.nonuniquetablenames);
        return;
    }

    // parse column definitions
    char  col_names[MAX_COLUMN_PER_TABLE][MAX_COLUMN_NAME_SIZE];
    char *cname     = c->argv[2]->ptr;
    int   col_count = 0;
    while (1) {
        char *type  = strchr(cname, CEQUALS);
        char *nextc = strchr(cname, CCOMMA);
        if (type) {
            *type = '\0';
            type++;
        } else {
            addReply(c,shared.missingcolumntype);
            return;
        }
        if (nextc) {
            *nextc = '\0';
            nextc++;
        }
        unsigned char miss = 1;
        for (unsigned char j = 0; j < NUM_COL_TYPES; j++) {
            if (!strcmp(type, Col_type_defs[j])) {
                Tbl[server.dbid][Num_tbls[server.dbid]].col_type[col_count] = j;
                miss = 0;
                break;
            }
        }
        if (miss) {
            addReply(c,shared.undefinedcolumntype);
            return;
        }
        if (strlen(cname) >= MAX_COLUMN_NAME_SIZE) {
            addReply(c,shared.columnnametoobig);
            return;
        }
        strcpy(col_names[col_count], cname);
        col_count++;
        if (!nextc) {
            break;
        } else if (col_count == MAX_COLUMN_PER_TABLE) {
            addReply(c,shared.toomanycolumns);
            return;
        }
        cname = nextc;
    }
    createTableCommitReply(c, col_names, col_count, tname);
}

unsigned char respOk(redisClient *c) {
    listNode *ln = listFirst(c->reply);
    robj     *o  = ln->value;
    char     *s  = o->ptr;
    if (!strcmp(s, shared.ok->ptr)) return 1;
    else                            return 0;
}

static unsigned char respNotErr(redisClient *c) {
    listNode *ln = listFirst(c->reply);
    robj     *o  = ln->value;
    char     *s  = o->ptr;
    if (!strncmp(s, "-ERR ", 5)) return 0;
    else                         return 1;
}

static void cpyColDef(char *cdefs,
                      int  *slot,
                      int   tmatch,
                      int   cmatch,
                      int   qcols,
                      int   loop,
                      bool  has_conflicts,
                      bool  cname_cflix[]) {
    robj *col = Tbl[server.dbid][tmatch].col_name[cmatch];
    if (has_conflicts && cname_cflix[loop]) { // prepend tbl_name
        robj *tbl = Tbl[server.dbid][tmatch].name;
        memcpy(cdefs + *slot, tbl->ptr, sdslen(tbl->ptr));  
        *slot        += sdslen(tbl->ptr);        // tblname
        memcpy(cdefs + *slot, PERIOD, 1);
        *slot = *slot + 1;
    }
    memcpy(cdefs + *slot, col->ptr, sdslen(col->ptr));  
    *slot        += sdslen(col->ptr);            // colname
    memcpy(cdefs + *slot, EQUALS, 1);
    *slot = *slot + 1;
    char *ctype   = Col_type_defs[Tbl[server.dbid][tmatch].col_type[cmatch]];
    int   ctlen   = strlen(ctype);               // [INT,STRING]
    memcpy(cdefs + *slot, ctype, ctlen);
    *slot        += ctlen;
    if (loop != (qcols - 1)) {
        memcpy(cdefs + *slot, COMMA, 1);
        *slot = *slot + 1;                       // ,
    }
}

static bool _internalCreateTable(redisClient *c,
                                 redisClient *fc,
                                 int          qcols,
                                 int          cmatchs[],
                                 int          tmatch,
                                 int          j_tbls[],
                                 int          j_cols[],
                                 bool         cname_cflix[]) {
    if (find_table(c->argv[2]->ptr) > 0) return 1;

    char cdefs[MAX_TBL_DEF_SIZE];
    int  slot  = 0;
    for (int i = 0; i < qcols; i++) {
        if (tmatch != -1) {
            cpyColDef(cdefs, &slot, tmatch, cmatchs[i], qcols, i, 
                      0, cname_cflix);
        } else {
            cpyColDef(cdefs, &slot, j_tbls[i], j_cols[i], qcols, i,
                      1, cname_cflix);
        }
    }
    fc->argc    = 3;
    fc->argv[2] = createStringObject(cdefs, slot);
    legacyTableCommand(fc);
    if (!respOk(fc)) {
        listNode *ln = listFirst(fc->reply);
        addReply(c, ln->value);
        return 0;
    }
    return 1;
}

bool internalCreateTable(redisClient *c,
                         redisClient *fc,
                         int          qcols,
                         int          cmatchs[],
                         int          tmatch) {
    int  idum[1];
    bool bdum[1];
    return _internalCreateTable(c, fc, qcols, cmatchs, tmatch,
                                idum, idum, bdum);
}

bool createTableFromJoin(redisClient *c,
                         redisClient *fc,
                         int          qcols,
                         int          j_tbls [],
                         int          j_cols[]) {
    bool cname_cflix[MAX_JOIN_INDXS];
    for (int i = 0; i < qcols; i++) {
        for (int j = 0; j < qcols; j++) {
            if (i == j) continue;
            if (!strcmp(Tbl[server.dbid][j_tbls[i]].col_name[j_cols[i]]->ptr,
                        Tbl[server.dbid][j_tbls[j]].col_name[j_cols[j]]->ptr)) {
                cname_cflix[i] = 1;
                break;
            } else {
                cname_cflix[i] = 0;
            }
        }
    }

    int idum[1];
    return _internalCreateTable(c, fc, qcols, idum, -1,
                                j_tbls, j_cols, cname_cflix);
}

bool performStoreCmdOrReply(redisClient *c, redisClient *fc, int sto) {
    (*StorageCommands[sto].func)(fc);
    if (!respNotErr(fc)) {
        listNode *ln = listFirst(fc->reply);
        addReply(c, ln->value);
        return 0;
    }
    return 1;
}

static bool istoreAction(redisClient *c,
                         redisClient *fc,
                         int          tmatch,
                         int          cmatchs[],
                         int          qcols,
                         int          sto,
                         robj        *pko,
                         robj        *row,
                         robj        *newname,
                         bool         sub_pk,
                         uint32       nargc) {
    aobj  cols[MAX_COLUMN_PER_TABLE];
    int   totlen = 0;
    for (int i = 0; i < qcols; i++) {
        cols[i]  = getColStr(row, cmatchs[i], pko, tmatch);
        totlen  += cols[i].len;
    }

    char *newrow = NULL;
    int   rowlen = 0;
    fc->argc     = qcols + 1;
    fc->argv[1]  = cloneRobj(newname); /* the NEW Stored Objects NAME */
    //argv[0] NOT NEEDED
    if (StorageCommands[sto].argc) { // not INSERT
        int    n    = 0;
        robj **argv = fc->argv;
        if (sub_pk) { /* overwrite pk=newname:cols[0] */
            argv[1]->ptr = sdscatlen(argv[1]->ptr, COLON,   1);
            argv[1]->ptr = sdscatlen(argv[1]->ptr, cols[0].s, cols[0].len);
            n++;
        }
        argv[2] = createStringObject(cols[n].s, cols[n].len);
        if (nargc > 1) {
            n++;
            argv[3]  = createStringObject(cols[n].s, cols[n].len);
        }
        for (int i = 0; i < qcols; i++) {
            if (cols[i].sixbit) free(cols[i].s);
        }
    } else {                         // INSERT
        //TODO this can be done simpler w/ sdscatlen()
        int len = totlen + qcols -1;
        if (len > rowlen) {
            rowlen = len;
            if (newrow) newrow = zrealloc(newrow, rowlen); /* sds */
            else        newrow = zmalloc(         rowlen); /* sds */
        }
        int slot = 0;
        for (int i = 0; i < qcols; i++) {
            memcpy(newrow + slot, cols[i].s, cols[i].len);
            slot += cols[i].len;
            if (i != (qcols - 1)) {
                memcpy(newrow + slot, COMMA, 1);
                slot++;
            }
            if (cols[i].sixbit) free(cols[i].s);
        }
        fc->argv[2] = createStringObject(newrow, len);
    }

    if (newrow) zfree(newrow);
    return performStoreCmdOrReply(c, fc, sto);
}

void istoreCommit(redisClient *c,
                  int          tmatch,
                  int          imatch,
                  char        *sto_type,
                  char        *col_list,
                  char        *range,
                  robj        *newname,
                  int          obc, 
                  bool         asc) {
    //RL4 "istoreCommit ORDER BY ignoring: obc: %d asc: %d", obc, asc);
    int sto;
    CHECK_STORE_TYPE_OR_REPLY(sto_type,sto,)
    int cmatchs[MAX_COLUMN_PER_TABLE];
    int qcols = parseColListOrReply(c, tmatch, col_list, cmatchs);
    if (!qcols) {
        addReply(c, shared.nullbulk);
        return;
    }
    bool sub_pk    = (StorageCommands[sto].argc < 0);
    int  nargc     = abs(StorageCommands[sto].argc);
    sds  last_argv = c->argv[c->argc - 1]->ptr;
    if (nargc) { /* if NOT INSERT check nargc */
        if (last_argv[sdslen(last_argv) -1] == '$') {
            sub_pk = 1;
            nargc++;
            last_argv[sdslen(last_argv) -1] = '\0';
            sdsupdatelen(last_argv);
        }
        if (nargc != qcols) {
            addReply(c, shared.storagenumargsmismatch);
            return;
        }
        if (sub_pk) nargc--;
    }
    RANGE_CHECK_OR_REPLY(range)
    robj *o = lookupKeyRead(c->db, Tbl[server.dbid][tmatch].name);

    robj               *argv[STORAGE_MAX_ARGC + 1];
    struct redisClient *fc = rsql_createFakeClient();
    fc->argv               = argv;
    if (!StorageCommands[sto].argc) { // create table first if needed
        fc->argv[1] = cloneRobj(newname);
        if (!internalCreateTable(c, fc, qcols, cmatchs, tmatch)) {
            freeFakeClient(fc);
            decrRefCount(low);
            decrRefCount(high);
            return;
        }
    }

    btEntry            *be, *nbe;
    unsigned int        stored = 0;
    robj               *ind    = Index[server.dbid][imatch].obj;
    bool                virt   = Index[server.dbid][imatch].virt;
    robj               *bt     = virt ? o : lookupKey(c->db, ind);
    btStreamIterator   *bi     = btGetRangeIterator(bt, low, high, virt);
    while ((be = btRangeNext(bi, 1)) != NULL) {                // iterate btree
        if (virt) {
            robj *pko = be->key;
            robj *row = be->val;
            if (!istoreAction(c, fc, tmatch, cmatchs, qcols, sto,
                              pko, row, newname, sub_pk, nargc))
                goto istore_err;
            stored++;
        } else {
            robj             *val = be->val;
            btStreamIterator *nbi = btGetFullRangeIterator(val, 0, 0);
            while ((nbe = btRangeNext(nbi, 1)) != NULL) {     // iterate NodeBT
                robj *pko = nbe->key;
                robj *row = btFindVal(o, pko,
                                      Tbl[server.dbid][tmatch].col_type[0]);
                if (!istoreAction(c, fc, tmatch, cmatchs, qcols, sto,
                                  pko, row, newname, sub_pk, nargc)) {
                    btReleaseRangeIterator(nbi);
                    goto istore_err;
                }
                stored++;
            }
            btReleaseRangeIterator(nbi);
        }
    }

    if (sub_pk) { /* write back in "$" for AOF and Slaves */
        sds l_argv = sdscatprintf(sdsempty(), "%s$",
                                             (char *)c->argv[c->argc - 1]->ptr);
        sdsfree(c->argv[c->argc - 1]->ptr);
        c->argv[c->argc - 1]->ptr = l_argv;
    }
istore_err:
    btReleaseRangeIterator(bi);
    decrRefCount(low);
    decrRefCount(high);
    freeFakeClient(fc);

    addReplyLongLong(c, stored);
}

#if 0
void istoreCommand(redisClient *c) {
    int imatch = checkIndexedColumnOrReply(c, c->argv[3]->ptr);
    if (imatch == -1) return;
    int tmatch = Index[server.dbid][imatch].table;
    istoreCommit(c, tmatch, imatch, c->argv[1]->ptr, c->argv[5]->ptr,
                 c->argv[4]->ptr, c->argv[2]);
}
#endif
