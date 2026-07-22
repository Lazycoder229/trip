// tests/test_compiler.c
//
// Unit tests for src/compiler.c: compile(source, chunk) -> bool.
//
// These check the ACTUAL emitted bytecode (chunk.code[]), traced against
// compiler.c's own codegen (letDeclaration/statement/emitConstant/
// emitShort), not just "did it return true". A regression that changes
// operand width, opcode order, or constant indexing will show up here
// even if the language still happens to run correctly end to end.

#include "test.h"
#include "test_support.h"
#include "../src/compiler/compiler.h"
#include "../src/compiler/compiler.c"
#include "../src/chunk.h"
#include "../src/value.h"
#include "../src/object.h"
#include <string.h>

TEST(compile_returns_true_for_valid_source) {
    resetTestVM();
    Chunk chunk;
    initChunk(&chunk);

    bool ok = compile("let x = 1\n", &chunk);
    ASSERT_TRUE(ok);

    freeChunk(&chunk);
}

TEST(compile_returns_false_on_syntax_error) {
    resetTestVM();
    Chunk chunk;
    initChunk(&chunk);

    // Missing variable name after 'let' -> errorAt() sets parser.hadError.
    bool ok = compile("let = 1\n", &chunk);
    ASSERT_TRUE(!ok);

    freeChunk(&chunk);
}

TEST(top_level_let_emits_constant_then_define_let_then_return) {
    // "let x = 1" at top level (scopeDepth == 0), per letDeclaration():
    //   expression()        -> emitConstant(1.0)   => OP_CONSTANT, hi, lo
    //   emitByte(OP_DEFINE_LET); emitShort(nameIdx) => OP_DEFINE_LET, hi, lo
    // then compile()'s trailing emitReturn() => OP_RETURN
    resetTestVM();
    Chunk chunk;
    initChunk(&chunk);

    bool ok = compile("let x = 1\n", &chunk);
    ASSERT_TRUE(ok);

    ASSERT_EQ_INT(7, chunk.count);
    ASSERT_EQ_INT(OP_CONSTANT,    chunk.code[0]);
    ASSERT_EQ_INT(0,              chunk.code[1]); // constant index 0, high byte
    ASSERT_EQ_INT(0,              chunk.code[2]); // constant index 0, low byte
    ASSERT_EQ_INT(OP_DEFINE_LET,  chunk.code[3]);
    ASSERT_EQ_INT(0,              chunk.code[4]); // name constant index 1, high byte
    ASSERT_EQ_INT(1,              chunk.code[5]); // name constant index 1, low byte
    ASSERT_EQ_INT(OP_RETURN,      chunk.code[6]);

    // The two constants added were, in order: 1.0 (the value) then "x"
    // (the identifier name used by OP_DEFINE_LET).
    ASSERT_EQ_INT(2, chunk.constants.count);
    ASSERT_TRUE(IS_NUMBER(chunk.constants.values[0]));
    ASSERT_TRUE(AS_NUMBER(chunk.constants.values[0]) == 1);
    ASSERT_TRUE(IS_STRING(chunk.constants.values[1]));
    ASSERT_TRUE(strcmp(AS_STRING(chunk.constants.values[1])->chars, "x") == 0);

    freeChunk(&chunk);
}

TEST(bare_expression_statement_emits_op_add_then_pops_result) {
    // "1 + 2" as a bare top-level expression statement, per statement()'s
    // fallthrough branch: expression(); then OP_POP (since it's not void).
    resetTestVM();
    Chunk chunk;
    initChunk(&chunk);

    bool ok = compile("1 + 2\n", &chunk);
    ASSERT_TRUE(ok);

    // OP_CONSTANT(1), OP_CONSTANT(2), OP_ADD, OP_POP, OP_RETURN
    ASSERT_EQ_INT(OP_CONSTANT, chunk.code[0]);
    ASSERT_EQ_INT(OP_CONSTANT, chunk.code[3]);
    ASSERT_EQ_INT(OP_ADD,      chunk.code[6]);
    ASSERT_EQ_INT(OP_POP,      chunk.code[7]);
    ASSERT_EQ_INT(OP_RETURN,   chunk.code[8]);
    ASSERT_EQ_INT(9, chunk.count);

    freeChunk(&chunk);
}

TEST(empty_program_compiles_to_just_a_return) {
    resetTestVM();
    Chunk chunk;
    initChunk(&chunk);

    bool ok = compile("", &chunk);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(1, chunk.count);
    ASSERT_EQ_INT(OP_RETURN, chunk.code[0]);

    freeChunk(&chunk);
}

int main(void) {
    printf("compiler tests\n");
    RUN_TEST(compile_returns_true_for_valid_source);
    RUN_TEST(compile_returns_false_on_syntax_error);
    RUN_TEST(top_level_let_emits_constant_then_define_let_then_return);
    RUN_TEST(bare_expression_statement_emits_op_add_then_pops_result);
    RUN_TEST(empty_program_compiles_to_just_a_return);
    TEST_SUMMARY();
}
