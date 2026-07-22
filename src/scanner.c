#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "scanner.h"

Scanner scanner;

void initScanner(const char* source) {
    scanner.start = source;
    scanner.current = source;
    scanner.line = 1;

    scanner.indentStack[0] = 0;
    scanner.indentTop = 0;
    scanner.pendingDedents = 0;
    scanner.atLineStart = true; // the very first line's indentation must be measured too
    scanner.bracketDepth = 0;
}

// Helper functions to navigate the string
static bool isAtEnd() {
    return *scanner.current == '\0';
}

static char advance() {
    scanner.current++;
    return scanner.current[-1];
}

static char peek() {
    return *scanner.current;
}

static char peekNext() {
    if (isAtEnd()) return '\0';
    return scanner.current[1];
}

static bool matchChar(char expected) {
    if (isAtEnd()) return false;
    if (*scanner.current != expected) return false;
    scanner.current++;
    return true;
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isAlphanumeric(char c) {
    return isAlpha(c) || isDigit(c);
}

// Token creation functions
static Token makeToken(TokenKind type) {
    Token token;
    token.type = type;
    token.start = scanner.start;
    token.length = (int)(scanner.current - scanner.start);
    token.line = scanner.line;
    return token;
}

static Token errorToken(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = scanner.line;
    return token;
}

// Skips spaces, tabs, carriage returns, and comments — everything that
// has no meaning to the parser. Stops at a real newline, a real token,
// or EOF.
//
// Two comment styles:
//   # ...to end of line
//   /* ... */  (can span multiple lines)
//
// If `indent` is non-NULL, every space/tab character skipped is counted
// into it — this lets measureIndent() reuse this same function to count
// indentation while still ignoring comments along the way.
//
// Returns false only if a `/* ... */` comment is never closed before EOF.
static bool skipInsignificant(int* indent) {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\t':
                if (indent) (*indent)++;
                advance();
                break;

            case '\r':
                advance();
                break;

            case '#':
                while (!isAtEnd() && peek() != '\n') advance();
                break;

            case '/':
                if (peekNext() == '*') {
                    advance(); advance(); // consume "/*"
                    while (!isAtEnd() && !(peek() == '*' && peekNext() == '/')) {
                        if (peek() == '\n') scanner.line++;
                        advance();
                    }
                    if (isAtEnd()) return false; // never found the closing */
                    advance(); advance();         // consume "*/"
                    break;
                }
                return true; // a real '/' (division) — not a comment, stop here

            default:
                return true;
        }
    }
}
static Token number() {
    while (isDigit(peek())) advance();

    // Look for a fractional part (like 1.2)
    if (peek() == '.' && isDigit(scanner.current[1])) {
        // Consume the "."
        advance();
        while (isDigit(peek())) advance();
    }

    return makeToken(TOKEN_NUMBER);
}

static TokenKind checkKeyword(const char* word, TokenKind type) {
    int len = (int)(scanner.current - scanner.start);
    int wordLen = (int)strlen(word);
    if (len == wordLen && memcmp(scanner.start, word, len) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenKind identifierType() {
    switch (scanner.start[0]) {
        case 'n':
            if (scanner.current - scanner.start > 1) {
                if (checkKeyword("not", TOKEN_NOT) != TOKEN_IDENTIFIER) return TOKEN_NOT;
            }
            return checkKeyword("nil", TOKEN_NIL);
       case 't':
    if (scanner.current - scanner.start > 1) {
        if (checkKeyword("try", TOKEN_TRY) != TOKEN_IDENTIFIER) return TOKEN_TRY;
        if (checkKeyword("throw", TOKEN_THROW) != TOKEN_IDENTIFIER) return TOKEN_THROW;
        if (checkKeyword("typeof", TOKEN_TYPEOF) != TOKEN_IDENTIFIER) return TOKEN_TYPEOF;
    }
    return checkKeyword("true", TOKEN_TRUE);
        case 'f':
            if (scanner.current - scanner.start == 2 && scanner.start[1] == 'n') return TOKEN_FN;
            if (scanner.current - scanner.start > 1) {
                if (checkKeyword("for", TOKEN_FOR) != TOKEN_IDENTIFIER) return TOKEN_FOR;
                if (checkKeyword("finally", TOKEN_FINALLY) != TOKEN_IDENTIFIER) return TOKEN_FINALLY;
            }
            return checkKeyword("false", TOKEN_FALSE);
        case 'l':
            return checkKeyword("let", TOKEN_LET);
        case 'c':
            if (scanner.current - scanner.start > 1) {
                if (checkKeyword("continue", TOKEN_CONTINUE) != TOKEN_IDENTIFIER) {
                    return TOKEN_CONTINUE;
                }
                if (checkKeyword("const", TOKEN_CONST) != TOKEN_IDENTIFIER) {
                    return TOKEN_CONST;
                }
                if (checkKeyword("case", TOKEN_CASE) != TOKEN_IDENTIFIER) {
                    return TOKEN_CASE;
                }
                if (checkKeyword("catch", TOKEN_CATCH) != TOKEN_IDENTIFIER) {
                    return TOKEN_CATCH;
                }
                if (checkKeyword("class", TOKEN_CLASS) != TOKEN_IDENTIFIER) {
                    return TOKEN_CLASS;
                }
            }
            return TOKEN_IDENTIFIER;
        case 'a':
            if (scanner.current - scanner.start > 1) {
                if (checkKeyword("async", TOKEN_ASYNC) != TOKEN_IDENTIFIER) return TOKEN_ASYNC;
                if (checkKeyword("await", TOKEN_AWAIT) != TOKEN_IDENTIFIER) return TOKEN_AWAIT;
            }
            return checkKeyword("and", TOKEN_AND);
        case 'o':
            return checkKeyword("or", TOKEN_OR);
        case 'i':
            if (scanner.current - scanner.start == 2 &&
                scanner.start[1] == 'n') return TOKEN_IN;
            if (checkKeyword("import", TOKEN_IMPORT) != TOKEN_IDENTIFIER) return TOKEN_IMPORT;
            return checkKeyword("if", TOKEN_IF);
        case 'e':
            if (scanner.current - scanner.start > 2) {
                if (checkKeyword("elif", TOKEN_ELIF) != TOKEN_IDENTIFIER) return TOKEN_ELIF;
            }
            return checkKeyword("else", TOKEN_ELSE);
        case 'r':
            return checkKeyword("return", TOKEN_RETURN);
        case 's':
            if (scanner.current - scanner.start > 1) {
                if (checkKeyword("spawn", TOKEN_SPAWN) != TOKEN_IDENTIFIER) return TOKEN_SPAWN;
            }
            return checkKeyword("self", TOKEN_SELF);
        case 'w':
            return checkKeyword("while", TOKEN_WHILE);
        case 'b':
            return checkKeyword("break", TOKEN_BREAK);
        case 'm':
            return checkKeyword("match", TOKEN_MATCH);
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier() {
    while (isAlphanumeric(peek())) advance();
    return makeToken(identifierType());
}

static Token fstring() {
    int braceDepth = 0;
    while (!isAtEnd()) {
        char c = peek();

        if (braceDepth == 0 && c == '"') break; // real closing quote

        if (braceDepth > 0 && c == '"') {
            // Nested string literal inside {...} — skip it whole.
            advance();
            while (!isAtEnd() && peek() != '"') {
                if (peek() == '\\') advance();
                advance();
            }
            if (!isAtEnd()) advance();
            continue;
        }

        if (c == '{') { braceDepth++; advance(); continue; }
        if (c == '}') { if (braceDepth > 0) braceDepth--; advance(); continue; }

        if (c == '\\') advance();
        advance();
    }

    if (isAtEnd()) {
        return errorToken("Unterminated f-string.");
    }

    advance(); // consume closing '"'
    return makeToken(TOKEN_FSTRING);
}

static Token string() {
    // The opening '"' was already consumed by scanToken()'s advance().
    // Scan until closing '"' or EOF.
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance(); // skip the escape character
        }
        advance();
    }

    if (isAtEnd()) {
        return errorToken("Unterminated string.");
    }

    advance(); // consume closing '"'
    return makeToken(TOKEN_STRING);
}

// ── Triple-quoted multi-line string  """...""" ────────────────────────────
// Called after the opening three '"' have been consumed.
// Scans until the closing '"""', tracking newlines for the line counter.
// The token spans from the first character after the opening quotes to
// just before the closing quotes; copyStringWithEscapes() / the compiler's
// multilineString() handler will strip the leading newline and dedent.
static Token multilineString() {
    while (!isAtEnd()) {
        if (peek() == '"' && scanner.current[1] == '"' && scanner.current[2] == '"') {
            // Consume all three closing quote characters.
            advance(); advance(); advance();
            return makeToken(TOKEN_MULTILINE_STRING);
        }
        if (peek() == '\n') scanner.line++;
        advance();
    }
    return errorToken("Unterminated triple-quoted string.");
}

// ── Raw string  r"..." ────────────────────────────────────────────────────
// Called after the opening 'r' and '"' have been consumed.
// Backslashes are NOT treated as escape characters — they are literal.
// The only thing that ends the string is a bare '"' (not preceded by '\').
static Token rawString() {
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\n') scanner.line++; // raw strings can span lines too
        advance();
    }
    if (isAtEnd()) return errorToken("Unterminated raw string.");
    advance(); // consume closing '"'
    return makeToken(TOKEN_RAW_STRING);
}

// Measures the indentation of a fresh logical line and turns it into
// INDENT / DEDENT tokens, the same way Python's tokenizer does. Blank
// lines (whitespace only) are skipped entirely — they never affect
// block structure.
static Token measureIndent() {
    for (;;) {
        int indent = 0;
        if (!skipInsignificant(&indent)) {
            return errorToken("Unterminated block comment.");
        }
        if (peek() == '\r') advance(); // tolerate CRLF line endings

        if (peek() == '\n') {          // blank (or comment-only) line: try the next one
            advance();
            scanner.line++;
            continue;
        }

        if (peek() == '\0') {          // file ends right after blank lines
            if (scanner.indentTop > 0) {
                scanner.pendingDedents = scanner.indentTop - 1;
                scanner.indentTop = 0;
                return makeToken(TOKEN_DEDENT);
            }
            return makeToken(TOKEN_EOF);
        }

        scanner.start = scanner.current; // don't include the indentation in the next token

        if (indent > scanner.indentStack[scanner.indentTop]) {
            if (scanner.indentTop + 1 >= MAX_INDENT_LEVELS) {
                return errorToken("Too many indentation levels.");
            }
            scanner.indentTop++;
            scanner.indentStack[scanner.indentTop] = indent;
            scanner.atLineStart = false;
            return makeToken(TOKEN_INDENT);
        }

        if (indent < scanner.indentStack[scanner.indentTop]) {
            int dedents = 0;
            while (scanner.indentTop > 0 && scanner.indentStack[scanner.indentTop] > indent) {
                scanner.indentTop--;
                dedents++;
            }
            if (scanner.indentStack[scanner.indentTop] != indent) {
                return errorToken("Inconsistent indentation.");
            }
            scanner.pendingDedents = dedents - 1;
            scanner.atLineStart = false;
            return makeToken(TOKEN_DEDENT);
        }

        // Same indentation as before: no INDENT/DEDENT needed, scan normally.
        scanner.atLineStart = false;
        return scanToken();
    }
}

// The main function that our future compiler will call
Token scanToken() {
    // A dedent that drops more than one level owes the parser several
    // DEDENT tokens in a row, one per call.
    if (scanner.pendingDedents > 0) {
        scanner.pendingDedents--;
        return makeToken(TOKEN_DEDENT);
    }

    if (scanner.atLineStart) {
        return measureIndent();
    }

    if (!skipInsignificant(NULL)) {
        return errorToken("Unterminated block comment.");
    }
    scanner.start = scanner.current;

    if (isAtEnd()) {
        // Reached EOF mid-line (no trailing newline on the last statement):
        // still owe DEDENT tokens for every block left open.
        if (scanner.indentTop > 0) {
            scanner.pendingDedents = scanner.indentTop - 1;
            scanner.indentTop = 0;
            return makeToken(TOKEN_DEDENT);
        }
        return makeToken(TOKEN_EOF);
    }

    char c = advance();

    if (c == '\n') {
        scanner.line++;
        if (scanner.bracketDepth > 0) {
            // Inside an unclosed ( [ { — this is a line-wrap, not a new
            // logical line. Don't emit TOKEN_NEWLINE and don't measure
            // indentation for the continuation line; just keep scanning.
            scanner.start = scanner.current;
            return scanToken();
        }
        scanner.atLineStart = true; // next call must measure the new line's indentation
        return makeToken(TOKEN_NEWLINE);
    }

    if (c == 'f' && peek() == '"') {
        advance(); // consume the opening quote
        return fstring();
    }
    // Triple-quoted multi-line string: """..."""
    if (c == '"' && peek() == '"' && peekNext() == '"') {
        advance(); advance(); // consume the second and third '"'
        return multilineString();
    }
    // Raw string: r"..."
    if (c == 'r' && peek() == '"') {
        advance(); // consume the opening '"'
        return rawString();
    }
    if (isDigit(c)) return number();
    if (isAlpha(c)) return identifier();
    if (c == '"') return string();

    // char literal 'x' or escaped 'x' like '\n'
    if (c == '\'') {
        if (!isAtEnd()) {
            if (peek() == '\\' && peekNext() != '\0') {
                advance(); // the backslash
                advance(); // the escaped character
            } else {
                advance(); // the char itself
            }
            if (!isAtEnd() && *scanner.current == '\'') {
                advance(); // closing quote
                return makeToken(TOKEN_CHAR_LIT);
            }
        }
        return errorToken("Unterminated char literal.");
    }

    switch (c) {
        case '(': scanner.bracketDepth++; return makeToken(TOKEN_LEFT_PAREN);
        case ')': if (scanner.bracketDepth > 0) scanner.bracketDepth--; return makeToken(TOKEN_RIGHT_PAREN);
        case '[': scanner.bracketDepth++; return makeToken(TOKEN_LEFT_BRACKET);
        case ']': if (scanner.bracketDepth > 0) scanner.bracketDepth--; return makeToken(TOKEN_RIGHT_BRACKET);
        case '{': scanner.bracketDepth++; return makeToken(TOKEN_LEFT_BRACE);
        case '}': if (scanner.bracketDepth > 0) scanner.bracketDepth--; return makeToken(TOKEN_RIGHT_BRACE);
        case ':': return makeToken(TOKEN_COLON);
        case ',': return makeToken(TOKEN_COMMA);
        case '%': return makeToken(TOKEN_PERCENT);
        case '+':
            if (matchChar('+')) return makeToken(TOKEN_PLUS_PLUS);
            if (matchChar('=')) return makeToken(TOKEN_PLUS_EQUAL);
            return makeToken(TOKEN_PLUS);
        case '-':
            if (matchChar('-')) return makeToken(TOKEN_MINUS_MINUS);
            if (matchChar('=')) return makeToken(TOKEN_MINUS_EQUAL);
            return makeToken(TOKEN_MINUS);
        case '*': return makeToken(matchChar('=') ? TOKEN_STAR_EQUAL  : TOKEN_STAR);
        case '/': return makeToken(matchChar('=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
        case '.':
    if (matchChar('.')) {
        if (matchChar('.')) return makeToken(TOKEN_ELLIPSIS);
        return makeToken(TOKEN_DOTDOT);
    }
    return makeToken(TOKEN_DOT);
        case '=': return makeToken(matchChar('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '!': return makeToken(matchChar('=') ? TOKEN_BANG_EQUAL  : TOKEN_BANG);
        case '<': return makeToken(matchChar('=') ? TOKEN_LESS_EQUAL  : TOKEN_LESS);
        case '>': return makeToken(matchChar('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        
    }

    return errorToken("Unexpected character.");
}