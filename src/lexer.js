// ─── Token Types ───────────────────────────────────────────────────────────
const TT = {
  // Literals
  NUMBER: 'NUMBER', STRING: 'STRING', BOOL: 'BOOL', NULL: 'NULL',
  IDENT:  'IDENT',

  // Keywords
  LET: 'LET', FN: 'FN', RETURN: 'RETURN',
  IF: 'IF', ELSE: 'ELSE', ELIF: 'ELIF',
  WHILE: 'WHILE', FOR: 'FOR', IN: 'IN',
  CLASS: 'CLASS', SELF: 'SELF', NEW: 'NEW',
  IMPORT: 'IMPORT', BREAK: 'BREAK', CONTINUE: 'CONTINUE',

  // Operators
  PLUS: '+', MINUS: '-', STAR: '*', SLASH: '/', PERCENT: '%',
  EQ: '==', NEQ: '!=', LT: '<', GT: '>', LTE: '<=', GTE: '>=',
  STAR_EQ: '*=', SLASH_EQ: '/=',
  AND: 'AND', OR: 'OR', NOT: 'NOT',
  ASSIGN: '=', PLUS_EQ: '+=', MINUS_EQ: '-=',

  // Delimiters
  LPAREN: '(', RPAREN: ')', LBRACE: '{', RBRACE: '}',
  LBRACKET: '[', RBRACKET: ']',
  COMMA: ',', DOT: '.', COLON: ':', ARROW: '->',

  // Structure
  NEWLINE: 'NEWLINE', INDENT: 'INDENT', DEDENT: 'DEDENT',
  EOF: 'EOF',

   STATIC: 'STATIC', SUPER: 'SUPER',
  TRY: 'TRY', CATCH: 'CATCH', FINALLY: 'FINALLY', THROW: 'THROW',
}

const KEYWORDS = new Set([
  'let','fn','return','if','else','elif','while','for','in',
  'class','self','super','new','import','break','continue',
  'true','false','null','and','or','not',
  'static','try','catch','finally','throw',
])

class Token {
  constructor(type, value, line) {
    this.type  = type
    this.value = value
    this.line  = line
  }
  toString() { return `Token(${this.type}, ${JSON.stringify(this.value)})` }
}

// ─── Lexer ─────────────────────────────────────────────────────────────────
class Lexer {
  constructor(source) {
    this.source = source
    this.pos    = 0
    this.line   = 1
    this.tokens = []
    this.indentStack = [0]
  }

  error(msg) { throw new Error(`[Lexer] Line ${this.line}: ${msg}`) }
  peek(offset = 0) { return this.source[this.pos + offset] }
  advance() { return this.source[this.pos++] }
  at(ch) { return this.source[this.pos] === ch }

  tokenize() {
    const processedSource = this.processTripleQuotes(this.source)
    const lines = processedSource.split('\n')

    for (let lineNum = 0; lineNum < lines.length; lineNum++) {
      const raw  = lines[lineNum]
      this.line  = lineNum + 1

      // Skip blank lines and comment-only lines
      const trimmed = raw.trimEnd()
      if (trimmed.trim() === '' || trimmed.trim().startsWith('#')) continue

      // Measure indent
      let indent = 0
      while (indent < trimmed.length && trimmed[indent] === ' ') indent++

      const currentIndent = this.indentStack[this.indentStack.length - 1]

      if (indent > currentIndent) {
        this.indentStack.push(indent)
        this.tokens.push(new Token(TT.INDENT, indent, this.line))
      } else {
        while (indent < this.indentStack[this.indentStack.length - 1]) {
          this.indentStack.pop()
          this.tokens.push(new Token(TT.DEDENT, null, this.line))
        }
        if (indent !== this.indentStack[this.indentStack.length - 1]) {
          this.error(`Inconsistent indentation at line ${this.line}`)
        }
      }

      // Lex the content of this line
      this.pos = 0
      this.lineSource = trimmed.slice(indent)
      this.lexLine()

      this.tokens.push(new Token(TT.NEWLINE, null, this.line))
    }

    // Close remaining indents
    while (this.indentStack.length > 1) {
      this.indentStack.pop()
      this.tokens.push(new Token(TT.DEDENT, null, this.line))
    }

    this.tokens.push(new Token(TT.EOF, null, this.line))
    return this.tokens
  }
processTripleQuotes(source) {
  let result = ''
  let i = 0
  while (i < source.length) {
    // Detect opening """
    if (source[i] === '"' && source[i+1] === '"' && source[i+2] === '"') {
      i += 3 // skip opening """
      let str = ''
      while (i < source.length) {
        // Detect closing """
        if (source[i] === '"' && source[i+1] === '"' && source[i+2] === '"') {
          i += 3
          break
        }
        // Convert real newlines to \n escape so lexLine sees one flat string
        if (source[i] === '\n') {
          str += '\\n'
          i++
        } else if (source[i] === '"') {
          // single quote inside triple string — escape it
          str += '\\"'
          i++
        } else {
          str += source[i++]
        }
      }
      // Emit as a regular "..." token the line lexer already understands
      result += `"${str}"`
    } else {
      result += source[i++]
    }
  }
  return result
}
  lexLine() {
  const src = this.lineSource
  let i = 0

    while (i < src.length) {
      const ch = src[i]

      // Skip spaces and tabs within the line (after leading indent is stripped)
      if (ch === ' ' || ch === '\t') { i++; continue }

      // Comment
      if (ch === '#') break

      // String
      if (ch === '"' || ch === "'") {
        const quote = ch; i++
        let str = ''
        while (i < src.length && src[i] !== quote) {
          if (src[i] === '\\') {
            i++
            const esc = { n:'\n', t:'\t', r:'\r', '\\':'\\', "'":"'", '"':'"' }
            str += esc[src[i]] ?? src[i]
          } else {
            str += src[i]
          }
          i++
        }
        i++ // closing quote
        this.tokens.push(new Token(TT.STRING, str, this.line))
        continue
      }

      // Number
      if (/\d/.test(ch)) {
        let num = ''
        while (i < src.length && /[\d.]/.test(src[i])) num += src[i++]
        this.tokens.push(new Token(TT.NUMBER, parseFloat(num), this.line))
        continue
      }

      // Identifier or keyword
      if (/[a-zA-Z_]/.test(ch)) {
        let word = ''
        while (i < src.length && /[a-zA-Z0-9_]/.test(src[i])) word += src[i++]

        if (word === 'true' || word === 'false')
          this.tokens.push(new Token(TT.BOOL, word === 'true', this.line))
        else if (word === 'null')
          this.tokens.push(new Token(TT.NULL, null, this.line))
        else if (KEYWORDS.has(word))
          this.tokens.push(new Token(word.toUpperCase(), word, this.line))
        else
          this.tokens.push(new Token(TT.IDENT, word, this.line))
        continue
      }

      // Two-char operators
      const two = src[i] + src[i+1]
      const twoMap = {
        '==': TT.EQ, '!=': TT.NEQ, '<=': TT.LTE, '>=': TT.GTE,
        '->': TT.ARROW, '+=': TT.PLUS_EQ, '-=': TT.MINUS_EQ,
        '*=': TT.STAR_EQ, '/=': TT.SLASH_EQ,
      }
      if (twoMap[two]) {
        this.tokens.push(new Token(twoMap[two], two, this.line))
        i += 2; continue
      }

      // Single-char operators
      const oneMap = {
        '+': TT.PLUS, '-': TT.MINUS, '*': TT.STAR, '/': TT.SLASH,
        '%': TT.PERCENT, '<': TT.LT, '>': TT.GT, '=': TT.ASSIGN,
        '(': TT.LPAREN, ')': TT.RPAREN, '[': TT.LBRACKET, ']': TT.RBRACKET,
        '{': TT.LBRACE,  '}': TT.RBRACE, ',': TT.COMMA, '.': TT.DOT,
        ':': TT.COLON,
      }
      if (oneMap[ch]) {
        this.tokens.push(new Token(oneMap[ch], ch, this.line))
        i++; continue
      }

      this.error(`Unknown character: '${ch}'`)
    }
  }
}

module.exports = { Lexer, Token, TT, KEYWORDS }
