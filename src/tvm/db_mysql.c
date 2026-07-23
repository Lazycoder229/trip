#include "tvm.h"
#include <mysql.h>
#include <pthread.h>

// ── MySQL / MariaDB builtins ───────────────────────────────────────────
// Extended driver adding:
//   • Connection pooling      (mysqlPoolCreate / mysqlPoolGet / mysqlPoolRelease / mysqlPoolClose)
//   • Multi-result queries    (mysqlQueryMulti)
//   • Batch INSERT            (mysqlInsertBatch)
//   • Transactions            (mysqlBegin / mysqlCommit / mysqlRollback)
//   • Prepared statements     (mysqlPrepare / mysqlExecute / mysqlStmtClose)
//   • Ping / reconnect check  (mysqlPing)
//   • Affected rows           (mysqlAffectedRows)
//   • Row count               (mysqlNumRows)
//
// Build: same flags as before —
//   pkg-config --cflags --libs libmariadb   (or mysqlclient)
//   add -lpthread

// ── Called from object.c freeObject() ─────────────────────────────────
void tripMysqlCloseHandle(void* conn) {
    if (conn) mysql_close((MYSQL*)conn);
}


// ── Internal helpers ───────────────────────────────────────────────────

static Value mysqlFieldToValue(const char* data, unsigned long len,
                               enum enum_field_types type) {
    if (data == NULL) return NIL_VAL;
    switch (type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            char buf[128];
            unsigned long n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
            memcpy(buf, data, n);
            buf[n] = '\0';
            return NUMBER_VAL(strtod(buf, NULL));
        }
        default: {
            ObjString* s = copyString(data, (int)len);
            return (Value){VAL_OBJ, {.obj = (Obj*)s}};
        }
    }
}

// Consume a single result set into an ObjList* of ObjDict* rows.
// Returns NULL and sets *errOut on failure.
static ObjList* consumeResultSet(MYSQL* conn, const char** errOut) {
    MYSQL_RES* result = mysql_store_result(conn);
    if (result == NULL) {
        if (mysql_field_count(conn) != 0) {
            *errOut = mysql_error(conn);
            return NULL;
        }
        // Non-SELECT: return empty list (caller checks affected rows separately)
        return newList();
    }

    unsigned int numFields = mysql_num_fields(result);
    MYSQL_FIELD*  fields   = mysql_fetch_fields(result);

    ObjList* rows = newList();
    push((Value){VAL_OBJ, {.obj = (Obj*)rows}}); // GC guard

    MYSQL_ROW row;
    unsigned long* lengths;
    while ((row = mysql_fetch_row(result)) != NULL) {
        lengths = mysql_fetch_lengths(result);
        ObjDict* rowDict = newDict();
        push((Value){VAL_OBJ, {.obj = (Obj*)rowDict}}); // GC guard
        for (unsigned int i = 0; i < numFields; i++) {
            ObjString* key = copyString(fields[i].name,
                                        (int)strlen(fields[i].name));
            push((Value){VAL_OBJ, {.obj = (Obj*)key}}); // GC guard
            Value val = mysqlFieldToValue(row[i], lengths[i], fields[i].type);
            push(val); // GC guard
            dictSet(rowDict, key, val);
            pop(); pop(); // val, key
        }
        listAppend(rows, (Value){VAL_OBJ, {.obj = (Obj*)rowDict}});
        pop(); // rowDict
    }
    mysql_free_result(result);
    pop(); // rows guard
    return rows;
}

// ── Connection Pool ────────────────────────────────────────────────────
// A pool is a plain C struct kept alive via a void* stored inside an
// ObjDBConn whose `conn` field is NULL and whose `pool` flag is true.
// We use the existing ObjDBConn object as a pool handle; the script
// passes it to mysqlPoolGet() which returns a real connection.

#define POOL_MAX_SIZE 64

typedef struct {
    pthread_mutex_t lock;
    // Config (copied at creation time)
    char host[256];
    char user[256];
    char pass[256];
    char db[256];
    int  port;
    // Pool state
    MYSQL* conns[POOL_MAX_SIZE];  // idle connections
    bool   inUse[POOL_MAX_SIZE];  // true when checked out
    int    size;                  // total slots created so far
    int    maxSize;
    bool   closed;
} MysqlPool;

// Called by freeObject() for stmt handles the script never closed.
void tripMysqlCloseStmt(void* stmt) {
    if (stmt) mysql_stmt_close((MYSQL_STMT*)stmt);
}

// Called by freeObject() for pool handles the script never closed.
// MysqlPool is now declared above, so the cast is valid here.
void tripMysqlFreePool(void* pool) {
    if (!pool) return;
    MysqlPool* p = (MysqlPool*)pool;
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < p->size; i++) {
        if (p->conns[i]) { mysql_close(p->conns[i]); p->conns[i] = NULL; }
    }
    p->closed = true;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

// Pool handle is smuggled via a second ObjDBConn type: conn==NULL, pool!=NULL.
// We extend ObjDBConn's void* to carry either a MYSQL* or a MysqlPool*.
// We distinguish them with the `isPool` flag we add to ObjDBConn.
// ⚠ REQUIRES adding to ObjDBConn in object.h (see chunk.h patch below):
//     bool  isPool;
//     void* pool;   // MysqlPool* when isPool == true

static MYSQL* poolOpenConn(MysqlPool* p, char* errOut, size_t errLen) {
    MYSQL* c = mysql_init(NULL);
    if (!c) { snprintf(errOut, errLen, "mysql_init failed"); return NULL; }
    char reconnect = 1;
    mysql_options(c, MYSQL_OPT_RECONNECT, &reconnect);
    if (!mysql_real_connect(c, p->host, p->user, p->pass, p->db,
                             (unsigned int)p->port, NULL, CLIENT_MULTI_STATEMENTS)) {
        snprintf(errOut, errLen, "%s", mysql_error(c));
        mysql_close(c);
        return NULL;
    }
    return c;
}

// ── Prepared-statement handle ──────────────────────────────────────────
// We reuse ObjDBConn with isPool=false, conn=NULL, but add a `stmt` field.
// ⚠ REQUIRES adding to ObjDBConn in object.h:
//     MYSQL_STMT* stmt;  // non-NULL when this is a prepared-stmt handle

// ── Main dispatch ──────────────────────────────────────────────────────

InterpretResult callBuiltinMysql(uint8_t id, uint8_t argc) {
    switch (id) {

        // ── Original builtins (unchanged behaviour) ──────────────────

        case BUILTIN_MYSQL_CONNECT: {
            if (argc < 4 || argc > 5)
                return raiseError("mysqlConnect() expects 4-5 arguments");
            int port = 3306;
            if (argc == 5) {
                Value pv = pop();
                if (!IS_NUMBER(pv)) return raiseError("mysqlConnect() port must be a number");
                port = (int)AS_NUMBER(pv);
            }
            Value dbV = pop(), passV = pop(), userV = pop(), hostV = pop();
            if (!IS_STRING(hostV) || !IS_STRING(userV) ||
                !IS_STRING(passV) || !IS_STRING(dbV))
                return raiseError("mysqlConnect() host/user/pass/db must be strings");

            MYSQL* conn = mysql_init(NULL);
            if (!conn) return raiseError("mysqlConnect() failed to allocate handle");
            char reconnect = 1;
            mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
            if (!mysql_real_connect(conn,
                    AS_STRING(hostV)->chars, AS_STRING(userV)->chars,
                    AS_STRING(passV)->chars, AS_STRING(dbV)->chars,
                    (unsigned int)port, NULL, CLIENT_MULTI_STATEMENTS)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "mysqlConnect() failed: %s", mysql_error(conn));
                mysql_close(conn);
                return raiseError("%s", buf);
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)newDBConn(conn)}});
            break;
        }

        case BUILTIN_MYSQL_QUERY: {
            if (argc != 2) return raiseError("mysqlQuery() expects 2 arguments (conn, sql)");
            Value sqlV = pop(), connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlQuery() first arg must be a mysql connection");
            if (!IS_STRING(sqlV))   return raiseError("mysqlQuery() sql must be a string");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlQuery() connection is closed");
            MYSQL*     conn = (MYSQL*)dbc->conn;
            ObjString* sql  = AS_STRING(sqlV);
            if (mysql_real_query(conn, sql->chars, (unsigned long)sql->length) != 0)
                return raiseError("mysqlQuery() failed: %s", mysql_error(conn));
            MYSQL_RES* res = mysql_store_result(conn);
            if (!res) {
                if (mysql_field_count(conn) != 0)
                    return raiseError("mysqlQuery() fetch failed: %s", mysql_error(conn));
                push(NUMBER_VAL((double)mysql_affected_rows(conn)));
                break;
            }
            unsigned int nf = mysql_num_fields(res);
            MYSQL_FIELD* fields = mysql_fetch_fields(res);
            ObjList* rows = newList();
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}});
            MYSQL_ROW row; unsigned long* lens;
            while ((row = mysql_fetch_row(res)) != NULL) {
                lens = mysql_fetch_lengths(res);
                ObjDict* rd = newDict();
                push((Value){VAL_OBJ, {.obj = (Obj*)rd}});
                for (unsigned int i = 0; i < nf; i++) {
                    ObjString* k = copyString(fields[i].name, (int)strlen(fields[i].name));
                    push((Value){VAL_OBJ, {.obj = (Obj*)k}});
                    Value v = mysqlFieldToValue(row[i], lens[i], fields[i].type);
                    push(v);
                    dictSet(rd, k, v);
                    pop(); pop();
                }
                listAppend(rows, (Value){VAL_OBJ, {.obj = (Obj*)rd}});
                pop();
            }
            mysql_free_result(res);
            pop(); // rows guard
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}});
            break;
        }

        case BUILTIN_MYSQL_ESCAPE: {
            if (argc != 2) return raiseError("mysqlEscape() expects 2 arguments");
            Value strV = pop(), connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlEscape() first arg must be a mysql connection");
            if (!IS_STRING(strV))   return raiseError("mysqlEscape() second arg must be a string");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlEscape() connection is closed");
            ObjString* src = AS_STRING(strV);
            char* buf = (char*)malloc((size_t)src->length * 2 + 1);
            if (!buf) return raiseError("mysqlEscape() out of memory");
            unsigned long outLen = mysql_real_escape_string(
                (MYSQL*)dbc->conn, buf, src->chars, (unsigned long)src->length);
            ObjString* escaped = copyString(buf, (int)outLen);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)escaped}});
            break;
        }

        case BUILTIN_MYSQL_LAST_INSERT_ID: {
            if (argc != 1) return raiseError("mysqlLastInsertId() expects 1 argument");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlLastInsertId() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlLastInsertId() connection is closed");
            push(NUMBER_VAL((double)mysql_insert_id((MYSQL*)dbc->conn)));
            break;
        }

        case BUILTIN_MYSQL_ERROR: {
            if (argc != 1) return raiseError("mysqlError() expects 1 argument");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlError() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlError() connection is closed");
            const char* err = mysql_error((MYSQL*)dbc->conn);
            if (!err || err[0] == '\0') { push(NIL_VAL); break; }
            push((Value){VAL_OBJ, {.obj = (Obj*)copyString(err, (int)strlen(err))}});
            break;
        }

        case BUILTIN_MYSQL_CLOSE: {
            if (argc != 1) return raiseError("mysqlClose() expects 1 argument");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlClose() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->fromPool && !dbc->closed) {
                return raiseError(
                    "mysqlClose() cannot be used on a connection checked out "
                    "from a pool — this connection is owned by the pool. "
                    "Use mysqlPoolRelease(pool, conn) instead.");
            }
            if (!dbc->closed && dbc->conn) {
                mysql_close((MYSQL*)dbc->conn);
                dbc->conn   = NULL;
                dbc->closed = true;
            }
            push(NIL_VAL);
            break;
        }

        // ── NEW: Ping ─────────────────────────────────────────────────
        // mysqlPing(conn) -> bool
        // Returns true if the server is reachable (reconnects if needed).
        case BUILTIN_MYSQL_PING: {
            if (argc != 1) return raiseError("mysqlPing() expects 1 argument (conn)");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlPing() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) { push(BOOL_VAL(false)); break; }
            int rc = mysql_ping((MYSQL*)dbc->conn);
            push(BOOL_VAL(rc == 0));
            break;
        }

        // ── NEW: Affected rows ────────────────────────────────────────
        // mysqlAffectedRows(conn) -> number
        case BUILTIN_MYSQL_AFFECTED_ROWS: {
            if (argc != 1) return raiseError("mysqlAffectedRows() expects 1 argument (conn)");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlAffectedRows() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlAffectedRows() connection is closed");
            push(NUMBER_VAL((double)mysql_affected_rows((MYSQL*)dbc->conn)));
            break;
        }

        // ── NEW: Multi-result query ───────────────────────────────────
        // mysqlQueryMulti(conn, sql) -> list of lists-of-dicts (one per result set)
        //
        // Requires CLIENT_MULTI_STATEMENTS to have been negotiated at
        // connect time. Both mysqlConnect() and the connection-pool opener
        // (poolOpenConn) now pass CLIENT_MULTI_STATEMENTS to
        // mysql_real_connect() unconditionally, so every connection/pool
        // handle produced by this VM supports multi-statement queries
        // (semicolon-separated SELECTs, CALL sproc, etc.) automatically.
        // No extra script-level flag is needed or exists.
        case BUILTIN_MYSQL_QUERY_MULTI: {
            if (argc != 2) return raiseError("mysqlQueryMulti() expects 2 arguments (conn, sql)");
            Value sqlV = pop(), connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlQueryMulti() first arg must be a mysql connection");
            if (!IS_STRING(sqlV))   return raiseError("mysqlQueryMulti() sql must be a string");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlQueryMulti() connection is closed");
            MYSQL* conn = (MYSQL*)dbc->conn;
            ObjString* sql = AS_STRING(sqlV);

            if (mysql_real_query(conn, sql->chars, (unsigned long)sql->length) != 0)
                return raiseError("mysqlQueryMulti() failed: %s", mysql_error(conn));

            ObjList* allSets = newList();
            push((Value){VAL_OBJ, {.obj = (Obj*)allSets}}); // GC guard

            do {
                const char* err = NULL;
                ObjList* rs = consumeResultSet(conn, &err);
                if (!rs) return raiseError("mysqlQueryMulti() result error: %s", err);
                push((Value){VAL_OBJ, {.obj = (Obj*)rs}});
                listAppend(allSets, (Value){VAL_OBJ, {.obj = (Obj*)rs}});
                pop();
            } while (mysql_next_result(conn) == 0);

            pop(); // allSets guard
            push((Value){VAL_OBJ, {.obj = (Obj*)allSets}});
            break;
        }

        // ── NEW: Batch INSERT ─────────────────────────────────────────
        // mysqlInsertBatch(conn, table, listOfDicts) -> number (affected rows)
        //
        // Builds a single multi-row INSERT from a list of dicts. All dicts
        // must have the same keys. Values are escaped automatically.
        // Example:
        //   mysqlInsertBatch(conn, "users", [
        //       {name: "Alice", age: 30},
        //       {name: "Bob",   age: 25},
        //   ]);
        case BUILTIN_MYSQL_INSERT_BATCH: {
            if (argc != 3)
                return raiseError("mysqlInsertBatch() expects 3 arguments (conn, table, listOfDicts)");
            Value rowsV  = pop();
            Value tableV = pop();
            Value connV  = pop();
            if (!IS_DB_CONN(connV))   return raiseError("mysqlInsertBatch() first arg must be a mysql connection");
            if (!IS_STRING(tableV))   return raiseError("mysqlInsertBatch() table must be a string");
            if (!IS_LIST(rowsV))      return raiseError("mysqlInsertBatch() third arg must be a list of dicts");

            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlInsertBatch() connection is closed");
            MYSQL*  conn  = (MYSQL*)dbc->conn;
            ObjList* list = AS_LIST(rowsV);

            if (list->count == 0) { push(NUMBER_VAL(0)); break; }
            if (!IS_DICT(list->items[0]))
                return raiseError("mysqlInsertBatch() each row must be a dict");

            ObjDict* first = AS_DICT(list->items[0]);

            // ── Build column header ──
            // We use a dynamic buffer (realloc-based) to avoid fixed limits.
            size_t cap = 4096;
            char*  sql = (char*)malloc(cap);
            if (!sql) return raiseError("mysqlInsertBatch() out of memory");
            size_t len = 0;

#define SQL_APPEND(ptr, plen) do { \
    size_t _add = strlen(ptr); \
    while (len + _add + 1 > cap) { cap *= 2; sql = realloc(sql, cap); \
        if (!sql) return raiseError("mysqlInsertBatch() out of memory"); } \
    memcpy(sql + len, ptr, _add); len += _add; sql[len] = '\0'; \
} while(0)

            char tmp[512];
            snprintf(tmp, sizeof(tmp), "INSERT INTO `%s` (", AS_STRING(tableV)->chars);
            SQL_APPEND(tmp, len);

            // Collect column names from first row
            const char** colNames  = (const char**)malloc(sizeof(char*) * first->count);
            int*          colLens  = (int*)malloc(sizeof(int) * first->count);
            if (!colNames || !colLens) { free(sql); return raiseError("mysqlInsertBatch() out of memory"); }
            int ncols = 0;

            // Iterate dict entries
            for (int b = 0; b < first->capacity; b++) {
                DictEntry* e = &first->entries[b];
                if (!e->key) continue;
                if (ncols > 0) SQL_APPEND(",", len);
                snprintf(tmp, sizeof(tmp), "`%s`", e->key->chars);
                SQL_APPEND(tmp, len);
                colNames[ncols] = e->key->chars;
                colLens[ncols]  = e->key->length;
                ncols++;
            }
            SQL_APPEND(") VALUES ", len);

            // ── Build value rows ──
            char* escBuf = (char*)malloc(4096);
            size_t escCap = 4096;

            for (int r = 0; r < list->count; r++) {
                if (!IS_DICT(list->items[r])) {
                    free(sql); free(colNames); free(colLens); free(escBuf);
                    return raiseError("mysqlInsertBatch() row %d is not a dict", r);
                }
                ObjDict* row = AS_DICT(list->items[r]);
                if (r > 0) SQL_APPEND(",", len);
                SQL_APPEND("(", len);

                for (int c = 0; c < ncols; c++) {
                    ObjString* keyObj = copyString(colNames[c], colLens[c]);
                    Value val;
                    bool found = dictGet(row, keyObj, &val);
                    if (c > 0) SQL_APPEND(",", len);

                    if (!found || IS_NIL(val)) {
                        SQL_APPEND("NULL", len);
                    } else if (IS_BOOL(val)) {
                        SQL_APPEND(AS_BOOL(val) ? "1" : "0", len);
                    } else if (IS_NUMBER(val)) {
                        snprintf(tmp, sizeof(tmp), "%.17g", AS_NUMBER(val));
                        SQL_APPEND(tmp, len);
                    } else if (IS_STRING(val)) {
                        ObjString* sv = AS_STRING(val);
                        size_t need = (size_t)sv->length * 2 + 3;
                        if (need > escCap) {
                            escCap = need * 2;
                            escBuf = (char*)realloc(escBuf, escCap);
                            if (!escBuf) { free(sql); free(colNames); free(colLens);
                                return raiseError("mysqlInsertBatch() out of memory"); }
                        }
                        escBuf[0] = '\'';
                        unsigned long el = mysql_real_escape_string(
                            conn, escBuf + 1, sv->chars, (unsigned long)sv->length);
                        escBuf[1 + el] = '\'';
                        escBuf[2 + el] = '\0';
                        SQL_APPEND(escBuf, len);
                    } else {
                        SQL_APPEND("NULL", len);
                    }
                }
                SQL_APPEND(")", len);
            }
            free(escBuf); free(colNames); free(colLens);
#undef SQL_APPEND

            int rc = mysql_real_query(conn, sql, (unsigned long)len);
            free(sql);
            if (rc != 0)
                return raiseError("mysqlInsertBatch() failed: %s", mysql_error(conn));
            push(NUMBER_VAL((double)mysql_affected_rows(conn)));
            break;
        }

        // ── NEW: Transaction control ──────────────────────────────────
        // mysqlBegin(conn)    -> nil    (START TRANSACTION)
        // mysqlCommit(conn)   -> nil
        // mysqlRollback(conn) -> nil

        case BUILTIN_MYSQL_BEGIN: {
            if (argc != 1) return raiseError("mysqlBegin() expects 1 argument (conn)");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlBegin() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlBegin() connection is closed");
            if (mysql_query((MYSQL*)dbc->conn, "START TRANSACTION") != 0)
                return raiseError("mysqlBegin() failed: %s", mysql_error((MYSQL*)dbc->conn));
            push(NIL_VAL);
            break;
        }

        case BUILTIN_MYSQL_COMMIT: {
            if (argc != 1) return raiseError("mysqlCommit() expects 1 argument (conn)");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlCommit() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlCommit() connection is closed");
            if (mysql_commit((MYSQL*)dbc->conn) != 0)
                return raiseError("mysqlCommit() failed: %s", mysql_error((MYSQL*)dbc->conn));
            push(NIL_VAL);
            break;
        }

        case BUILTIN_MYSQL_ROLLBACK: {
            if (argc != 1) return raiseError("mysqlRollback() expects 1 argument (conn)");
            Value connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlRollback() arg must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlRollback() connection is closed");
            if (mysql_rollback((MYSQL*)dbc->conn) != 0)
                return raiseError("mysqlRollback() failed: %s", mysql_error((MYSQL*)dbc->conn));
            push(NIL_VAL);
            break;
        }

        // ── NEW: Prepared statements ──────────────────────────────────
        // mysqlPrepare(conn, sql)          -> stmtHandle
        // mysqlExecute(stmtHandle, list)   -> list of dicts | number
        // mysqlStmtClose(stmtHandle)       -> nil
        //
        // Parameters are passed as a list; types are inferred from the
        // script values (number → MYSQL_TYPE_DOUBLE, string → MYSQL_TYPE_STRING,
        // nil → MYSQL_TYPE_NULL).
        //
        // Stmt handles are represented as ObjDBConn with conn=NULL and
        // stmt != NULL. Distinguish with dbc->isStmt flag (see object.h patch).

        case BUILTIN_MYSQL_PREPARE: {
            if (argc != 2) return raiseError("mysqlPrepare() expects 2 arguments (conn, sql)");
            Value sqlV = pop(), connV = pop();
            if (!IS_DB_CONN(connV)) return raiseError("mysqlPrepare() first arg must be a mysql connection");
            if (!IS_STRING(sqlV))   return raiseError("mysqlPrepare() sql must be a string");
            ObjDBConn* dbc = AS_DB_CONN(connV);
            if (dbc->closed || !dbc->conn) return raiseError("mysqlPrepare() connection is closed");
            MYSQL* conn = (MYSQL*)dbc->conn;
            ObjString* sql = AS_STRING(sqlV);

            MYSQL_STMT* stmt = mysql_stmt_init(conn);
            if (!stmt) return raiseError("mysqlPrepare() failed to allocate statement");
            if (mysql_stmt_prepare(stmt, sql->chars, (unsigned long)sql->length) != 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "mysqlPrepare() failed: %s", mysql_stmt_error(stmt));
                mysql_stmt_close(stmt);
                return raiseError("%s", buf);
            }

            // Wrap in ObjDBConn with isStmt=true
            ObjDBConn* sh = newDBConn(NULL);  // conn=NULL, we'll set stmt below
            sh->isStmt = true;
            sh->stmt   = stmt;
            push((Value){VAL_OBJ, {.obj = (Obj*)sh}});
            break;
        }

        case BUILTIN_MYSQL_EXECUTE: {
            if (argc != 2)
                return raiseError("mysqlExecute() expects 2 arguments (stmtHandle, paramList)");
            Value paramsV = pop(), stmtV = pop();
            if (!IS_DB_CONN(stmtV))
                return raiseError("mysqlExecute() first arg must be a statement handle");
            ObjDBConn* sh = AS_DB_CONN(stmtV);
            if (!sh->isStmt || !sh->stmt)
                return raiseError("mysqlExecute() first arg must be a statement handle");
            if (!IS_LIST(paramsV))
                return raiseError("mysqlExecute() second arg must be a list of parameters");

            MYSQL_STMT* stmt = sh->stmt;
            ObjList*    plist = AS_LIST(paramsV);
            int         nparams = plist->count;

            // ── Bind input parameters ──
            MYSQL_BIND* binds = NULL;
            double*     numBufs = NULL;
            unsigned long* strLens = NULL;
            my_bool*    isNullFlags = NULL;

            if (nparams > 0) {
                binds       = (MYSQL_BIND*)calloc(nparams, sizeof(MYSQL_BIND));
                numBufs     = (double*)malloc(sizeof(double) * nparams);
                strLens     = (unsigned long*)malloc(sizeof(unsigned long) * nparams);
                isNullFlags = (my_bool*)calloc(nparams, sizeof(my_bool));
                if (!binds || !numBufs || !strLens || !isNullFlags) {
                    free(binds); free(numBufs); free(strLens); free(isNullFlags);
                    return raiseError("mysqlExecute() out of memory");
                }

                for (int i = 0; i < nparams; i++) {
                    Value v = plist->items[i];
                    if (IS_NIL(v)) {
                        isNullFlags[i] = 1;
                        binds[i].buffer_type = MYSQL_TYPE_NULL;
                        binds[i].is_null     = &isNullFlags[i];
                    } else if (IS_NUMBER(v)) {
                        numBufs[i]           = AS_NUMBER(v);
                        binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                        binds[i].buffer      = &numBufs[i];
                        binds[i].is_null     = &isNullFlags[i];
                    } else if (IS_BOOL(v)) {
                        numBufs[i]           = AS_BOOL(v) ? 1.0 : 0.0;
                        binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                        binds[i].buffer      = &numBufs[i];
                        binds[i].is_null     = &isNullFlags[i];
                    } else if (IS_STRING(v)) {
                        ObjString* s         = AS_STRING(v);
                        strLens[i]           = (unsigned long)s->length;
                        binds[i].buffer_type = MYSQL_TYPE_STRING;
                        binds[i].buffer      = s->chars;
                        binds[i].buffer_length = (unsigned long)s->length;
                        binds[i].length      = &strLens[i];
                        binds[i].is_null     = &isNullFlags[i];
                    } else {
                        isNullFlags[i] = 1;
                        binds[i].buffer_type = MYSQL_TYPE_NULL;
                        binds[i].is_null     = &isNullFlags[i];
                    }
                }

                if (mysql_stmt_bind_param(stmt, binds) != 0) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "mysqlExecute() bind failed: %s",
                             mysql_stmt_error(stmt));
                    free(binds); free(numBufs); free(strLens); free(isNullFlags);
                    return raiseError("%s", buf);
                }
            }

            if (mysql_stmt_execute(stmt) != 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "mysqlExecute() execute failed: %s",
                         mysql_stmt_error(stmt));
                free(binds); free(numBufs); free(strLens); free(isNullFlags);
                return raiseError("%s", buf);
            }
            free(binds); free(numBufs); free(strLens); free(isNullFlags);

            // ── Fetch results ──
            MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
            if (!meta) {
                // Non-SELECT
                push(NUMBER_VAL((double)mysql_stmt_affected_rows(stmt)));
                break;
            }

            unsigned int nf = mysql_num_fields(meta);
            MYSQL_FIELD* fields = mysql_fetch_fields(meta);

            // Allocate result binds
            MYSQL_BIND* resBind = (MYSQL_BIND*)calloc(nf, sizeof(MYSQL_BIND));
            char**      resBufs = (char**)calloc(nf, sizeof(char*));
            unsigned long* resLens   = (unsigned long*)calloc(nf, sizeof(unsigned long));
            unsigned long* resBufCap = (unsigned long*)calloc(nf, sizeof(unsigned long));
            my_bool* resNull = (my_bool*)calloc(nf, sizeof(my_bool));
            if (!resBind || !resBufs || !resLens || !resBufCap || !resNull) {
                mysql_free_result(meta);
                free(resBind); free(resBufs); free(resLens); free(resBufCap); free(resNull);
                return raiseError("mysqlExecute() out of memory");
            }

            // Start with a modest per-column buffer; mysql_stmt_fetch()
            // reports MYSQL_DATA_TRUNCATED (rather than silently dropping
            // data or ending the result set) when a value doesn't fit, and
            // we grow + re-fetch that specific column below when that
            // happens — so this initial size only affects how often we
            // need to re-fetch, never correctness.
            const unsigned long INITIAL_COL_BUF = 4096;
            for (unsigned int i = 0; i < nf; i++) {
                resBufCap[i] = INITIAL_COL_BUF;
                resBufs[i]   = (char*)malloc(resBufCap[i]);
                if (!resBufs[i]) {
                    for (unsigned int j = 0; j < i; j++) free(resBufs[j]);
                    mysql_free_result(meta);
                    free(resBind); free(resBufs); free(resLens); free(resBufCap); free(resNull);
                    return raiseError("mysqlExecute() out of memory");
                }
                resBind[i].buffer_type   = MYSQL_TYPE_STRING;
                resBind[i].buffer        = resBufs[i];
                resBind[i].buffer_length = resBufCap[i];
                resBind[i].length        = &resLens[i];
                resBind[i].is_null       = &resNull[i];
            }

            mysql_stmt_bind_result(stmt, resBind);
            mysql_stmt_store_result(stmt);

            ObjList* rows = newList();
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}});

            int fetchRc;
            bool fetchFailed = false;
            while (true) {
                fetchRc = mysql_stmt_fetch(stmt);
                if (fetchRc == MYSQL_NO_DATA) break;
                if (fetchRc != 0 && fetchRc != MYSQL_DATA_TRUNCATED) { fetchFailed = true; break; }

                if (fetchRc == MYSQL_DATA_TRUNCATED) {
                    // At least one column didn't fit in its buffer.
                    // Grow the buffer for each truncated column to its
                    // actual length and re-fetch just that column so no
                    // data (or rows) are silently lost.
                    for (unsigned int i = 0; i < nf; i++) {
                        if (!resNull[i] && resLens[i] > resBufCap[i]) {
                            free(resBufs[i]);
                            resBufCap[i] = resLens[i] + 1;
                            resBufs[i]   = (char*)malloc(resBufCap[i]);
                            if (!resBufs[i]) { fetchFailed = true; break; }
                            resBind[i].buffer        = resBufs[i];
                            resBind[i].buffer_length = resBufCap[i];
                            if (mysql_stmt_fetch_column(stmt, &resBind[i], i, 0) != 0) {
                                fetchFailed = true;
                                break;
                            }
                        }
                    }
                    if (fetchFailed) break;
                }

                ObjDict* rd = newDict();
                push((Value){VAL_OBJ, {.obj = (Obj*)rd}});
                for (unsigned int i = 0; i < nf; i++) {
                    ObjString* k = copyString(fields[i].name, (int)strlen(fields[i].name));
                    push((Value){VAL_OBJ, {.obj = (Obj*)k}});
                    Value v = resNull[i]
                        ? NIL_VAL
                        : mysqlFieldToValue(resBufs[i], resLens[i], fields[i].type);
                    push(v);
                    dictSet(rd, k, v);
                    pop(); pop();
                }
                listAppend(rows, (Value){VAL_OBJ, {.obj = (Obj*)rd}});
                pop();
            }

            if (fetchFailed) {
                char buf[256];
                snprintf(buf, sizeof(buf), "mysqlExecute() fetch failed: %s", mysql_stmt_error(stmt));
                for (unsigned int i = 0; i < nf; i++) free(resBufs[i]);
                free(resBind); free(resBufs); free(resLens); free(resBufCap); free(resNull);
                mysql_free_result(meta);
                mysql_stmt_free_result(stmt);
                pop(); // rows guard
                return raiseError("%s", buf);
            }

            for (unsigned int i = 0; i < nf; i++) free(resBufs[i]);
            free(resBind); free(resBufs); free(resLens); free(resBufCap); free(resNull);
            mysql_free_result(meta);
            mysql_stmt_free_result(stmt);

            pop(); // rows guard
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}});
            break;
        }

        case BUILTIN_MYSQL_STMT_CLOSE: {
            if (argc != 1) return raiseError("mysqlStmtClose() expects 1 argument (stmtHandle)");
            Value stmtV = pop();
            if (!IS_DB_CONN(stmtV)) return raiseError("mysqlStmtClose() arg must be a statement handle");
            ObjDBConn* sh = AS_DB_CONN(stmtV);
            if (sh->isStmt && sh->stmt) {
                mysql_stmt_close(sh->stmt);
                sh->stmt   = NULL;
                sh->closed = true;
            }
            push(NIL_VAL);
            break;
        }

        // ── NEW: Connection pool ──────────────────────────────────────
        // mysqlPoolCreate(host, user, pass, db, maxSize [, port]) -> poolHandle
        // mysqlPoolGet(poolHandle)              -> conn (checked out from pool)
        // mysqlPoolRelease(poolHandle, conn)    -> nil  (return to pool)
        // mysqlPoolClose(poolHandle)            -> nil  (destroy all connections)

        case BUILTIN_MYSQL_POOL_CREATE: {
            if (argc < 5 || argc > 6)
                return raiseError("mysqlPoolCreate() expects 5-6 arguments (host, user, pass, db, maxSize [, port])");
            int port = 3306;
            if (argc == 6) {
                Value pv = pop();
                if (!IS_NUMBER(pv)) return raiseError("mysqlPoolCreate() port must be a number");
                port = (int)AS_NUMBER(pv);
            }
            Value maxV = pop(), dbV = pop(), passV = pop(), userV = pop(), hostV = pop();
            if (!IS_NUMBER(maxV))   return raiseError("mysqlPoolCreate() maxSize must be a number");
            if (!IS_STRING(hostV) || !IS_STRING(userV) ||
                !IS_STRING(passV) || !IS_STRING(dbV))
                return raiseError("mysqlPoolCreate() host/user/pass/db must be strings");

            int maxSize = (int)AS_NUMBER(maxV);
            if (maxSize < 1 || maxSize > POOL_MAX_SIZE)
                return raiseError("mysqlPoolCreate() maxSize must be 1-%d", POOL_MAX_SIZE);

            MysqlPool* pool = (MysqlPool*)calloc(1, sizeof(MysqlPool));
            if (!pool) return raiseError("mysqlPoolCreate() out of memory");

            pthread_mutex_init(&pool->lock, NULL);
            strncpy(pool->host, AS_STRING(hostV)->chars, sizeof(pool->host) - 1);
            strncpy(pool->user, AS_STRING(userV)->chars, sizeof(pool->user) - 1);
            strncpy(pool->pass, AS_STRING(passV)->chars, sizeof(pool->pass) - 1);
            strncpy(pool->db,   AS_STRING(dbV)->chars,   sizeof(pool->db)   - 1);
            pool->port    = port;
            pool->maxSize = maxSize;

            // Pre-open one connection to validate credentials immediately.
            char errbuf[256] = {0};
            MYSQL* c = poolOpenConn(pool, errbuf, sizeof(errbuf));
            if (!c) {
                pthread_mutex_destroy(&pool->lock);
                free(pool);
                return raiseError("mysqlPoolCreate() connect failed: %s", errbuf);
            }
            pool->conns[0] = c;
            pool->inUse[0] = false;
            pool->size     = 1;

            // Wrap in an ObjDBConn with isPool=true
            ObjDBConn* ph = newDBConn(NULL);
            ph->isPool = true;
            ph->pool   = pool;
            push((Value){VAL_OBJ, {.obj = (Obj*)ph}});
            break;
        }

        case BUILTIN_MYSQL_POOL_GET: {
            if (argc != 1) return raiseError("mysqlPoolGet() expects 1 argument (poolHandle)");
            Value poolV = pop();
            if (!IS_DB_CONN(poolV)) return raiseError("mysqlPoolGet() arg must be a pool handle");
            ObjDBConn* ph = AS_DB_CONN(poolV);
            if (!ph->isPool || !ph->pool) return raiseError("mysqlPoolGet() arg must be a pool handle");
            MysqlPool* p = (MysqlPool*)ph->pool;

            pthread_mutex_lock(&p->lock);
            if (p->closed) {
                pthread_mutex_unlock(&p->lock);
                return raiseError("mysqlPoolGet() pool is closed");
            }

            // Look for an idle connection
            for (int i = 0; i < p->size; i++) {
                if (!p->inUse[i]) {
                    // Quick ping to recover from idle timeout
                    if (mysql_ping(p->conns[i]) != 0) {
                        mysql_close(p->conns[i]);
                        char err[256];
                        p->conns[i] = poolOpenConn(p, err, sizeof(err));
                        if (!p->conns[i]) {
                            pthread_mutex_unlock(&p->lock);
                            return raiseError("mysqlPoolGet() reconnect failed: %s", err);
                        }
                    }
                    p->inUse[i] = true;
                    pthread_mutex_unlock(&p->lock);
                    ObjDBConn* dc = newDBConn(p->conns[i]);
                    dc->fromPool = true;
                    push((Value){VAL_OBJ, {.obj = (Obj*)dc}});
                    return INTERPRET_OK;
                }
            }

            // Grow pool if possible
            if (p->size < p->maxSize) {
                char errbuf[256];
                MYSQL* c = poolOpenConn(p, errbuf, sizeof(errbuf));
                if (!c) {
                    pthread_mutex_unlock(&p->lock);
                    return raiseError("mysqlPoolGet() grow failed: %s", errbuf);
                }
                int idx = p->size++;
                p->conns[idx] = c;
                p->inUse[idx] = true;
                pthread_mutex_unlock(&p->lock);
                ObjDBConn* dc = newDBConn(c);
                dc->fromPool = true;
                push((Value){VAL_OBJ, {.obj = (Obj*)dc}});
                return INTERPRET_OK;
            }

            pthread_mutex_unlock(&p->lock);
            return raiseError("mysqlPoolGet() pool exhausted (all %d connections in use)", p->maxSize);
        }

        case BUILTIN_MYSQL_POOL_RELEASE: {
            if (argc != 2) return raiseError("mysqlPoolRelease() expects 2 arguments (poolHandle, conn)");
            Value connV = pop(), poolV = pop();
            if (!IS_DB_CONN(poolV)) return raiseError("mysqlPoolRelease() first arg must be a pool handle");
            if (!IS_DB_CONN(connV)) return raiseError("mysqlPoolRelease() second arg must be a connection");
            ObjDBConn* ph = AS_DB_CONN(poolV);
            if (!ph->isPool || !ph->pool) return raiseError("mysqlPoolRelease() first arg must be a pool handle");
            ObjDBConn* dc = AS_DB_CONN(connV);
            if (!dc->fromPool) return raiseError("mysqlPoolRelease() second arg is not a connection checked out from a pool");
            if (dc->closed || !dc->conn) return raiseError("mysqlPoolRelease() connection was already released or closed");
            MysqlPool* p  = (MysqlPool*)ph->pool;

            pthread_mutex_lock(&p->lock);
            MYSQL* rawConn = (MYSQL*)dc->conn;
            for (int i = 0; i < p->size; i++) {
                if (p->conns[i] == rawConn) {
                    p->inUse[i] = false;
                    // Auto-rollback: if the returned connection has an open
                    // transaction (auto-commit off), roll it back to leave
                    // the connection clean for the next caller.
                    mysql_rollback(rawConn);
                    mysql_autocommit(rawConn, 1);
                    break;
                }
            }
            // Invalidate the handle so the script can't keep using it.
            dc->conn   = NULL;
            dc->closed = true;
            pthread_mutex_unlock(&p->lock);
            push(NIL_VAL);
            break;
        }

        case BUILTIN_MYSQL_POOL_CLOSE: {
            if (argc != 1) return raiseError("mysqlPoolClose() expects 1 argument (poolHandle)");
            Value poolV = pop();
            if (!IS_DB_CONN(poolV)) return raiseError("mysqlPoolClose() arg must be a pool handle");
            ObjDBConn* ph = AS_DB_CONN(poolV);
            if (!ph->isPool || !ph->pool) return raiseError("mysqlPoolClose() arg must be a pool handle");
            MysqlPool* p = (MysqlPool*)ph->pool;

            pthread_mutex_lock(&p->lock);
            p->closed = true;
            for (int i = 0; i < p->size; i++) {
                if (p->conns[i]) {
                    mysql_close(p->conns[i]);
                    p->conns[i] = NULL;
                }
            }
            pthread_mutex_unlock(&p->lock);
            pthread_mutex_destroy(&p->lock);
            free(p);
            ph->pool   = NULL;
            ph->closed = true;
            push(NIL_VAL);
            break;
        }

        default:
            return raiseError("Unknown MySQL builtin %d", id);
    }
    return INTERPRET_OK;
}