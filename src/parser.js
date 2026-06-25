const { TT } = require('./lexer')
const AST = require('./ast')

class Parser {
  constructor(tokens) {
    this.tokens = tokens
    this.pos    = 0
  }

  // ─── Helpers ─────────────────────────────────────────────────────────────
  peek()    { return this.tokens[this.pos] }
  prev()    { return this.tokens[this.pos - 1] }
  advance() { return this.tokens[this.pos++] }
  isEOF()   { return this.peek().type === TT.EOF }

  check(type)   { return this.peek().type === type }
  match(...types) {
    for (const t of types) {
      if (this.check(t)) { this.advance(); return true }
    }
    return false
  }
  expect(type, msg) {
    if (!this.check(type)) {
      const tok = this.peek()
      throw new Error(`[Parser] Line ${tok.line}: Expected ${msg}, got '${tok.value ?? tok.type}'`)
    }
    return this.advance()
  }
  skipNewlines() {
    while (this.check(TT.NEWLINE)) this.advance()
  }

  // ─── Entry ───────────────────────────────────────────────────────────────
  parse() {
    const body = []
    this.skipNewlines()
    while (!this.isEOF()) {
      body.push(this.parseStatement())
      this.skipNewlines()
    }
    return new AST.Program(body)
  }

  // ─── Block (indented body or single-line after colon) ───────────────────
  parseBlock() {
    this.expect(TT.COLON, "':' before block")

    // Single-line block: fn(x): return x * 2
    if (!this.check(TT.NEWLINE)) {
      const stmt = this.parseStatement()
      return new AST.Block([stmt])
    }

    this.expect(TT.NEWLINE, "newline after ':'")
    this.expect(TT.INDENT, 'indented block')
    const stmts = []
    this.skipNewlines()
    while (!this.check(TT.DEDENT) && !this.isEOF()) {
      stmts.push(this.parseStatement())
      this.skipNewlines()
    }
    this.match(TT.DEDENT)
    return new AST.Block(stmts)
  }

  // ─── Statements ──────────────────────────────────────────────────────────
  parseStatement() {
    const tok = this.peek()

    if (tok.type === TT.LET)      return this.parseLet()
    if (tok.type === TT.FN)       return this.parseFn()
    if (tok.type === TT.CLASS)    return this.parseClass()
    if (tok.type === TT.RETURN)   return this.parseReturn()
    if (tok.type === TT.IF)       return this.parseIf()
    if (tok.type === TT.WHILE)    return this.parseWhile()
    if (tok.type === TT.FOR)      return this.parseFor()
    if (tok.type === TT.BREAK)    { this.advance(); this.match(TT.NEWLINE); return new AST.BreakStatement() }
    if (tok.type === TT.CONTINUE) { this.advance(); this.match(TT.NEWLINE); return new AST.ContinueStatement() }
    if (tok.type === TT.IMPORT)   return this.parseImport()
    if (tok.type === TT.TRY)      return this.parseTryCatch()
    if (tok.type === TT.THROW)    return this.parseThrow()

    return this.parseExprStatement()
  }

  parseLet() {
    this.advance() // let
    const name = this.expect(TT.IDENT, 'variable name').value
    this.expect(TT.ASSIGN, "'='")
    const value = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.LetStatement(name, value)
  }

  parseFn() {
    this.advance() // fn
    const name = this.check(TT.IDENT) ? this.advance().value : null
    this.expect(TT.LPAREN, "'('")
    const params = this.parseParams()
    this.expect(TT.RPAREN, "')'")
    const body = this.parseBlock()
    return new AST.FnDeclaration(name, params, body)
  }

  parseParams() {
    const params = []
    while (!this.check(TT.RPAREN)) {
      // Accept both IDENT and SELF ('self') as parameter names
      if (this.check(TT.SELF)) {
        this.advance()
        params.push('self')
      } else {
        params.push(this.expect(TT.IDENT, 'parameter name').value)
      }
      if (!this.match(TT.COMMA)) break
    }
    return params
  }

  parseClass() {
    this.advance() // class
    const name = this.expect(TT.IDENT, 'class name').value
    let superclass = null
    if (this.match(TT.LPAREN)) {
      superclass = this.expect(TT.IDENT, 'superclass name').value
      this.expect(TT.RPAREN, "')'")
    }
    const body = this.parseClassBlock()
    return new AST.ClassDeclaration(name, superclass, body)
  }

  parseClassBlock() {
    this.expect(TT.COLON, "':' before class body")
    this.expect(TT.NEWLINE, "newline after ':'")
    this.expect(TT.INDENT, 'indented class body')
    const stmts = []
    this.skipNewlines()
    while (!this.check(TT.DEDENT) && !this.isEOF()) {
      // support 'static fn method(...):'
      if (this.check(TT.STATIC)) {
        this.advance()
        const fn = this.parseFn()
        fn.isStatic = true
        stmts.push(fn)
      } else {
        stmts.push(this.parseStatement())
      }
      this.skipNewlines()
    }
    this.match(TT.DEDENT)
    return new AST.Block(stmts)
  }

  parseReturn() {
    const line = this.peek().line
    this.advance() // return
    let value = null
    if (!this.check(TT.NEWLINE) && !this.check(TT.EOF)) {
      value = this.parseExpr()
    }
    this.match(TT.NEWLINE)
    return new AST.ReturnStatement(value)
  }

  parseIf() {
    this.advance() // if
    const condition  = this.parseExpr()
    const consequent = this.parseBlock()
    const alternates = []
    let final = null

    this.skipNewlines()
    while (this.check(TT.ELIF)) {
      this.advance()
      const elifCond = this.parseExpr()
      const elifBody = this.parseBlock()
      alternates.push({ condition: elifCond, body: elifBody })
      this.skipNewlines()
    }
    if (this.check(TT.ELSE)) {
      this.advance()
      final = this.parseBlock()
    }
    return new AST.IfStatement(condition, consequent, alternates, final)
  }

  parseWhile() {
    this.advance() // while
    const condition = this.parseExpr()
    const body = this.parseBlock()
    return new AST.WhileStatement(condition, body)
  }

  parseFor() {
    this.advance() // for
    const variable = this.expect(TT.IDENT, 'loop variable').value
    this.expect(TT.IN, "'in'")
    const iterable = this.parseExpr()
    const body = this.parseBlock()
    return new AST.ForStatement(variable, iterable, body)
  }

  parseImport() {
    this.advance() // import
    const path = this.expect(TT.STRING, 'module path').value
    this.match(TT.NEWLINE)
    return new AST.ImportStatement(path)
  }

  parseThrow() {
    this.advance() // throw
    const value = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.ThrowStatement(value)
  }

  parseTryCatch() {
    this.advance() // try
    const tryBlock = this.parseBlock()

    let catchVar   = null
    let catchBlock = null
    let finallyBlock = null

    this.skipNewlines()
    if (this.check(TT.CATCH)) {
      this.advance() // catch
      // optional: catch(e)
      if (this.check(TT.LPAREN)) {
        this.advance()
        catchVar = this.expect(TT.IDENT, 'error variable name').value
        this.expect(TT.RPAREN, "')'")
      }
      catchBlock = this.parseBlock()
      this.skipNewlines()
    }

    if (this.check(TT.FINALLY)) {
      this.advance() // finally
      finallyBlock = this.parseBlock()
    }

    return new AST.TryCatchStatement(tryBlock, catchVar, catchBlock, finallyBlock)
  }

  parseExprStatement() {
    const expr = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.ExprStatement(expr)
  }

  // ─── Expressions (Pratt / precedence climbing) ────────────────────────────
  parseExpr()       { return this.parseAssign() }

  parseAssign() {
    const left = this.parseOr()
    if (this.check(TT.ASSIGN)) {
      this.advance()
      const value = this.parseAssign()
      return new AST.AssignExpr(left, value, '=')
    }
    if (this.check(TT.PLUS_EQ)) {
      this.advance()
      return new AST.AssignExpr(left, this.parseAssign(), '+=')
    }
    if (this.check(TT.MINUS_EQ)) {
      this.advance()
      return new AST.AssignExpr(left, this.parseAssign(), '-=')
    }
   
    if (this.check(TT.STAR_EQ)) {
      this.advance()
      return new AST.AssignExpr(left, this.parseAssign(), '*=')
    }
    if (this.check(TT.SLASH_EQ)) {
      this.advance()
      return new AST.AssignExpr(left, this.parseAssign(), '/=')
    }
        return left
  }

  parseOr() {
    let left = this.parseAnd()
    while (this.check(TT.OR)) {
      const op = this.advance().value
      left = new AST.BinaryExpr(left, op, this.parseAnd())
    }
    return left
  }

  parseAnd() {
    let left = this.parseEquality()
    while (this.check(TT.AND)) {
      const op = this.advance().value
      left = new AST.BinaryExpr(left, op, this.parseEquality())
    }
    return left
  }

  parseEquality() {
    let left = this.parseComparison()
    while (this.check(TT.EQ) || this.check(TT.NEQ)) {
      const op = this.advance().type
      left = new AST.BinaryExpr(left, op, this.parseComparison())
    }
    return left
  }

  parseComparison() {
    let left = this.parseAddSub()
    while ([TT.LT, TT.GT, TT.LTE, TT.GTE].includes(this.peek().type)) {
      const op = this.advance().type
      left = new AST.BinaryExpr(left, op, this.parseAddSub())
    }
    return left
  }

  parseAddSub() {
    let left = this.parseMulDiv()
    while (this.check(TT.PLUS) || this.check(TT.MINUS)) {
      const op = this.advance().type
      left = new AST.BinaryExpr(left, op, this.parseMulDiv())
    }
    return left
  }

  parseMulDiv() {
    let left = this.parseUnary()
    while (this.check(TT.STAR) || this.check(TT.SLASH) || this.check(TT.PERCENT)) {
      const op = this.advance().type
      left = new AST.BinaryExpr(left, op, this.parseUnary())
    }
    return left
  }

  parseUnary() {
    if (this.check(TT.NOT)) {
      this.advance()
      return new AST.UnaryExpr('not', this.parseUnary())
    }
    if (this.check(TT.MINUS)) {
      this.advance()
      return new AST.UnaryExpr('-', this.parseUnary())
    }
    return this.parsePostfix()
  }

  parsePostfix() {
    let expr = this.parsePrimary()

    while (true) {
      if (this.check(TT.DOT)) {
        this.advance()
        const prop = this.expect(TT.IDENT, 'property name').value
        expr = new AST.MemberExpr(expr, prop)
      } else if (this.check(TT.LBRACKET)) {
        this.advance()
        const index = this.parseExpr()
        this.expect(TT.RBRACKET, "']'")
        expr = new AST.IndexExpr(expr, index)
      } else if (this.check(TT.LPAREN)) {
        const line = this.peek().line
        this.advance()
        const args = []
        while (!this.check(TT.RPAREN)) {
          args.push(this.parseExpr())
          if (!this.match(TT.COMMA)) break
        }
        this.expect(TT.RPAREN, "')'")
        expr = new AST.CallExpr(expr, args, line)
      } else break
    }

    return expr
  }

  parsePrimary() {
    const tok = this.peek()

    if (tok.type === TT.NUMBER)  { this.advance(); return new AST.NumberLiteral(tok.value) }
    if (tok.type === TT.STRING)  { this.advance(); return new AST.StringLiteral(tok.value) }
    if (tok.type === TT.BOOL)    { this.advance(); return new AST.BoolLiteral(tok.value) }
    if (tok.type === TT.NULL)    { this.advance(); return new AST.NullLiteral() }

    if (tok.type === TT.IDENT || tok.type === TT.SELF || tok.type === TT.SUPER) {
      this.advance()
      return new AST.Identifier(tok.value, tok.line)
    }

    if (tok.type === TT.FN) return this.parseFn() // anonymous fn

    // new ClassName(args) — syntactic sugar for a plain call expression.
    // 'new Dog("Rex")' is identical to 'Dog("Rex")' at the AST level;
    // the interpreter already handles class instantiation via CallExpr.
    if (tok.type === TT.NEW) {
      this.advance() // consume 'new'
      const nameTok = this.expect(TT.IDENT, 'class name')
      const callee  = new AST.Identifier(nameTok.value, nameTok.line)
      const line    = nameTok.line
      this.expect(TT.LPAREN, "'('")
      const args = []
      while (!this.check(TT.RPAREN)) {
        args.push(this.parseExpr())
        if (!this.match(TT.COMMA)) break
      }
      this.expect(TT.RPAREN, "')'")
      return new AST.CallExpr(callee, args, line)
    }

    if (tok.type === TT.LPAREN) {
      this.advance()
      const expr = this.parseExpr()
      this.expect(TT.RPAREN, "')'")
      return expr
    }

    if (tok.type === TT.LBRACKET) {
      this.advance()
      const elements = []
      while (!this.check(TT.RBRACKET)) {
        elements.push(this.parseExpr())
        if (!this.match(TT.COMMA)) break
      }
      this.expect(TT.RBRACKET, "']'")
      return new AST.ArrayLiteral(elements)
    }

    if (tok.type === TT.LBRACE) {
      this.advance()
      const pairs = []
      while (!this.check(TT.RBRACE)) {
        const key = this.parseExpr()
        this.expect(TT.COLON, "':'")
        const val = this.parseExpr()
        pairs.push([key, val])
        if (!this.match(TT.COMMA)) break
      }
      this.expect(TT.RBRACE, "'}'")
      return new AST.DictLiteral(pairs)
    }

    throw new Error(`[Parser] Line ${tok.line}: Unexpected token '${tok.value ?? tok.type}'`)
  }
}

module.exports = { Parser }