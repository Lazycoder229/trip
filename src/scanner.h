#ifndef scanner_h
#define scanner_h

#include <stdbool.h>

#define MAX_INDENT_LEVELS 64

typedef enum {
    // Single-character tokens
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACKET,  // [
    TOKEN_RIGHT_BRACKET, // ]
    TOKEN_LEFT_BRACE,    // {
    TOKEN_RIGHT_BRACE,   // }
    TOKEN_COLON,         // :
    TOKEN_COMMA,         // ,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
    TOKEN_PERCENT,       // %
    TOKEN_EQUAL,

    // Comparison operators
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_BANG,

    // Compound assignment
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,

    // Postfix
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,

    TOKEN_NEWLINE,
    TOKEN_INDENT,
    TOKEN_DEDENT,

    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_FSTRING,           // f"...{expr}..."
    TOKEN_MULTILINE_STRING,  // """..."""  (triple-quoted, dedented)
    TOKEN_RAW_STRING,        // r"..."     (no escape processing)
    TOKEN_NUMBER,
    TOKEN_CHAR_LIT,   // 'a'

    // Keywords
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NIL,
    TOKEN_LET,
    TOKEN_CONST,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_TYPEOF,
    TOKEN_ELIF,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_DOTDOT, 
    TOKEN_ELLIPSIS,   // ..
    TOKEN_FN,
    TOKEN_RETURN,
    TOKEN_MATCH,
    TOKEN_CASE,
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_THROW,
    TOKEN_FINALLY,
    TOKEN_IMPORT,
    TOKEN_DOT,       // .  (method call)
    TOKEN_CLASS,
    TOKEN_SELF,
    TOKEN_SPAWN,
    TOKEN_ASYNC,
    TOKEN_AWAIT,
    TOKEN_ERROR,
    TOKEN_EOF
} TokenKind;

typedef struct {
    TokenKind   type;
    const char* start;
    int         length;
    int         line;
} Token;

// The state of our scanner. Exposed (not just forward-declared) so
// compiler.c can save and restore it wholesale — needed to temporarily
// point the scanner at an isolated sub-string when compiling the
// `{expr}` parts of an f-string, then resume exactly where the outer
// scan left off.
typedef struct {
    const char* start;
    const char* current;
    int line;

    // Python-style indentation tracking.
    int indentStack[MAX_INDENT_LEVELS]; // indentStack[0] is always 0 (top level)
    int indentTop;                      // index of the current indentation level
    int pendingDedents;                 // extra DEDENT tokens still owed to the parser
    bool atLineStart;                   // true when we still need to measure indentation

    // Depth of unclosed ( [ { at the current scan position. While this is
    // > 0 we're in the middle of a multi-line call/list/dict literal that
    // just happens to wrap onto a new physical line — that wrap is NOT a
    // new logical line, so newlines are swallowed instead of becoming
    // TOKEN_NEWLINE, and indentation is never measured for them. Without
    // this, a line-wrapped list/dict/call argument list makes the scanner
    // think a new logical line started, which can push a bogus INDENT and
    // later owe a stray DEDENT that terminates the enclosing block early.
    int bracketDepth;
} Scanner;

extern Scanner scanner;

void  initScanner(const char* source);
Token scanToken();

#endif