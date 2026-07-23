#include "tvm.h"
#include <mysql.h>

// ── MySQL / MariaDB builtins ───────────────────────────────────────────
// Thin wrapper around libmysqlclient (or MariaDB Connector/C, which ships
// the same mysql.h / libmysqlclient-compatible ABI). Mirrors the shape of
// net_tcp.c / net_http.c: pop args in reverse, validate, do the native
// call, push the result.
//
// Build note: this deliberately includes <mysql.h> (no subdirectory)
// rather than <mysql/mysql.h>, because the two common providers disagree
// on install layout — Oracle's libmysqlclient-dev puts it under
// /usr/include/mysql/, MariaDB's libmariadb-dev under
// /usr/include/mariadb/. Point the compiler at the right one with
// `pkg-config --cflags libmariadb` (or `mysqlclient`) and link with
// `pkg-config --libs libmariadb` (or `-lmysqlclient`).

// Called from object.c's freeObject() as a safety net for connections the
// script never explicitly closed (same role as tripCloseSocketHandle()
// for OBJ_SOCKET).
void tripMysqlCloseHandle(void* conn) {
    if (conn) mysql_close((MYSQL*)conn);
}

static Value mysqlFieldToValue(const char* data, unsigned long len, enum enum_field_types type) {
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
            // Text protocol always hands back a string, even for numeric
            // columns — convert so scripts get numbers, not "42".
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

InterpretResult callBuiltinMysql(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_MYSQL_CONNECT: {
            if (argc < 4 || argc > 5)
                return raiseError("mysqlConnect() expects 4-5 arguments (host, user, pass, db [, port])");

            int port = 3306;
            if (argc == 5) {
                Value portVal = pop();
                if (!IS_NUMBER(portVal)) return raiseError("mysqlConnect() port must be a number");
                port = (int)AS_NUMBER(portVal);
            }
            Value dbVal   = pop();
            Value passVal = pop();
            Value userVal = pop();
            Value hostVal = pop();
            if (!IS_STRING(hostVal) || !IS_STRING(userVal) || !IS_STRING(passVal) || !IS_STRING(dbVal))
                return raiseError("mysqlConnect() host, user, pass, db must all be strings");

            MYSQL* conn = mysql_init(NULL);
            if (conn == NULL) return raiseError("mysqlConnect() failed to allocate connection handle");

            // Reconnect defends against long-lived scripts (servers) whose
            // connection idles out; cheap to set and matches how the
            // tcp*/tls* builtins favor "just works" defaults.
            // NOTE: intentionally not using the `my_bool` typedef here —
            // MariaDB's mysql.h still defines it, but MySQL 8's client
            // headers removed it. A plain 1-byte `char` is ABI-compatible
            // with what mysql_options() expects for this flag on both.
            char reconnect = 1;
            mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

            MYSQL* result = mysql_real_connect(
                conn,
                AS_STRING(hostVal)->chars,
                AS_STRING(userVal)->chars,
                AS_STRING(passVal)->chars,
                AS_STRING(dbVal)->chars,
                (unsigned int)port,
                NULL, 0
            );
            if (result == NULL) {
                char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "mysqlConnect() failed: %s", mysql_error(conn));
                mysql_close(conn);
                return raiseError("%s", errbuf);
            }

            ObjDBConn* obj = newDBConn(conn);
            push((Value){VAL_OBJ, {.obj = (Obj*)obj}});
            break;
        }

        case BUILTIN_MYSQL_QUERY: {
            if (argc != 2) return raiseError("mysqlQuery() expects 2 arguments (connection, sql)");
            Value sqlVal  = pop();
            Value connVal = pop();
            if (!IS_DB_CONN(connVal)) return raiseError("mysqlQuery() first argument must be a mysql connection");
            if (!IS_STRING(sqlVal))   return raiseError("mysqlQuery() sql must be a string");

            ObjDBConn* dbc = AS_DB_CONN(connVal);
            if (dbc->closed || dbc->conn == NULL) return raiseError("mysqlQuery() connection is closed");
            MYSQL* conn = (MYSQL*)dbc->conn;
            ObjString* sqlStr = AS_STRING(sqlVal);

            if (mysql_real_query(conn, sqlStr->chars, (unsigned long)sqlStr->length) != 0)
                return raiseError("mysqlQuery() failed: %s", mysql_error(conn));

            MYSQL_RES* result = mysql_store_result(conn);
            if (result == NULL) {
                // NULL result is only an error if the query *should* have
                // produced a result set (mysql_field_count > 0). Otherwise
                // it's a normal INSERT/UPDATE/DELETE/DDL statement.
                if (mysql_field_count(conn) != 0)
                    return raiseError("mysqlQuery() failed to fetch results: %s", mysql_error(conn));
                push(NUMBER_VAL((double)mysql_affected_rows(conn)));
                break;
            }

            unsigned int numFields = mysql_num_fields(result);
            MYSQL_FIELD* fields = mysql_fetch_fields(result);

            ObjList* rows = newList();
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}}); // GC guard

            MYSQL_ROW row;
            unsigned long* lengths;
            while ((row = mysql_fetch_row(result)) != NULL) {
                lengths = mysql_fetch_lengths(result);
                ObjDict* rowDict = newDict();
                push((Value){VAL_OBJ, {.obj = (Obj*)rowDict}}); // GC guard
                for (unsigned int i = 0; i < numFields; i++) {
                    ObjString* key = copyString(fields[i].name, (int)strlen(fields[i].name));
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

            pop(); // rows
            push((Value){VAL_OBJ, {.obj = (Obj*)rows}});
            break;
        }

        case BUILTIN_MYSQL_ESCAPE: {
            if (argc != 2) return raiseError("mysqlEscape() expects 2 arguments (connection, str)");
            Value strVal  = pop();
            Value connVal = pop();
            if (!IS_DB_CONN(connVal)) return raiseError("mysqlEscape() first argument must be a mysql connection");
            if (!IS_STRING(strVal))   return raiseError("mysqlEscape() second argument must be a string");

            ObjDBConn* dbc = AS_DB_CONN(connVal);
            if (dbc->closed || dbc->conn == NULL) return raiseError("mysqlEscape() connection is closed");
            ObjString* src = AS_STRING(strVal);

            // Worst case every byte needs escaping, hence 2x + 1 (per the
            // documented mysql_real_escape_string() contract).
            char* buf = (char*)malloc((size_t)src->length * 2 + 1);
            if (buf == NULL) return raiseError("mysqlEscape() out of memory");
            unsigned long outLen = mysql_real_escape_string((MYSQL*)dbc->conn, buf, src->chars, (unsigned long)src->length);

            ObjString* escaped = copyString(buf, (int)outLen);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)escaped}});
            break;
        }

        case BUILTIN_MYSQL_LAST_INSERT_ID: {
            if (argc != 1) return raiseError("mysqlLastInsertId() expects 1 argument (connection)");
            Value connVal = pop();
            if (!IS_DB_CONN(connVal)) return raiseError("mysqlLastInsertId() argument must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connVal);
            if (dbc->closed || dbc->conn == NULL) return raiseError("mysqlLastInsertId() connection is closed");
            push(NUMBER_VAL((double)mysql_insert_id((MYSQL*)dbc->conn)));
            break;
        }

        case BUILTIN_MYSQL_ERROR: {
            if (argc != 1) return raiseError("mysqlError() expects 1 argument (connection)");
            Value connVal = pop();
            if (!IS_DB_CONN(connVal)) return raiseError("mysqlError() argument must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connVal);
            if (dbc->closed || dbc->conn == NULL) return raiseError("mysqlError() connection is closed");
            const char* err = mysql_error((MYSQL*)dbc->conn);
            if (err == NULL || err[0] == '\0') { push(NIL_VAL); break; }
            ObjString* s = copyString(err, (int)strlen(err));
            push((Value){VAL_OBJ, {.obj = (Obj*)s}});
            break;
        }

        case BUILTIN_MYSQL_CLOSE: {
            if (argc != 1) return raiseError("mysqlClose() expects 1 argument (connection)");
            Value connVal = pop();
            if (!IS_DB_CONN(connVal)) return raiseError("mysqlClose() argument must be a mysql connection");
            ObjDBConn* dbc = AS_DB_CONN(connVal);
            if (!dbc->closed && dbc->conn != NULL) {
                mysql_close((MYSQL*)dbc->conn);
                dbc->closed = true;
            }
            push(NIL_VAL);
            break;
        }

        default:
            return raiseError("Unknown MySQL builtin %d", id);
    }
    return INTERPRET_OK;
}
