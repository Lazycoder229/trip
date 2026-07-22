#include "tvm.h"
#include <time.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
// MinGW links against Windows CRT which lacks strptime.
// Minimal implementation covering common strftime-compatible specifiers.
static char* strptime(const char* s, const char* fmt, struct tm* tm) {
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'Y': { // 4-digit year
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s || end - s != 4) return NULL;
                    tm->tm_year = (int)v - 1900; s = end; break;
                }
                case 'm': { // 2-digit month 01-12
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s) return NULL;
                    tm->tm_mon = (int)v - 1; s = end; break;
                }
                case 'd': { // 2-digit day 01-31
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s) return NULL;
                    tm->tm_mday = (int)v; s = end; break;
                }
                case 'H': { // hour 00-23
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s) return NULL;
                    tm->tm_hour = (int)v; s = end; break;
                }
                case 'M': { // minute 00-59
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s) return NULL;
                    tm->tm_min = (int)v; s = end; break;
                }
                case 'S': { // second 00-59
                    char* end; long v = strtol(s, &end, 10);
                    if (end == s) return NULL;
                    tm->tm_sec = (int)v; s = end; break;
                }
                case '%': { // literal %
                    if (*s != '%') return NULL;
                    s++; break;
                }
                default: return NULL; // unsupported specifier
            }
            fmt++;
        } else {
            if (*s != *fmt) return NULL;
            s++; fmt++;
        }
    }
    return (char*)s;
}
#else
#include <unistd.h>
#endif

// Date/Calendar helper
#define DATE_DICT_SET(d, k, v) \
    dictSet((d), copyString((k), (int)strlen(k)), NUMBER_VAL((double)(v)))

#define PUSH_TM_DICT(tm_ptr) do {                                           \
    ObjDict* _d = newDict();                                                \
    DATE_DICT_SET(_d, "year",    (tm_ptr)->tm_year + 1900);                 \
    DATE_DICT_SET(_d, "month",   (tm_ptr)->tm_mon  + 1);                   \
    DATE_DICT_SET(_d, "day",     (tm_ptr)->tm_mday);                        \
    DATE_DICT_SET(_d, "hour",    (tm_ptr)->tm_hour);                        \
    DATE_DICT_SET(_d, "minute",  (tm_ptr)->tm_min);                         \
    DATE_DICT_SET(_d, "second",  (tm_ptr)->tm_sec);                         \
    DATE_DICT_SET(_d, "weekday", (tm_ptr)->tm_wday);                       \
    DATE_DICT_SET(_d, "yearday", (tm_ptr)->tm_yday + 1);                   \
    DATE_DICT_SET(_d, "dst",     (tm_ptr)->tm_isdst > 0 ? 1 : 0);          \
    push((Value){VAL_OBJ, {.obj = (Obj*)_d}});                             \
} while(0)


InterpretResult callBuiltinTime(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_TIME: {
            if (argc != 0) return raiseError("time() takes no arguments");
#ifdef _WIN32
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            double secs = (double)(t - 116444736000000000ULL) / 10000000.0;
            push(NUMBER_VAL(secs));
#else
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            push(NUMBER_VAL((double)ts.tv_sec + (double)ts.tv_nsec / 1e9));
#endif
            break;
        }

        case BUILTIN_TIME_MS: {
            if (argc != 0) return raiseError("timeMs() takes no arguments");
#ifdef _WIN32
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            double ms = (double)(t - 116444736000000000ULL) / 10000.0;
            push(NUMBER_VAL(ms));
#else
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            push(NUMBER_VAL((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6));
#endif
            break;
        }

        case BUILTIN_SLEEP: {
            if (argc != 1) return raiseError("sleep() expects 1 argument (milliseconds)");
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("sleep() argument must be a number");
            double ms = AS_NUMBER(v);
            if (ms < 0) return raiseError("sleep() argument must be non-negative");
#ifdef _WIN32
            Sleep((DWORD)ms);
#else
            struct timespec ts;
            ts.tv_sec  = (time_t)(ms / 1000.0);
            ts.tv_nsec = (long)(((long long)ms % 1000) * 1000000L);
            nanosleep(&ts, NULL);
#endif
            push(NIL_VAL);
            break;
        }

        case BUILTIN_DATE_NOW: {
            if (argc != 0) return raiseError("dateNow() takes no arguments");
            time_t t = time(NULL);
            struct tm* tm_ptr = localtime(&t);
            if (!tm_ptr) return raiseError("dateNow() failed to get local time");
            PUSH_TM_DICT(tm_ptr);
            break;
        }

        case BUILTIN_DATE_LOCAL: {
            if (argc > 1) return raiseError("dateLocal() expects 0 or 1 argument (timestamp)");
            time_t t;
            if (argc == 1) {
                Value v = pop();
                if (!IS_NUMBER(v)) return raiseError("dateLocal() argument must be a number");
                t = (time_t)AS_NUMBER(v);
            } else {
                t = time(NULL);
            }
            struct tm* tm_ptr = localtime(&t);
            if (!tm_ptr) return raiseError("dateLocal() localtime() failed");
            PUSH_TM_DICT(tm_ptr);
            break;
        }

        case BUILTIN_DATE_UTC: {
            if (argc > 1) return raiseError("dateUTC() expects 0 or 1 argument (timestamp)");
            time_t t;
            if (argc == 1) {
                Value v = pop();
                if (!IS_NUMBER(v)) return raiseError("dateUTC() argument must be a number");
                t = (time_t)AS_NUMBER(v);
            } else {
                t = time(NULL);
            }
            struct tm* tm_ptr = gmtime(&t);
            if (!tm_ptr) return raiseError("dateUTC() gmtime() failed");
            PUSH_TM_DICT(tm_ptr);
            break;
        }

        case BUILTIN_DATE_FORMAT: {
            if (argc != 2) return raiseError("dateFormat() expects 2 arguments (timestamp, format)");
            Value fmtVal = pop();
            Value tsVal  = pop();
            if (!IS_NUMBER(tsVal)) return raiseError("dateFormat() first argument must be a timestamp number");
            if (!IS_STRING(fmtVal)) return raiseError("dateFormat() second argument must be a format string");
            time_t t = (time_t)AS_NUMBER(tsVal);
            const char* fmt = AS_STRING(fmtVal)->chars;
            struct tm* tm_ptr = localtime(&t);
            if (!tm_ptr) return raiseError("dateFormat() localtime() failed");
            char buf[512];
            size_t written = strftime(buf, sizeof(buf), fmt, tm_ptr);
            if (written == 0) return raiseError("dateFormat() format string produced empty output (buffer too small or bad format?)");
            push((Value){VAL_OBJ, {.obj = (Obj*)copyString(buf, (int)written)}});
            break;
        }

        case BUILTIN_DATE_PARSE: {
            if (argc != 2) return raiseError("dateParse() expects 2 arguments (string, format)");
            Value fmtVal = pop();
            Value strVal = pop();
            if (!IS_STRING(strVal)) return raiseError("dateParse() first argument must be a string");
            if (!IS_STRING(fmtVal)) return raiseError("dateParse() second argument must be a format string");
            const char* src = AS_STRING(strVal)->chars;
            const char* fmt = AS_STRING(fmtVal)->chars;
            struct tm tm_val;
            memset(&tm_val, 0, sizeof(tm_val));
            tm_val.tm_isdst = -1;
            const char* end = strptime(src, fmt, &tm_val);
            if (!end) {
                push(NIL_VAL);
            } else {
                time_t t = mktime(&tm_val);
                push(NUMBER_VAL((double)t));
            }
            break;
        }

        case BUILTIN_DATE_MAKE: {
            if (argc != 6) return raiseError("dateMake() expects 6 arguments (year, month, day, hour, minute, second)");
            Value secVal = pop();
            Value minVal = pop();
            Value hourVal = pop();
            Value dayVal  = pop();
            Value monVal  = pop();
            Value yearVal = pop();
            if (!IS_NUMBER(yearVal) || !IS_NUMBER(monVal)  || !IS_NUMBER(dayVal) ||
                !IS_NUMBER(hourVal) || !IS_NUMBER(minVal)  || !IS_NUMBER(secVal)) {
                return raiseError("dateMake() all arguments must be numbers");
            }
            struct tm tm_val;
            memset(&tm_val, 0, sizeof(tm_val));
            tm_val.tm_year  = (int)AS_NUMBER(yearVal) - 1900;
            tm_val.tm_mon   = (int)AS_NUMBER(monVal)  - 1;
            tm_val.tm_mday  = (int)AS_NUMBER(dayVal);
            tm_val.tm_hour  = (int)AS_NUMBER(hourVal);
            tm_val.tm_min   = (int)AS_NUMBER(minVal);
            tm_val.tm_sec   = (int)AS_NUMBER(secVal);
            tm_val.tm_isdst = -1;
            time_t t = mktime(&tm_val);
            if (t == (time_t)-1) return raiseError("dateMake() failed to convert date to timestamp (invalid date?)");
            push(NUMBER_VAL((double)t));
            break;
        }

        default:
            return raiseError("Unknown Time builtin %d", id);
    }
    return INTERPRET_OK;
}
