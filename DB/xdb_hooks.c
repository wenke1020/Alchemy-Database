/*
 * This file implements ALCHEMY_DATABASE's redis-server hooks
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

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <strings.h>

//PROTOTYPES (annoying complier warnings) -- TODO include these :)
int inet_aton(const char *cp, struct in_addr *inp);

#include "xdb_hooks.h"

#include "dict.h"
#include "adlist.h"
#include "redis.h"

#include "hiredis.h"

#include "webserver.h"
#include "messaging.h"
#include "internal_commands.h"
#include "ctable.h"
#include "sixbit.h"
#include "luatrigger.h"
#include "aof_alsosql.h"
#include "rdb_alsosql.h"
#include "desc.h"
#include "range.h"
#include "ddl.h"
#include "index.h"
#include "find.h"
#include "alsosql.h"

extern int       Num_tbls; extern r_tbl_t *Tbl;
extern int       Num_indx; extern r_ind_t *Index;


extern char     *LuaCronFunc;
extern ulong     Operations;

extern uchar     OutputMode;
extern dictType  sdsDictType;
extern dictType  dbDictType;

extern long      CurrCard;
extern long      CurrUpdated;
extern robj     *CurrError;

// GLOBALS
cli            *CurrClient         = NULL;

char           *Basedir            = "./"; //TODO redundant w/ "dir"
char           *LuaIncludeFile     = NULL; //TODO is LuaIncludeFile needed?

// internal.lua should be in the CWD
#define LUA_INTERNAL_FILE "internal.lua"

/* WEBSERVERMODE variables */
int             WebServerMode      = -1;
char           *WebServerIndexFunc = NULL;

/* WHITELISTED IPS for WebServerMode */
struct in_addr  WS_WL_Addr;
struct in_addr  WS_WL_Mask;
unsigned int    WS_WL_Broadcast = 0;
unsigned int    WS_WL_Subnet    = 0;

// SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF
bool SQL_AOF       = 0;
bool SQL_AOF_MYSQL = 0;

// RDB Load Table & Index Highwaters
uint32  Tbl_HW = 0; list   *DropT = NULL; dict   *TblD;
uint32  Ind_HW = 0; list   *DropI = NULL; dict   *IndD;
                                          dict   *StmtD; dict *DynLuaD;

/* PROTOTYPES */
int  yesnotoi(char *s);
int *getKeysUsingCommandTable(rcommand *cmd, robj **argv, int argc, int *nkeys);
// from scripting.c
void luaReplyToRedisReply(redisClient *c, lua_State *lua);
// from rdb.c
int       rdbSaveType(FILE *fp, unsigned char type);
int       rdbSaveLen(FILE *fp, uint32_t len);
int       rdbLoadType(FILE *fp);
uint32_t  rdbLoadLen(FILE *fp, int *isencoded);

// from /DB/*.c
void showCommand     (redisClient *c);

void createCommand   (redisClient *c);
void dropCommand     (redisClient *c);
void descCommand     (redisClient *c);
void alterCommand    (redisClient *c);
void sqlDumpCommand  (redisClient *c);

void insertCommand   (redisClient *c);
void replaceCommand  (redisClient *c);
void sqlSelectCommand(redisClient *c);
void updateCommand   (redisClient *c);
void deleteCommand   (redisClient *c);
void tscanCommand    (redisClient *c);

void luafuncCommand  (redisClient *c);

void explainCommand  (redisClient *c);
void prepareCommand  (redisClient *c);
void executeCommand  (redisClient *c);

#ifdef CLIENT_BTREE_DEBUG
void btreeCommand     (redisClient *c);
void validateBTommand (redisClient *c);
#endif

void messageCommand  (redisClient *c);

void purgeCommand    (redisClient *c);
void dirtyCommand    (redisClient *c);

void evictCommand     (redisClient *c);

#ifdef REDIS3
  #define CMD_END       NULL,1,1,1,0,0
  #define GLOB_FUNC_END NULL,0,0,0,0,0
#else
  #define CMD_END NULL,0,0,0
#endif

struct redisCommand DXDBCommandTable[] = {
    // SELECT
    {"select",     sqlSelectCommand,  -2, 0,                 CMD_END},
    {"scan",       tscanCommand,      -4, 0,                 CMD_END},
    // CRUD
    {"insert",     insertCommand,     -5, REDIS_CMD_DENYOOM, CMD_END},
    {"update",     updateCommand,      6, REDIS_CMD_DENYOOM, CMD_END},
    {"delete",     deleteCommand,      5, 0,                 CMD_END},
    {"replace",    replaceCommand,    -5, REDIS_CMD_DENYOOM, CMD_END},
    // EVICT
    {"evict",      evictCommand,      -3, 0,                 GLOB_FUNC_END},
    // DDL
    {"create",     createCommand,     -4, REDIS_CMD_DENYOOM, GLOB_FUNC_END},
    {"drop",       dropCommand,        3, 0,                 GLOB_FUNC_END},
    {"desc",       descCommand,        2, 0,                 GLOB_FUNC_END},
    {"dump",       sqlDumpCommand,    -2, 0,                 GLOB_FUNC_END},
    // MODIFICATION RDBMS
    {"alter",      alterCommand,      -5, 0,                 GLOB_FUNC_END},
    // NOSQL(advanced)
    {"lua",        luafuncCommand,    -2, 0,                 GLOB_FUNC_END},
    {"message",    messageCommand,    -2, 0,                 GLOB_FUNC_END},
    // FAILOVER
    {"purge",      purgeCommand,       1, 0,                 GLOB_FUNC_END},
    {"dirty",      dirtyCommand,      -1, 0,                 GLOB_FUNC_END},
    // PROFILE/DEBUG
    {"explain",    explainCommand,    -6, 0,                 GLOB_FUNC_END},
    {"show",       showCommand,        2, 0,                 GLOB_FUNC_END},
#ifdef CLIENT_BTREE_DEBUG
    {"btree",      btreeCommand,      -2, 0,                 GLOB_FUNC_END},
    {"vbtree",     validateBTommand,  -2, 0,                 GLOB_FUNC_END},
#endif
    // PREPARED_STATEMENTs
    {"prepare",    prepareCommand,     9, 0,                 GLOB_FUNC_END},
    {"execute",    executeCommand,    -2, 0,                 GLOB_FUNC_END},
};


int *DXDB_getKeysFromCommand(rcommand *cmd, robj **argv, int argc, int *numkeys,
                             int flags,     sds *override_key, bool *err) {
    //printf("DXDB_getKeysFromCommand\n");
    if (cmd->proc == sqlSelectCommand || cmd->proc == insertCommand    || 
        cmd->proc == updateCommand    || cmd->proc == deleteCommand    || 
        cmd->proc == replaceCommand   || cmd->proc == tscanCommand) {
            *numkeys      = 0;
            *override_key = override_getKeysFromComm(cmd, argv, argc, err);
            return NULL;
    }
    if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd, argv, argc, numkeys, flags);
    } else {
        return getKeysUsingCommandTable(cmd, argv, argc, numkeys);
    }
}

void DXDB_populateCommandTable(dict *server_commands) {
    //printf("addDXDBfunctions: %p\n", server_commands);
    int numcommands = sizeof(DXDBCommandTable)/sizeof(struct redisCommand);
    for (int j = 0; j < numcommands; j++) {
        struct redisCommand *c = DXDBCommandTable + j;
        int retval = dictAdd(server_commands, sdsnew(c->name), c);
        assert(retval == DICT_OK);
    }
}

void DXDB_initServerConfig() { //printf("DXDB_initServerConfig\n");
    LuaIncludeFile     = NULL;
    LuaCronFunc        = NULL;
    Basedir            = zstrdup("./extra/"); // DEFAULT dir for Alchemy
    WebServerMode      = -1;
    WebServerIndexFunc = NULL;
    WS_WL_Broadcast    =  0;
    WS_WL_Subnet       =  0;
    bzero(&WS_WL_Addr, sizeof(struct in_addr));
    bzero(&WS_WL_Mask, sizeof(struct in_addr));
}

static void init_DXDB_PersistentStorageItems(uint32 ntbl, uint32 nindx) {
    if (Tbl) free(Tbl);
    Num_tbls = 0;
    Tbl      = malloc(sizeof(r_tbl_t) * ntbl);
    bzero(Tbl,        sizeof(r_tbl_t) * ntbl);
    Tbl_HW   = ntbl;
    if (TblD) dictRelease(TblD);
    TblD     = dictCreate(&sdsDictType, NULL);
    if (DropT) { listRelease(DropT); DropT = NULL; }

    if (Index) free(Index);
    Num_indx = 0;
    Index    = malloc(sizeof(r_ind_t) * nindx);
    bzero(Index,      sizeof(r_ind_t) * nindx);
    Ind_HW   = nindx;
    if (IndD) dictRelease(IndD);
    IndD     = dictCreate(&sdsDictType, NULL);
    if (DropI) { listRelease(DropI); DropI = NULL; }

    if (StmtD) dictRelease(StmtD);
    StmtD    = dictCreate(&dbDictType,  NULL);
}
static void initServer_Extra() {
    if (DynLuaD) dictRelease(DynLuaD);
    DynLuaD  = dictCreate(&dbDictType,  NULL);
}

void DXDB_initServer() { //printf("DXDB_initServer\n");
    server.stat_num_dirty_commands = 0;
    aeCreateTimeEvent(server.el, 1, luaCronTimeProc, NULL, NULL);
    initX_DB_Range(); initAccessCommands(); init_six_bit_strings();
    init_DXDB_PersistentStorageItems(INIT_MAX_NUM_TABLES, INIT_MAX_NUM_INDICES);
    initServer_Extra();
    CurrClient     = NULL;
    Operations     = 0;
}

static bool loadLuaHelperFile(cli *c, char *fname) {
    sds  fwpath = sdscatprintf(sdsempty(), "%s%s", Basedir, fname);
    bool ret    = 1;
    //printf("loadLuaHelperFile: %s\n", fwpath);
    if (luaL_loadfile(server.lua, fwpath) || lua_pcall(server.lua, 0, 0, 0)) {
        const char *lerr = lua_tostring(server.lua, -1);
        if (c) addReplySds(c, sdscatprintf(sdsempty(),
                           "-ERR luaL_loadfile: %s err: %s\r\n", fwpath, lerr));
        else fprintf(stderr, "loadLuaHelperFile: %s err: %s\r\n", fwpath, lerr);
        ret = 0;
    }
    CLEAR_LUA_STACK sdsfree(fwpath); return ret;
}
static bool initLua(cli *c) {
    lua_pushcfunction(server.lua, luaSetHttpResponseHeaderCommand);
    lua_setglobal(server.lua, "SetHttpResponseHeader");
    lua_pushcfunction(server.lua, luaSetHttpRedirectCommand);
    lua_setglobal(server.lua, "SetHttpRedirect");
    lua_pushcfunction(server.lua, luaSetHttp304Command);
    lua_setglobal(server.lua, "SetHttp304");

    lua_pushcfunction(server.lua, luaConvertToRedisProtocolCommand);
    lua_setglobal(server.lua, "Redisify");
    lua_pushcfunction(server.lua, luaSha1Command);
    lua_setglobal(server.lua, "SHA1");
    lua_pushcfunction(server.lua, luaSQLCommand);
    lua_setglobal(server.lua, "SQL");
    lua_pushcfunction(server.lua, luaIsConnectedToMaster);
    lua_setglobal(server.lua, "IsConnectedToMaster");

    lua_pushcfunction(server.lua, luaRemoteMessageCommand);
    lua_setglobal(server.lua, "RemoteMessage");
    lua_pushcfunction(server.lua, luaRemotePipeCommand);
    lua_setglobal(server.lua, "RemotePipe");

    lua_pushcfunction(server.lua, luaSubscribeFDCommand);
    lua_setglobal(server.lua, "SubscribeFD");
    lua_pushcfunction(server.lua, luaGetFDForChannelCommand);
    lua_setglobal(server.lua, "GetFDForChannel");
    lua_pushcfunction(server.lua, luaUnsubscribeFDCommand);
    lua_setglobal(server.lua, "UnsubscribeFD");
    lua_pushcfunction(server.lua, luaCloseFDCommand);
    lua_setglobal(server.lua, "CloseFD");

    // LUA_INDEX_CALLBACKS
    lua_pushcfunction(server.lua, luaAlchemySetIndex);
    lua_setglobal(server.lua, "alchemySetIndex");
    lua_pushcfunction(server.lua, luaAlchemyUpdateIndex);
    lua_setglobal(server.lua, "alchemyUpdateIndex");
    lua_pushcfunction(server.lua, luaAlchemyDeleteIndex);
    lua_setglobal(server.lua, "alchemyDeleteIndex");

    if                    (!loadLuaHelperFile(c, LUA_INTERNAL_FILE)) return 0;
    if (LuaIncludeFile  && !loadLuaHelperFile(c, LuaIncludeFile))    return 0;
    else                                                             return 1;
}
static bool reloadLua(cli *c) {
    lua_close(server.lua); scriptingInit(); return initLua(c);
}
void DXDB_main() { //NOTE: must come after rdbLoad()
    if (!initLua(NULL)) exit(-1);
}

void DXDB_emptyDb() { //printf("DXDB_emptyDb\n");
    for (int k = 0; k < Num_tbls; k++) emptyTable(k); /* deletes indices also */
    init_DXDB_PersistentStorageItems(INIT_MAX_NUM_TABLES, INIT_MAX_NUM_INDICES);
}

bool isWhiteListedIp(cli *c) {
    if (WS_WL_Broadcast) { // check WHITELISTED IPs
        unsigned int saddr     = c->sa.sin_addr.s_addr;
        unsigned int b_masked  = saddr | WS_WL_Broadcast;
        unsigned int sn_masked = saddr & WS_WL_Subnet;
        if (b_masked  == WS_WL_Broadcast &&
            sn_masked == WS_WL_Subnet) return 1;
    }
    return 0;
}
rcommand *DXDB_lookupCommand(sds name) {
    struct redisCommand *cmd = dictFetchValue(server.commands, name);
    if (WebServerMode > 0) {
        if (!CurrClient) return cmd; // called during load in whitelist
        if (CurrClient == server.master) return cmd; // feed from master
        if (CurrClient == server.lua_client) return cmd; // lua already internal
        if (!CurrClient->InternalRequest) {
            if (isWhiteListedIp(CurrClient)) return cmd;
            return cmd ? (cmd->proc == luafuncCommand) ? cmd : NULL : NULL;
        }
    }
    return cmd;
}

void DXDB_call(struct redisCommand *cmd, long long *dirty) {
    if (cmd->proc == luafuncCommand || cmd->proc == messageCommand) *dirty = 0;
    if (*dirty) server.stat_num_dirty_commands++;
}

static void computeWS_WL_MinMax() {
    if (!WS_WL_Broadcast && WS_WL_Mask.s_addr && WS_WL_Addr.s_addr) {
        WS_WL_Subnet    = WS_WL_Addr.s_addr & WS_WL_Mask.s_addr;
        WS_WL_Broadcast = WS_WL_Addr.s_addr | ~WS_WL_Mask.s_addr;
    }
}
int DXDB_loadServerConfig(int argc, sds *argv) {
    //printf("DXDB_loadServerConfig: 0: %s\n", argv[0]);
    if        (!strcasecmp(argv[0], "include_lua")   && argc == 2) {
        if (LuaIncludeFile) zfree(LuaIncludeFile);
        LuaIncludeFile = zstrdup(argv[1]);
        return 0;
    } else if (!strcasecmp(argv[0], "luacronfunc")   && argc == 2) {
        if (LuaCronFunc) zfree(LuaCronFunc);
        LuaCronFunc = zstrdup(argv[1]);
        return 0;
    } else if (!strcasecmp(argv[0], "basedir")       && argc == 2) {
        if (Basedir) zfree(Basedir);
        Basedir = zstrdup(argv[1]);
        return 0;
    } else if (!strcasecmp(argv[0], "outputmode")    && argc == 2) {
        if        (!strcasecmp(argv[1], "embedded")) {
            OutputMode = OUTPUT_EMBEDDED;
        } else if (!strcasecmp(argv[1], "pure_redis")) {
            OutputMode = OUTPUT_PURE_REDIS;
        } else if (!strcasecmp(argv[1],"normal")) {
            OutputMode = OUTPUT_NORMAL;
        } else {
            char *err = "argument must be 'embedded', 'pure_redis' or 'normal'";
            fprintf(stderr, "%s\n", err);
            return -1;
        }
        return 0;
    } else if (!strcasecmp(argv[0], "webserver_mode") && argc == 2) {
        if ((WebServerMode = yesnotoi(argv[1])) == -1) {
            char *err = "argument must be 'yes' or 'no'";
            fprintf(stderr, "%s\n", err);
            return -1;
        }
        return 0;
    } else if (!strcasecmp(argv[0], "webserver_index_function") && argc == 2) {
        if (WebServerIndexFunc) zfree(WebServerIndexFunc);
        WebServerIndexFunc = zstrdup(argv[1]);
        return 0;
    } else if (!strcasecmp(argv[0], "webserver_whitelist_address") &&
                argc == 2) {
        if (!inet_aton(argv[1], &WS_WL_Addr)) {
            fprintf(stderr, "ERR: webserver_whitelist_address: %s\n", argv[1]);
            return -1;
        }
        computeWS_WL_MinMax();
        return 0;
    } else if (!strcasecmp(argv[0], "webserver_whitelist_netmask") &&
                argc == 2) {
        if (!inet_aton(argv[1], &WS_WL_Mask)) {
            fprintf(stderr, "ERR: webserver_whitelist_netmask: %s\n", argv[1]);
            return -1;
        }
        computeWS_WL_MinMax();
        return 0;
    } else if (!strcasecmp(argv[0],"sqlappendonly") && argc == 2) {
        if        (!strcasecmp(argv[1], "no")) {
            server.appendonly = 0;
        } else if (!strcasecmp(argv[1], "yes")) {
            server.appendonly = 1;
        } else if (!strcasecmp(argv[1], "mysql")) {
            server.appendonly = 1;
            SQL_AOF_MYSQL = 1;
        } else {
            fprintf(stderr, "argument must be 'yes', 'no' or 'mysql;\n");
            return -1;
        }
        SQL_AOF = 1;
        return 0;
    }
    return 1;
}

void initClient(redisClient *c) {       //printf("initClient\n");
    c->Explain         =  0;
    c->Prepare         =  NULL;
    c->LruColInSelect  =  0;
    c->LfuColInSelect  =  0;
    c->InternalRequest =  0;
    bzero(&c->http, sizeof(alchemy_http_info));
    c->http.retcode    =  200; // DEFAULT to "HTTP 200 OK"
    c->LastJTAmatch    = -1;
    c->NumJTAlias      =  0;
    c->bindaddr        =  NULL;
    c->bindport        =  0;
}
void DXDB_createClient(int fd, redisClient *c) {//printf("DXDB_createClient\n");
    initClient(c);
    c->scb             =  NULL;
    if (fd == -1) c->InternalRequest = 1;
}

int   DXDB_processCommand(redisClient *c) { //printf("DXDB_processCommand\n");
    if (c->http.mode == HTTP_MODE_ON) return continue_http_session(c);
    Operations++;
    CurrClient  = c;
    CurrCard    = 0;
    CurrUpdated = 0;
    CurrError   = NULL;
    initClient(c);
    sds arg0       = c->argv[0]->ptr;
    sds arg2       = c->argc > 2 ? c->argv[2]->ptr : NULL;
    if (c->argc == 3 /* FIRST LINE OF HTTP REQUEST */                     &&
        (!strcasecmp(arg0, "GET")      || !strcasecmp(arg0, "POST") ||
         !strcasecmp(arg0, "HEAD"))                                       &&
        (!strcasecmp(arg2, "HTTP/1.0") || !strcasecmp(arg2, "HTTP/1.1")))
        return start_http_session(c);
    else return 0;
}

bool DXDB_processInputBuffer_begin(redisClient *c) {// NOTE: used for POST BODY
    if (c->http.post && c->http.req_clen && c->http.mode == HTTP_MODE_POSTBODY){
        c->http.post_body = c->querybuf;
        c->querybuf       = sdsempty();
        c->http.mode      = HTTP_MODE_ON;
        end_http_session(c);
        c->http.mode      = HTTP_MODE_OFF;
        return 1;
    }
    return 0;
}
void  DXDB_processInputBuffer_ZeroArgs(redisClient *c) {//HTTP Request End-Delim
    if (c->http.mode == HTTP_MODE_ON) {
        if (c->http.post && c->http.req_clen) c->http.mode = HTTP_MODE_POSTBODY;
        else { 
            end_http_session(c);
            c->http.mode = HTTP_MODE_OFF;
        }
    } else c->http.mode = HTTP_MODE_OFF;
    return;
}

//TODO webserver_mode, webserver_whitelist_address, webserver_whitelist_netmask, webserver_index_function, sqlslaveof
int DXDB_configSetCommand(cli *c, robj *o) {
    if (!strcasecmp(c->argv[2]->ptr, "include_lua")) {
        if (LuaIncludeFile) zfree(LuaIncludeFile);
        LuaIncludeFile = zstrdup(o->ptr);
        if (!reloadLua(c)) {
            addReplySds(c,sdscatprintf(sdsempty(),
               "-ERR problem loading lua helper file: %s\r\n", (char *)o->ptr));
            decrRefCount(o);
            return -1;
        }
        return 0;
    } else if (!strcasecmp(c->argv[2]->ptr,"luacronfunc")) {
        zfree(LuaCronFunc);
        LuaCronFunc = zstrdup(o->ptr);
        return 0;
    } else if (!strcasecmp(c->argv[2]->ptr, "outputmode")) {
        if        (!strcasecmp(o->ptr, "embedded")) {
            OutputMode = OUTPUT_EMBEDDED;
        } else if (!strcasecmp(o->ptr, "pure_redis")) {
            OutputMode = OUTPUT_PURE_REDIS;
        } else if (!strcasecmp(o->ptr, "normal")) {
            OutputMode = OUTPUT_NORMAL;
        } else {
            addReplySds(c,sdscatprintf(sdsempty(),
               "-ERR OUTPUTMODE: [EMBEDDED|PURE_REDIS|NORMAL] not: %s\r\n",
                (char *)o->ptr));
            decrRefCount(o);
            return -1;
        }
        return 0;
    }
    return 1;
}

static void configAddCommand(redisClient *c) {
    robj *o = getDecodedObject(c->argv[3]);
    if (!strcasecmp(c->argv[2]->ptr, "luafile")) {
        CLEAR_LUA_STACK
        if (!loadLuaHelperFile(c, o->ptr)) {
            addReplySds(c,sdscatprintf(sdsempty(),
               "-ERR problem adding LuaFile: (%s) msg: (%s)\r\n",
               (char *)o->ptr, lua_tostring(server.lua, -1)));
            CLEAR_LUA_STACK
            decrRefCount(o); return;
        }
        CLEAR_LUA_STACK
    } else if (!strcasecmp(c->argv[2]->ptr, "lua")) {
        CLEAR_LUA_STACK
        if (luaL_dostring(server.lua, o->ptr)) {
            lua_pop(server.lua, 1);
            addReplySds(c,sdscatprintf(sdsempty(),
               "-ERR problem adding lua: (%s) msg: (%s)\r\n",
               (char *)o->ptr, lua_tostring(server.lua, -1)));
            CLEAR_LUA_STACK
            decrRefCount(o); return;
        }
        CLEAR_LUA_STACK
    }
    decrRefCount(o);
    addReply(c,shared.ok);
}
unsigned char DXDB_configCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr, "ADD")) {
        if (c->argc != 4) return -1;
        configAddCommand(c); return 0;
    }
    return 1;
}
void DXDB_configGetCommand(redisClient *c, char *pattern, int *matches) {
    if (stringmatch(pattern, "include_lua", 0)) {
        addReplyBulkCString(c, "include_lua");
        addReplyBulkCString(c, LuaIncludeFile);
        *matches = *matches + 1;
    }
    if (stringmatch(pattern, "luacronfunc", 0)) {
        addReplyBulkCString(c, "luafronfunc");
        addReplyBulkCString(c, LuaCronFunc);
        *matches = *matches + 1;
    }
    if (stringmatch(pattern, "outputmode", 0)) {
        addReplyBulkCString(c, "outputmode");
        if      (EREDIS) addReplyBulkCString(c, "embedded");
        else if (OREDIS) addReplyBulkCString(c, "pure_redis");
        else             addReplyBulkCString(c, "normal");
        *matches = *matches + 1;
    }
}

int DXDB_rdbSave(FILE *fp) { //printf("DXDB_rdbSave\n");
    if (rdbSaveLen(fp, Num_tbls)                               == -1) return -1;
    if (rdbSaveLen(fp, Num_indx)                               == -1) return -1;
    for (int tmatch = 0; tmatch < Num_tbls; tmatch++) {
        r_tbl_t *rt = &Tbl[tmatch];
        if (!rt->name) continue; // respect deletion
        if (rdbSaveType        (fp, REDIS_BTREE)               == -1) return -1;
        if (rdbSaveBT          (fp, rt->btr)                   == -1) return -1;
        MATCH_PARTIAL_INDICES(tmatch)
        if (matches) {
            for (int i = 0; i < matches; i++) {
                r_ind_t *ri = &Index[inds[i]];
                if (ri->luat) {
                    if (rdbSaveType    (fp, REDIS_LUA_TRIGGER) == -1) return -1;
                    if (rdbSaveLuaTrigger(fp, ri)              == -1) return -1;
                } else {
                    if (rdbSaveType        (fp, REDIS_BTREE)   == -1) return -1;
                    if (rdbSaveBT          (fp, ri->btr)       == -1) return -1;
                }
            }
        }
    }
    if (rdbSaveType(fp, REDIS_EOF) == -1) return -1; /* SQL delim REDIS_EOF */
    return 0;
}

int DXDB_rdbLoad(FILE *fp) { //printf("DXDB_rdbLoad\n");
   uint32 ntbl, nindx;
   if ((ntbl  = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return -1;
   if ((nindx = rdbLoadLen(fp, NULL)) == REDIS_RDB_LENERR) return -1;
    init_DXDB_PersistentStorageItems(ntbl, nindx);
    while (1) {
        int type;
        if ((type = rdbLoadType(fp))  == -1)               return -1;
        if (type == REDIS_EOF)              break;    /* SQL delim REDIS_EOF */
        if        (type == REDIS_BTREE) {
            if (!rdbLoadBT(fp))                            return -1;
        } else if (type == REDIS_LUA_TRIGGER) {
            if (!rdbLoadLuaTrigger(fp))                    return -1;
        }
    }
    rdbLoadFinished(); // -> build Indexes
    return 0;
}

int DXDB_rewriteAppendOnlyFile(FILE *fp) {
    //printf("DXDB_rewriteAppendOnlyFile fp: %p\n", fp);
    for (int tmatch = 0; tmatch < Num_tbls; tmatch++) {
        r_tbl_t *rt = &Tbl[tmatch];
        if (!rt->btr) continue; /* virtual indices have NULLs */
        if (rt->btr->s.btype == BTREE_TABLE) { // Tables dump their indexes
            /* First dump table definition and ALL rows */
            if (!appendOnlyDumpTable(fp, rt->btr, tmatch)) return -1;
            /* then dump Table's Index definitions */
            if (!appendOnlyDumpIndices(fp, tmatch))        return -1;
        }
    }
    return 0;
}

void DXDB_flushdbCommand() {
    for (int tmatch = 0; tmatch < Num_tbls; tmatch++) emptyTable(tmatch);
    Num_tbls = Num_indx = 0;
}

void DBXD_genRedisInfoString(sds info) {
#ifdef REDIS3
    info = sdscat(info,"\r\n");
#endif
    info = sdscatprintf(info,
#ifdef REDIS3
            "# ALCHEMY\r\n"
#endif
            "luafilname:%s\r\n"
            "luacronfunc:%s\r\n"
            "basedir:%s\r\n"
            "outputmode:%s\r\n"
            "webserver_mode:%s\r\n"
            "webserver_index_function:%s\r\n",
             LuaIncludeFile, LuaCronFunc, Basedir,
             (EREDIS) ? "embedded" : ((OREDIS) ? "pure_redis" : "normal"),
             (WebServerMode == -1) ? "no" : "yes",
             WebServerIndexFunc);
}

extern struct sockaddr_in AcceptedClientSA;
void DXDB_setClientSA(redisClient *c) { c->sa = AcceptedClientSA; }

// LUA_COMMAND LUA_COMMAND LUA_COMMAND LUA_COMMAND LUA_COMMAND LUA_COMMAND
void luafuncCommand(redisClient *c) {
    if (luafunc_call(c, c->argc, c->argv)) return;
    luaReplyToRedisReply(c, server.lua);
}

// PURGE PURGE PURGE PURGE PURGE PURGE PURGE PURGE PURGE PURGE PURGE PURGE
void DXDB_syncCommand(redisClient *c) {
    sds ds = sdscatprintf(sdsempty(), "DIRTY %lld\r\n", 
                                       server.stat_num_dirty_commands);
    robj *r = createStringObject(ds, sdslen(ds));
    addReply(c, r); // SYNC DIRTY NUM
    decrRefCount(r);
    c->bindaddr = sdsdup(c->argv[1]->ptr);
    c->bindport = atoi(c->argv[2]->ptr);
}

static bool checkPurge() {
    listNode *ln; listIter li;
    uint32    num_ok = 0;
    sds       ds     = sdsnew("DIRTY\r\n"); //TODO does not need malloc'ing
    listRewind(server.slaves, &li);
    while((ln = listNext(&li))) {
        cli *slave = ln->value;
        redisReply *reply;
        int fd = remoteMessage(slave->bindaddr, slave->bindport, ds, 1, &reply);
        if (fd == -1) close(fd);
        if (reply) {
            assert(reply->type == REDIS_REPLY_INTEGER);
            if (reply->integer == server.stat_num_dirty_commands) num_ok++;
        }
    }
    sdsfree(ds);
    return (server.slaves->len == num_ok);
}
void purgeCommand(redisClient *c) {
    uint32 check_purge_usleep = 10000; // 10ms
    while (!checkPurge()) {
        redisLog(REDIS_WARNING, "Check PURGE on SLAVE failed, sleeping: %dus",
                                 check_purge_usleep);
        usleep(check_purge_usleep);
    }
    addReply(c, shared.ok);
    aeProcessEvents(server.el, AE_ALL_EVENTS);
    if (prepareForShutdown() == REDIS_OK) exit(0);
}
void dirtyCommand(redisClient *c) {
    if (c->argc != 1) { /* strtoul OK delimed by sds */
        server.stat_num_dirty_commands = strtoul(c->argv[1]->ptr, NULL, 10);
    }
    addReplyLongLong(c, server.stat_num_dirty_commands);
}

// SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF SQL_AOF
sds DXDB_SQL_feedAppendOnlyFile(rcommand *cmd, robj **argv, int argc) {
    if (cmd->proc == insertCommand || cmd->proc == updateCommand  ||
        cmd->proc == deleteCommand || cmd->proc == replaceCommand ||
        cmd->proc == createCommand || cmd->proc == dropCommand    ||
        cmd->proc == alterCommand) {
        if (cmd->proc == createCommand) {
            if ((!strcasecmp(argv[1]->ptr, "LRUINDEX"))   ||
                (!strcasecmp(argv[1]->ptr, "LUATRIGGER"))) return sdsempty();
            int arg = 1, targ = 4, coln = 5;
            if (!strcasecmp(argv[1]->ptr,   "UNIQUE")) {
                arg = 2; targ = 5; coln = 6;
            }
            if (!strcasecmp(argv[arg]->ptr, "INDEX")) { // index on TEXT
                int      tmatch = find_table(argv[targ]->ptr);
                r_tbl_t *rt     = &Tbl[tmatch];
                char    *token  = argv[coln]->ptr;
                char    *end    = strchr(token, ')');
                STACK_STRDUP(cname, (token + 1), (end - token - 1))
                icol_t   ic     = find_column(tmatch, cname);
                uchar    ctype  = rt->col[ic.cmatch].type;
                if (C_IS_S(ctype)) {
                    int      imatch = find_index(tmatch, ic);
                    r_ind_t *ri     = &Index[imatch];
                    return createAlterTableFulltext(rt, ri, ic.cmatch, 1);
                }
            }
        }
        sds buf = sdsempty();
        for (int j = 0; j < argc; j++) {
            robj *o = getDecodedObject(argv[j]);
            buf = sdscatlen(buf, o->ptr, sdslen(o->ptr));
            if (j != (argc - 1)) buf = sdscatlen(buf," ", 1);
            decrRefCount(o);
        }
        buf = sdscatlen(buf, ";\n", 2);
        return buf;
    }
    return sdsempty();
}
