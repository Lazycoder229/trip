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

  // ─── Block ───────────────────────────────────────────────────────────────
  parseBlock() {
    this.expect(TT.COLON, "':' before block")

    if (!this.check(TT.NEWLINE)) {
      const prev = this._inSingleLine
      this._inSingleLine = true
      const stmt = this.parseStatement()
      this._inSingleLine = prev
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
    if (tok.type === TT.CONST)    return this.parseConst()
    if (tok.type === TT.ASYNC)    return this.parseFn()
    if (tok.type === TT.FN)       return this.parseFn()
    if (tok.type === TT.AT)       return this.parseDecorated()
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
    if (tok.type === TT.PASS)     { this.advance(); this.match(TT.NEWLINE); return new AST.Block([]) }
    if (tok.type === TT.MATCH)    return this.parseMatch()

    return this.parseExprStatement()
  }

  // ─── Type hint helper ─────────────────────────────────────────────────────
  // Reads an optional `: TypeName` annotation and returns the type string or null.
  // Supports simple names (int, string) and generics (array[int]) — stored as
  // a plain string for now; the interpreter ignores it entirely.
  parseTypeHint() {
    if (!this.check(TT.COLON)) return null
    // Peek ahead: if next-next is NEWLINE/EOF it's a block colon, not a type hint
    // We disambiguate by checking whether this colon is followed by an IDENT
    const saved = this.pos
    this.advance() // consume ':'
    if (this.check(TT.IDENT)) {
      let hint = this.advance().value
      // generic: array[int], dict[string, int]
      if (this.check(TT.LBRACKET)) {
        this.advance()
        let inner = ''
        let depth = 1
        while (!this.isEOF() && depth > 0) {
          const t = this.advance()
          if (t.type === TT.LBRACKET) depth++
          else if (t.type === TT.RBRACKET) { depth--; if (depth === 0) break }
          inner += t.value ?? t.type
        }
        hint += `[${inner}]`
      }
      return hint
    }
    // Not a type hint — roll back
    this.pos = saved
    return null
  }

  parseLet() {
    this.advance() // let
    const name = this.expect(TT.IDENT, 'variable name').value

    // Optional type hint: let x: int = 5
    const typeHint = this.parseTypeHint()

    this.expect(TT.ASSIGN, "'='")
    const value = this.parseExpr()

    // Multi-declaration: let a = 1, b = 2, c = 3
    if (this.check(TT.COMMA)) {
      const decls = [new AST.LetStatement(name, value, typeHint)]
      while (this.match(TT.COMMA)) {
        const n = this.expect(TT.IDENT, 'variable name').value
        const th = this.parseTypeHint()
        this.expect(TT.ASSIGN, "'='")
        const v = this.parseExpr()
        decls.push(new AST.LetStatement(n, v, th))
      }
      this.match(TT.NEWLINE)
      return new AST.Block(decls)
    }

    this.match(TT.NEWLINE)
    return new AST.LetStatement(name, value, typeHint)
  }

  parseConst() {
    this.advance() // const
    const name = this.expect(TT.IDENT, 'constant name').value
    const typeHint = this.parseTypeHint()
    this.expect(TT.ASSIGN, "'='")
    const value = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.ConstStatement(name, value, typeHint)
  }

  parseFn() {
    let isAsync = false
    if (this.check(TT.ASYNC)) { this.advance(); isAsync = true }
    this.advance() // fn
    const name = this.check(TT.IDENT) ? this.advance().value : null
    this.expect(TT.LPAREN, "'('")
    const { params, paramTypes, defaults, hasRest, restName } = this.parseParams()
    this.expect(TT.RPAREN, "')'")

    // Optional return type: -> int
    let returnType = null
    if (this.check(TT.ARROW)) {
      this.advance()
      if (this.check(TT.IDENT)) returnType = this.advance().value
    }

    const body = this.parseBlock()
    const fn = new AST.FnDeclaration(name, params, body)
    fn.defaults   = defaults
    fn.hasRest    = hasRest
    fn.restName   = restName
    fn.paramTypes = paramTypes
    fn.returnType = returnType
    fn.isAsync    = isAsync
    return fn
  }

  // Returns { params, paramTypes, defaults, hasRest, restName }
  parseParams() {
    const params     = []
    const paramTypes = {}
    const defaults   = {}
    let hasRest      = false
    let restName     = null

    while (!this.check(TT.RPAREN)) {
      // *rest
      if (this.check(TT.STAR)) {
        this.advance()
        restName = this.expect(TT.IDENT, 'rest parameter name').value
        hasRest  = true
        this.match(TT.COMMA)
        break
      }

      let pname
      if (this.check(TT.SELF)) {
        this.advance(); pname = 'self'
      } else {
        pname = this.expect(TT.IDENT, 'parameter name').value
      }
      params.push(pname)

      // Optional type hint: fn foo(x: int)
      if (this.check(TT.COLON)) {
        this.advance()
        if (this.check(TT.IDENT)) paramTypes[pname] = this.advance().value
      }

      // default value?
      if (this.check(TT.ASSIGN)) {
        this.advance()
        defaults[pname] = this.parseExpr()
      }

      if (!this.match(TT.COMMA)) break
    }
    return { params, paramTypes, defaults, hasRest, restName }
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
    this.advance() // return
    if (!this.check(TT.NEWLINE) && !this.check(TT.EOF)) {
      const first = this.parseExpr()
      if (!this._inSingleLine && this.check(TT.COMMA)) {
        const elements = [first]
        while (this.match(TT.COMMA)) elements.push(this.parseExpr())
        this.match(TT.NEWLINE)
        return new AST.ReturnStatement(new AST.ArrayLiteral(elements))
      }
      this.match(TT.NEWLINE)
      return new AST.ReturnStatement(first)
    }
    this.match(TT.NEWLINE)
    return new AST.ReturnStatement(null)
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
    this.advance()
    const condition = this.parseExpr()
    const body = this.parseBlock()
    return new AST.WhileStatement(condition, body)
  }

  parseFor() {
    this.advance()
    const variable = this.expect(TT.IDENT, 'loop variable').value
    this.expect(TT.IN, "'in'")
    const iterable = this.parseExpr()
    const body = this.parseBlock()
    return new AST.ForStatement(variable, iterable, body)
  }

  parseImport() {
    this.advance()
    const p = this.expect(TT.STRING, 'module path').value
    this.match(TT.NEWLINE)
    return new AST.ImportStatement(p)
  }

  parseThrow() {
    this.advance()
    const value = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.ThrowStatement(value)
  }

  parseTryCatch() {
    this.advance()
    const tryBlock = this.parseBlock()
    let catchVar   = null, catchBlock = null, finallyBlock = null

    this.skipNewlines()
    if (this.check(TT.CATCH)) {
      this.advance()
      if (this.check(TT.LPAREN)) {
        this.advance()
        catchVar = this.expect(TT.IDENT, 'error variable name').value
        this.expect(TT.RPAREN, "')'")
      }
      catchBlock = this.parseBlock()
      this.skipNewlines()
    }
    if (this.check(TT.FINALLY)) {
      this.advance()
      finallyBlock = this.parseBlock()
    }
    return new AST.TryCatchStatement(tryBlock, catchVar, catchBlock, finallyBlock)
  }

  // ─── match statement ──────────────────────────────────────────────────────
  // match x:
  //     case 1:
  //         println("one")
  //     case "hi":
  //         println("hello")
  //     else:
  //         println("other")
  parseMatch() {
    const line = this.peek().line
    this.advance() // match
    const subject = this.parseExpr()
    this.expect(TT.COLON, "':' after match expression")
    this.expect(TT.NEWLINE, "newline after match ':'")
    this.expect(TT.INDENT, 'indented match body')
    this.skipNewlines()

    const cases   = []
    let elseBody  = null

    while (!this.check(TT.DEDENT) && !this.isEOF()) {
      if (this.check(TT.ELSE)) {
        this.advance()
        elseBody = this.parseBlock()
        this.skipNewlines()
        break
      }
      this.expect(TT.CASE, "'case'")
      const pattern = this.parseExpr()   // can be any literal/expr
      const body    = this.parseBlock()
      cases.push({ pattern, body })
      this.skipNewlines()
    }

    this.match(TT.DEDENT)
    return new AST.MatchStatement(subject, cases, elseBody, line)
  }

  // ─── @decorator ──────────────────────────────────────────────────────────
  // @dec1
  // @dec2
  // fn foo(): ...
  parseDecorated() {
    const line = this.peek().line
    const decorators = []
    while (this.check(TT.AT)) {
      this.advance() // @
      const name = this.expect(TT.IDENT, 'decorator name').value
      const args = []
      if (this.check(TT.LPAREN)) {
        this.advance()
        while (!this.check(TT.RPAREN)) {
          args.push(this.parseExpr())
          if (!this.match(TT.COMMA)) break
        }
        this.expect(TT.RPAREN, "')'")
      }
      decorators.push({ name, args })
      this.match(TT.NEWLINE)
      this.skipNewlines()
    }
    const fn = this.parseFn()
    return new AST.DecoratedFn(decorators, fn, line)
  }

  parseExprStatement() {
    const expr = this.parseExpr()
    this.match(TT.NEWLINE)
    return new AST.ExprStatement(expr)
  }

  // ─── Expressions ─────────────────────────────────────────────────────────
  parseExpr() { return this.parseTernary() }

  parseTernary() {
    let left = this.parseNullCoalesce()
    if (this.check(TT.IF)) {
      this.advance()
      const condition = this.parseNullCoalesce()
      this.expect(TT.ELSE, "'else'")
      const alternate = this.parseTernary()
      return new AST.TernaryExpr(condition, left, alternate)
    }
    return left
  }

  parseNullCoalesce() {
    let left = this.parseAssign()
    while (this.check(TT.QUESTION_QUESTION)) {
      this.advance()
      left = new AST.NullCoalesceExpr(left, this.parseAssign())
    }
    return left
  }

  parseAssign() {
    const left = this.parseOr()
    if (this.check(TT.ASSIGN)) {
      this.advance()
      const value = this.parseAssign()
      return new AST.AssignExpr(left, value, '=')
    }
    for (const [tt, op] of [[TT.PLUS_EQ,'+='], [TT.MINUS_EQ,'-='], [TT.STAR_EQ,'*='], [TT.SLASH_EQ,'/=']]) {
      if (this.check(tt)) {
        this.advance()
        return new AST.AssignExpr(left, this.parseAssign(), op)
      }
    }
    return left
  }

  parseOr() {
    let left = this.parseAnd()
    while (this.check(TT.OR)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.value, this.parseAnd(), opTok.line)
    }
    return left
  }

  parseAnd() {
    let left = this.parseEquality()
    while (this.check(TT.AND)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.value, this.parseEquality(), opTok.line)
    }
    return left
  }

  parseEquality() {
    let left = this.parseComparison()
    while (this.check(TT.EQ) || this.check(TT.NEQ)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.type, this.parseComparison(), opTok.line)
    }
    return left
  }

  parseComparison() {
    let left = this.parseIn()
    while ([TT.LT, TT.GT, TT.LTE, TT.GTE].includes(this.peek().type)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.type, this.parseIn(), opTok.line)
    }
    return left
  }

  parseIn() {
    let left = this.parseAddSub()
    if (this.check(TT.IN)) {
      const line = this.peek().line; this.advance()
      return new AST.InExpr(left, this.parseAddSub(), line)
    }
    if (this.check(TT.IS)) {
      const line = this.peek().line; this.advance()
      const typeName = this.expect(TT.IDENT, 'type name').value
      return new AST.IsExpr(left, typeName, line)
    }
    return left
  }

  parseAddSub() {
    let left = this.parseMulDiv()
    while (this.check(TT.PLUS) || this.check(TT.MINUS)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.type, this.parseMulDiv(), opTok.line)
    }
    return left
  }

  parseMulDiv() {
    let left = this.parseExp()
    while (this.check(TT.STAR) || this.check(TT.SLASH) || this.check(TT.PERCENT)) {
      const opTok = this.advance()
      left = new AST.BinaryExpr(left, opTok.type, this.parseExp(), opTok.line)
    }
    return left
  }

  parseExp() {
    let left = this.parseUnary()
    if (this.check(TT.STAR_STAR)) {
      const line = this.peek().line; this.advance()
      return new AST.BinaryExpr(left, '**', this.parseExp(), line)
    }
    return left
  }

  parseUnary() {
    if (this.check(TT.NOT)) { this.advance(); return new AST.UnaryExpr('not', this.parseUnary()) }
    if (this.check(TT.MINUS)) { this.advance(); return new AST.UnaryExpr('-', this.parseUnary()) }
    if (this.check(TT.PLUS_PLUS) || this.check(TT.MINUS_MINUS)) {
      const op = this.advance().type === TT.PLUS_PLUS ? '++' : '--'
      const target = this.parsePostfix()
      return new AST.IncrDecrExpr(target, op, true)
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
      } else if (this.check(TT.PLUS_PLUS) || this.check(TT.MINUS_MINUS)) {
        const op = this.advance().type === TT.PLUS_PLUS ? '++' : '--'
        expr = new AST.IncrDecrExpr(expr, op, false)
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

    if (tok.type === TT.AWAIT) {
      const line = tok.line
      this.advance()
      const value = this.parseUnary()
      return new AST.AwaitExpr(value, line)
    }

    if (tok.type === TT.YIELD) {
      const line = tok.line
      this.advance()
      const value = (!this.check(TT.NEWLINE) && !this.check(TT.EOF))
        ? this.parseExpr()
        : null
      return new AST.YieldExpr(value, line)
    }

    if (tok.type === TT.FSTRING) {      this.advance()
      return new AST.FStringExpr(tok.value, tok.line)
    }

    if (tok.type === TT.IDENT || tok.type === TT.SELF || tok.type === TT.SUPER) {
      this.advance()
      return new AST.Identifier(tok.value, tok.line)
    }

    if (tok.type === TT.FN) return this.parseFn()

    if (tok.type === TT.NEW) {
      this.advance()
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

    // ─── Array literal OR list comprehension ─────────────────────────────
    if (tok.type === TT.LBRACKET) {
      const line = tok.line
      this.advance()
      if (this.check(TT.RBRACKET)) { this.advance(); return new AST.ArrayLiteral([]) }

      const first = this.parseExpr()  // full expr OK — ternary `if`..`else` is fully consumed before `for`

      // [expr for var in iterable (if cond)?]
      if (this.check(TT.FOR)) {
        this.advance()
        const variable = this.expect(TT.IDENT, 'loop variable').value
        this.expect(TT.IN, "'in'")
        const iterable = this.parseAssign()  // stop before ternary `if`
        let condition = null
        if (this.check(TT.IF)) { this.advance(); condition = this.parseAssign() }
        this.expect(TT.RBRACKET, "']'")
        return new AST.ListComprehension(first, variable, iterable, condition, line)
      }

      // Regular array literal
      const elements = [first]
      while (this.match(TT.COMMA)) {
        if (this.check(TT.RBRACKET)) break // trailing comma
        elements.push(this.parseExpr())
      }
      this.expect(TT.RBRACKET, "']'")
      return new AST.ArrayLiteral(elements)
    }

    // ─── Dict literal OR dict comprehension ──────────────────────────────
    if (tok.type === TT.LBRACE) {
      const line = tok.line
      this.advance()
      if (this.check(TT.RBRACE)) { this.advance(); return new AST.DictLiteral([]) }

      const firstKey = this.parseExpr()  // full expr OK
      this.expect(TT.COLON, "':'")
      const firstVal = this.parseExpr()

      // {key: val for var in iterable (if cond)?}
      if (this.check(TT.FOR)) {
        this.advance()
        const variable = this.expect(TT.IDENT, 'loop variable').value
        this.expect(TT.IN, "'in'")
        const iterable = this.parseAssign()  // stop before ternary `if`
        let condition = null
        if (this.check(TT.IF)) { this.advance(); condition = this.parseAssign() }
        this.expect(TT.RBRACE, "'}'")
        return new AST.DictComprehension(firstKey, firstVal, variable, iterable, condition, line)
      }

      // Regular dict literal
      const pairs = [[firstKey, firstVal]]
      while (this.match(TT.COMMA)) {
        if (this.check(TT.RBRACE)) break // trailing comma
        const key = this.parseExpr()
        this.expect(TT.COLON, "':'")
        const val = this.parseExpr()
        pairs.push([key, val])
      }
      this.expect(TT.RBRACE, "'}'")
      return new AST.DictLiteral(pairs)
    }

    throw new Error(`[Parser] Line ${tok.line}: Unexpected token '${tok.value ?? tok.type}'`)
  }
}

module.exports = { Parser }