/*
 * This file implements the basic SQL commands of Alsosql (single row ops)
 *  and calls the range-query and join ops
 *

MIT License

Copyright (c) 2010 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __ALSOSQL__H
#define __ALSOSQL__H

#include "redis.h"
#include "common.h"

robj *cloneRobj(robj *r);

int find_table(char *tname);
int find_column(int tmatch, char *column);

bool cCpyOrReply(redisClient *c, char *src, char *dest, unsigned int len);
void createTable(redisClient *c);

/* TABLE_CHECK_OR_REPLY(char *cargv1ptr) -
     creates (int tmatch) */
#define TABLE_CHECK_OR_REPLY(cargv1ptr, retval)   \
    int   tmatch = find_table(cargv1ptr);         \
    if (tmatch == -1) {                           \
        addReply(c, shared.nonexistenttable);     \
        return retval;                            \
    }

/* COLUMN_CHECK_OR_REPLY(char *cargv2ptr) -
     creates (char *cname, int cmatch)    */
#define COLUMN_CHECK_OR_REPLY(cargv2ptr, retval)   \
    char *cname  = cargv2ptr;                      \
    int   cmatch = find_column(tmatch, cname);     \
    if (cmatch == -1) {                            \
        addReply(c,shared.nonexistentcolumn);      \
        return retval;                             \
    }

int parseColListOrReply(redisClient   *c,
                        int            tmatch,
                        char          *clist,
                        int            cmatchs[]);

#define ASSIGN_UPDATE_HITS_AND_MISSES               \
    unsigned char  cmiss[MAX_COLUMN_PER_TABLE];     \
    char          *vals [MAX_COLUMN_PER_TABLE];     \
    unsigned int   vlens[MAX_COLUMN_PER_TABLE];     \
    for (int i = 0; i < ncols; i++) {               \
        unsigned char miss = 1;                     \
        for (int j = 0; j < qcols; j++) {           \
            if (i == cmatchs[j]) {                  \
                miss     = 0;                       \
                vals[i]  = mvals[j];                \
                vlens[i] = mvlens[j];               \
                break;                              \
            }                                       \
        }                                           \
        cmiss[i] = miss;                            \
    }

void selectReply(redisClient  *c,
                 robj         *o,    
                 robj         *pko,  
                 int           tmatch,
                 int           cmatchs[],
                 int           qcols);

void parseSelectColumnList(redisClient *c, sds *clist, int *argn);

int parseUpdateOrReply(redisClient  *c,
                       int           tmatch,
                       char         *cname,
                       int           cmatchs[],
                       char         *vals   [],
                       unsigned int  vlens  []);

#define LEN_OBJ                                               \
    unsigned long  card   = 0;                                \
    robj          *lenobj = createObject(REDIS_STRING, NULL); \
    addReply(c, lenobj);                                      \
    decrRefCount(lenobj);

#define EMPTY_LEN_OBJ             \
    unsigned long  card   = 0;    \
    robj          *lenobj = NULL;

#define INIT_LEN_OBJ                           \
    lenobj = createObject(REDIS_STRING, NULL); \
    addReply(c, lenobj);                       \
    decrRefCount(lenobj);

void createTableCommitReply(redisClient *c,
                            char         col_names[][MAX_COLUMN_NAME_SIZE],
                            int          col_count,
                            char        *tname);
void insertCommitReply(redisClient *c, 
                       sds          vals,
                       int          ncols,
                       int          tmatch,
                       int          matches,
                       int          indices[]);

#endif /*__ALSOSQL__H */ 