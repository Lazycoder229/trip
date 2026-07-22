// tests/test.h
//
// Minimal, dependency-free unit test harness for Trip's C internals.
// No cmocka/Unity/Criterion required — just this header + plain assert-style
// macros. Works with the same gcc/clang toolchain the project already uses.
//
// Usage pattern (see tests/test_scanner.c for a full example):
//
//     #include "test.h"
//     #include "../src/scanner.h"
//
//     TEST(number_literal) {
//         initScanner("42");
//         Token t = scanToken();
//         ASSERT_EQ_INT(TOKEN_NUMBER, t.type);
//     }
//
//     int main(void) {
//         RUN_TEST(number_literal);
//         TEST_SUMMARY();
//     }

#ifndef TRIP_TEST_H
#define TRIP_TEST_H

#include <stdio.h>
#include <string.h>

static int trip_tests_run    = 0;
static int trip_tests_failed = 0;
static int trip_asserts_failed_in_current = 0;
static const char* trip_current_test_name = "";

// Defines a test case as a static void function.
#define TEST(name) static void name(void)

// Runs a test case, tracking pass/fail and printing a one-line result.
#define RUN_TEST(name)                                                       \
    do {                                                                     \
        trip_current_test_name = #name;                                     \
        trip_asserts_failed_in_current = 0;                                  \
        trip_tests_run++;                                                    \
        name();                                                              \
        if (trip_asserts_failed_in_current == 0) {                          \
            printf("  [PASS] %s\n", #name);                                 \
        } else {                                                             \
            trip_tests_failed++;                                             \
        }                                                                    \
    } while (0)

// Prints the final summary and returns a shell-friendly exit code
// (0 = all passed, 1 = at least one failure) from main().
#define TEST_SUMMARY()                                                       \
    do {                                                                     \
        printf("\n%d test(s) run, %d failed\n", trip_tests_run,             \
               trip_tests_failed);                                          \
        return trip_tests_failed == 0 ? 0 : 1;                              \
    } while (0)

// ── Assertions ──────────────────────────────────────────────────────────
// Each assertion reports failures inline (test file + line) but does NOT
// abort the test function, so one test can report multiple problems at once.

#define ASSERT_TRUE(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            trip_asserts_failed_in_current++;                               \
            printf("  [FAIL] %s: ASSERT_TRUE(%s) failed at %s:%d\n",        \
                   trip_current_test_name, #cond, __FILE__, __LINE__);      \
        }                                                                     \
    } while (0)

#define ASSERT_EQ_INT(expected, actual)                                      \
    do {                                                                     \
        long long trip_e = (long long)(expected);                          \
        long long trip_a = (long long)(actual);                            \
        if (trip_e != trip_a) {                                             \
            trip_asserts_failed_in_current++;                               \
            printf("  [FAIL] %s: expected %s == %lld, got %lld at %s:%d\n", \
                   trip_current_test_name, #actual, trip_e, trip_a,         \
                   __FILE__, __LINE__);                                     \
        }                                                                    \
    } while (0)

// Compares a token's lexeme (Token.start/.length pair) against a C string.
#define ASSERT_TOKEN_TEXT(expected_cstr, tok)                                \
    do {                                                                     \
        const char* trip_exp = (expected_cstr);                            \
        size_t trip_exp_len = strlen(trip_exp);                            \
        if ((size_t)(tok).length != trip_exp_len ||                        \
            strncmp((tok).start, trip_exp, trip_exp_len) != 0) {           \
            trip_asserts_failed_in_current++;                               \
            printf("  [FAIL] %s: expected token text \"%s\", got \"%.*s\" " \
                   "at %s:%d\n",                                            \
                   trip_current_test_name, trip_exp, (tok).length,          \
                   (tok).start, __FILE__, __LINE__);                        \
        }                                                                    \
    } while (0)

#endif // TRIP_TEST_H
