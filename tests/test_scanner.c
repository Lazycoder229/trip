// tests/test_scanner.c
//
// Unit tests for src/scanner.c (the tokenizer).
//
// Why scanner first: it's the only module in this codebase with zero
// dependency on vm.h / value.h — everything else (table.c, memory.c,
// compiler.c, object.c) pulls in `extern VM vm;` and needs the real
// VM linked in to even compile. See the note at the bottom of this file
// for what's needed to unlock tests for those modules too.

#include "test.h"
#include "../src/scanner.h"

// ── helpers ───────────────────────────────────────────────────────────────

// Scans `source` and asserts the token kinds match `expected[0..count)`,
// in order. Keeps individual tests short and readable.
static void assertTokenSequence(const char* source, const TokenKind* expected, int count) {
    initScanner(source);
    for (int i = 0; i < count; i++) {
        Token t = scanToken();
        ASSERT_EQ_INT(expected[i], t.type);
    }
}

// ── single-character tokens ─────────────────────────────────────────────

TEST(single_char_tokens) {
    TokenKind expected[] = {
        TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
        TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
        TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
        TOKEN_COLON, TOKEN_COMMA,
        TOKEN_EOF
    };
    assertTokenSequence("()[]{}:,", expected, 9);
}

// ── operators, including multi-char lookalikes ──────────────────────────

TEST(arithmetic_operators) {
    TokenKind expected[] = {
        TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
        TOKEN_EOF
    };
    assertTokenSequence("+ - * / %", expected, 6);
}

TEST(comparison_operators_are_not_confused_with_assignment) {
    // Regression-style test: '==' must not scan as '=' followed by '='.
    TokenKind expected[] = {
        TOKEN_EQUAL, TOKEN_EQUAL_EQUAL, TOKEN_BANG_EQUAL,
        TOKEN_LESS, TOKEN_LESS_EQUAL, TOKEN_GREATER, TOKEN_GREATER_EQUAL,
        TOKEN_EOF
    };
    assertTokenSequence("= == != < <= > >=", expected, 8);
}

TEST(compound_assignment_and_postfix_operators) {
    TokenKind expected[] = {
        TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL,
        TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,
        TOKEN_EOF
    };
    assertTokenSequence("+= -= *= /= ++ --", expected, 7);
}

// ── literals ──────────────────────────────────────────────────────────────

TEST(integer_and_float_literals) {
    initScanner("42 3.14");
    Token a = scanToken();
    ASSERT_EQ_INT(TOKEN_NUMBER, a.type);
    ASSERT_TOKEN_TEXT("42", a);

    Token b = scanToken();
    ASSERT_EQ_INT(TOKEN_NUMBER, b.type);
    ASSERT_TOKEN_TEXT("3.14", b);

    ASSERT_EQ_INT(TOKEN_EOF, scanToken().type);
}

TEST(string_literal_token_includes_surrounding_quotes) {
    // makeToken() spans from scanner.start (set before the opening '"' is
    // consumed) to scanner.current (after the closing '"'), so the raw
    // lexeme includes both quote characters — stripping them is the
    // compiler/VM's job, not the scanner's.
    initScanner("\"hello world\"");
    Token t = scanToken();
    ASSERT_EQ_INT(TOKEN_STRING, t.type);
    ASSERT_TOKEN_TEXT("\"hello world\"", t);
}

TEST(unterminated_string_is_an_error_token) {
    initScanner("\"never closed");
    Token t = scanToken();
    ASSERT_EQ_INT(TOKEN_ERROR, t.type);
}

// ── identifiers vs. keywords ──────────────────────────────────────────────

TEST(keywords_are_recognized) {
    TokenKind expected[] = {
        TOKEN_LET, TOKEN_FN, TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR,
        TOKEN_IN, TOKEN_RETURN, TOKEN_CLASS, TOKEN_SELF, TOKEN_TRUE,
        TOKEN_FALSE, TOKEN_NIL, TOKEN_AND, TOKEN_OR, TOKEN_NOT,
        TOKEN_EOF
    };
    assertTokenSequence(
        "let fn if else while for in return class self true false nil and or not",
        expected, 17);
}

TEST(identifiers_that_merely_start_with_a_keyword_are_not_keywords) {
    // "letters" starts with "let" but must scan as one identifier, not
    // TOKEN_LET followed by garbage.
    initScanner("letters fnord classy selfie");
    for (int i = 0; i < 4; i++) {
        Token t = scanToken();
        ASSERT_EQ_INT(TOKEN_IDENTIFIER, t.type);
    }
}

// ── comments ──────────────────────────────────────────────────────────────

TEST(line_comments_are_skipped) {
    initScanner("let x = 1 # this is a comment\nlet y = 2");
    TokenKind expected[] = {
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_EQUAL, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_EQUAL, TOKEN_NUMBER,
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        Token t = scanToken();
        ASSERT_EQ_INT(expected[i], t.type);
    }
}

// ── Python-style indentation (INDENT/DEDENT) ─────────────────────────────

TEST(indent_and_dedent_around_a_simple_block) {
    // if true
    //     let x = 1
    // let y = 2
    const char* src =
        "if true\n"
        "    let x = 1\n"
        "let y = 2\n";

    initScanner(src);
    TokenKind expected[] = {
        TOKEN_IF, TOKEN_TRUE, TOKEN_NEWLINE,
        TOKEN_INDENT,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_EQUAL, TOKEN_NUMBER, TOKEN_NEWLINE,
        TOKEN_DEDENT,
        TOKEN_LET, TOKEN_IDENTIFIER, TOKEN_EQUAL, TOKEN_NUMBER,
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        Token t = scanToken();
        ASSERT_EQ_INT(expected[i], t.type);
    }
}

TEST(inconsistent_dedent_is_an_error) {
    // Indents to 4 spaces, then tries to dedent to 2 spaces, which doesn't
    // match any level on the indent stack (0 or 4).
    const char* src =
        "if true\n"
        "    let x = 1\n"
        "  let y = 2\n";
    initScanner(src);

    ASSERT_EQ_INT(TOKEN_IF, scanToken().type);
    ASSERT_EQ_INT(TOKEN_TRUE, scanToken().type);
    ASSERT_EQ_INT(TOKEN_NEWLINE, scanToken().type);
    ASSERT_EQ_INT(TOKEN_INDENT, scanToken().type);
    ASSERT_EQ_INT(TOKEN_LET, scanToken().type);
    ASSERT_EQ_INT(TOKEN_IDENTIFIER, scanToken().type);
    ASSERT_EQ_INT(TOKEN_EQUAL, scanToken().type);
    ASSERT_EQ_INT(TOKEN_NUMBER, scanToken().type);
    ASSERT_EQ_INT(TOKEN_NEWLINE, scanToken().type);
    ASSERT_EQ_INT(TOKEN_ERROR, scanToken().type); // "Inconsistent indentation."
}

// ── bracket-aware line wrapping ───────────────────────────────────────────

TEST(newlines_inside_brackets_do_not_produce_newline_tokens) {
    // A call/list argument list that wraps across physical lines is still
    // one logical line — no TOKEN_NEWLINE, no bogus INDENT/DEDENT.
    initScanner("foo(1,\n    2,\n    3)\n");
    TokenKind expected[] = {
        TOKEN_IDENTIFIER, TOKEN_LEFT_PAREN,
        TOKEN_NUMBER, TOKEN_COMMA,
        TOKEN_NUMBER, TOKEN_COMMA,
        TOKEN_NUMBER, TOKEN_RIGHT_PAREN,
        TOKEN_NEWLINE, TOKEN_EOF
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        Token t = scanToken();
        ASSERT_EQ_INT(expected[i], t.type);
    }
}

// ── entry point ───────────────────────────────────────────────────────────

int main(void) {
    printf("scanner tests\n");
    RUN_TEST(single_char_tokens);
    RUN_TEST(arithmetic_operators);
    RUN_TEST(comparison_operators_are_not_confused_with_assignment);
    RUN_TEST(compound_assignment_and_postfix_operators);
    RUN_TEST(integer_and_float_literals);
    RUN_TEST(string_literal_token_includes_surrounding_quotes);
    RUN_TEST(unterminated_string_is_an_error_token);
    RUN_TEST(keywords_are_recognized);
    RUN_TEST(identifiers_that_merely_start_with_a_keyword_are_not_keywords);
    RUN_TEST(line_comments_are_skipped);
    RUN_TEST(indent_and_dedent_around_a_simple_block);
    RUN_TEST(inconsistent_dedent_is_an_error);
    RUN_TEST(newlines_inside_brackets_do_not_produce_newline_tokens);
    TEST_SUMMARY();
}

// ── Note on table.c / memory.c / compiler.c ──────────────────────────────
//
// Those three all depend on symbols this zip doesn't include:
//   - value.h / value.c        (the Value type — table.h and object.h both
//                                #include "value.h")
//   - vm.h                     (memory.c, compiler.c both #include "vm.h"
//                                and reference `extern VM vm;`)
//   - src/tvm/*.c              (the VM itself — referenced by the Makefile's
//                                SRCS list, but not present in src.zip)
//
// Once those are added, the same pattern used here extends directly:
//   tests/test_table.c   -> #include "../src/table.h", link table.c +
//                            object.c + memory.c + vm.c (+ value.c)
//   tests/test_memory.c  -> same link set; test reallocate()'s grow/shrink/
//                            free behavior directly (sizes are easy to
//                            assert on without needing a full GC cycle)
//   tests/test_compiler.c -> compile small snippets with compileSource()-
//                            style calls and assert on the resulting
//                            Chunk's opcodes via getLine()/chunk.code[]
//
// If you can upload value.h/value.c, vm.h, and the src/tvm/ files, I can
// write those next.
