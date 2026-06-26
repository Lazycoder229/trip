// ─── AST Node Definitions ──────────────────────────────────────────────────

class NumberLiteral  { constructor(value)              { this.type = 'NumberLiteral';  this.value = value } }
class StringLiteral  { constructor(value)              { this.type = 'StringLiteral';  this.value = value } }
class BoolLiteral    { constructor(value)              { this.type = 'BoolLiteral';    this.value = value } }
class NullLiteral    {                                   constructor() { this.type = 'NullLiteral' } }
class Identifier     { constructor(name, line)         { this.type = 'Identifier';     this.name = name; this.line = line } }

class BinaryExpr {
  constructor(left, op, right, line) {
    this.type = 'BinaryExpr'; this.left = left; this.op = op; this.right = right; this.line = line
  }
}
class UnaryExpr {
  constructor(op, operand) {
    this.type = 'UnaryExpr'; this.op = op; this.operand = operand
  }
}
class AssignExpr {
  constructor(target, value, op = '=') {
    this.type = 'AssignExpr'; this.target = target; this.value = value; this.op = op
  }
}
class MemberExpr {
  constructor(object, property, line) {
    this.type = 'MemberExpr'; this.object = object; this.property = property; this.line = line
  }
}
class IndexExpr {
  constructor(object, index, line) {
    this.type = 'IndexExpr'; this.object = object; this.index = index; this.line = line
  }
}
class CallExpr {
  constructor(callee, args, line) {
    this.type = 'CallExpr'; this.callee = callee; this.args = args; this.line = line
  }
}
class ArrayLiteral {
  constructor(elements) { this.type = 'ArrayLiteral'; this.elements = elements }
}
class DictLiteral {
  constructor(pairs) { this.type = 'DictLiteral'; this.pairs = pairs }
}

// ─── Statements ────────────────────────────────────────────────────────────
class Program       { constructor(body)          { this.type = 'Program';       this.body = body } }
class Block         { constructor(body)          { this.type = 'Block';         this.body = body } }
class LetStatement {
  constructor(name, value, typeHint = null) { this.type = 'LetStatement'; this.name = name; this.value = value; this.typeHint = typeHint }
}
class FnDeclaration {
  constructor(name, params, body) {
    this.type = 'FnDeclaration'; this.name = name; this.params = params; this.body = body
  }
}
class ClassDeclaration {
  constructor(name, superclass, body) {
    this.type = 'ClassDeclaration'; this.name = name; this.superclass = superclass; this.body = body
  }
}
class ReturnStatement {
  constructor(value) { this.type = 'ReturnStatement'; this.value = value }
}
class IfStatement {
  constructor(condition, consequent, alternates, final) {
    this.type = 'IfStatement'
    this.condition  = condition
    this.consequent = consequent
    this.alternates = alternates
    this.final      = final
  }
}
class WhileStatement {
  constructor(condition, body) {
    this.type = 'WhileStatement'; this.condition = condition; this.body = body
  }
}
class ForStatement {
  constructor(variable, iterable, body) {
    this.type = 'ForStatement'; this.variable = variable; this.iterable = iterable; this.body = body
  }
}
class BreakStatement    { constructor() { this.type = 'BreakStatement' } }
class ContinueStatement { constructor() { this.type = 'ContinueStatement' } }
class ExprStatement     { constructor(expr) { this.type = 'ExprStatement'; this.expr = expr } }
class ImportStatement   { constructor(path, line) { this.type = 'ImportStatement'; this.path = path; this.line = line } }
class ThrowStatement {
  constructor(value, line) { this.type = 'ThrowStatement'; this.value = value; this.line = line }
}
class TryCatchStatement {
  constructor(tryBlock, catchVar, catchBlock, finallyBlock) {
    this.type = 'TryCatchStatement'
    this.tryBlock     = tryBlock
    this.catchVar     = catchVar
    this.catchBlock   = catchBlock
    this.finallyBlock = finallyBlock
  }
}

// ─── Phase 2 nodes ─────────────────────────────────────────────────────────
class ConstStatement {
  constructor(name, value, typeHint = null) { this.type = 'ConstStatement'; this.name = name; this.value = value; this.typeHint = typeHint }
}
class IncrDecrExpr {
  constructor(target, op, prefix) { this.type = 'IncrDecrExpr'; this.target = target; this.op = op; this.prefix = prefix }
}
class FStringExpr {
  constructor(parts, line) { this.type = 'FStringExpr'; this.parts = parts; this.line = line }
}
class TernaryExpr {
  constructor(condition, consequent, alternate) {
    this.type = 'TernaryExpr'; this.condition = condition; this.consequent = consequent; this.alternate = alternate
  }
}
class NullCoalesceExpr {
  constructor(left, right) { this.type = 'NullCoalesceExpr'; this.left = left; this.right = right }
}
class InExpr {
  constructor(item, container, line) { this.type = 'InExpr'; this.item = item; this.container = container; this.line = line }
}
class IsExpr {
  constructor(value, typeName, line) { this.type = 'IsExpr'; this.value = value; this.typeName = typeName; this.line = line }
}
class SpreadExpr {
  constructor(expr) { this.type = 'SpreadExpr'; this.expr = expr }
}

// ─── Phase 3 nodes ─────────────────────────────────────────────────────────

// [expr for var in iterable (if condition)?]
class ListComprehension {
  constructor(expr, variable, iterable, condition, line) {
    this.type      = 'ListComprehension'
    this.expr      = expr        // the mapped expression
    this.variable  = variable    // loop var name (string)
    this.iterable  = iterable    // iterable expression
    this.condition = condition   // optional filter expression (or null)
    this.line      = line
  }
}

// {keyExpr: valExpr for var in iterable (if condition)?}
class DictComprehension {
  constructor(keyExpr, valExpr, variable, iterable, condition, line) {
    this.type      = 'DictComprehension'
    this.keyExpr   = keyExpr
    this.valExpr   = valExpr
    this.variable  = variable
    this.iterable  = iterable
    this.condition = condition
    this.line      = line
  }
}

// match x: case p: body ... (else: body)?
class MatchStatement {
  constructor(subject, cases, elseBody, line) {
    this.type    = 'MatchStatement'
    this.subject = subject   // expression to match against
    this.cases   = cases     // array of { pattern, body }
    this.elseBody = elseBody // Block or null
    this.line    = line
  }
}

// ─── Phase 4 nodes ─────────────────────────────────────────────────────────

// yield expr  (inside a generator fn)
class YieldExpr {
  constructor(value, line) { this.type = 'YieldExpr'; this.value = value; this.line = line }
}

// await expr  (inside an async fn)
class AwaitExpr {
  constructor(value, line) { this.type = 'AwaitExpr'; this.value = value; this.line = line }
}

// @decorator applied to fn
class DecoratedFn {
  constructor(decorators, fn, line) {
    this.type = 'DecoratedFn'; this.decorators = decorators; this.fn = fn; this.line = line
  }
}

module.exports = {
  NumberLiteral, StringLiteral, BoolLiteral, NullLiteral, Identifier,
  BinaryExpr, UnaryExpr, AssignExpr, MemberExpr, IndexExpr, CallExpr,
  ArrayLiteral, DictLiteral,
  Program, Block, LetStatement, FnDeclaration, ClassDeclaration,
  ReturnStatement, IfStatement, WhileStatement, ForStatement,
  BreakStatement, ContinueStatement, ExprStatement, ImportStatement,
  ThrowStatement, TryCatchStatement,
  ConstStatement, IncrDecrExpr, FStringExpr, TernaryExpr,
  NullCoalesceExpr, InExpr, IsExpr, SpreadExpr,
  ListComprehension, DictComprehension, MatchStatement,
  YieldExpr, AwaitExpr, DecoratedFn,
}