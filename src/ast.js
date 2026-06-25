// ─── AST Node Definitions ──────────────────────────────────────────────────
// Each node represents one syntactic construct in the language.

class NumberLiteral  { constructor(value)              { this.type = 'NumberLiteral';  this.value = value } }
class StringLiteral  { constructor(value)              { this.type = 'StringLiteral';  this.value = value } }
class BoolLiteral    { constructor(value)              { this.type = 'BoolLiteral';    this.value = value } }
class NullLiteral    {                                   constructor() { this.type = 'NullLiteral' } }
class Identifier     { constructor(name, line)         { this.type = 'Identifier';     this.name = name; this.line = line } }

class BinaryExpr {
  constructor(left, op, right) {
    this.type = 'BinaryExpr'; this.left = left; this.op = op; this.right = right
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
  constructor(object, property) {
    this.type = 'MemberExpr'; this.object = object; this.property = property
  }
}
class IndexExpr {
  constructor(object, index) {
    this.type = 'IndexExpr'; this.object = object; this.index = index
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
  constructor(name, value) { this.type = 'LetStatement'; this.name = name; this.value = value }
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
    this.alternates = alternates  // array of { condition, body }
    this.final      = final       // else block or null
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
class ImportStatement   { constructor(path) { this.type = 'ImportStatement'; this.path = path } }

module.exports = {
  NumberLiteral, StringLiteral, BoolLiteral, NullLiteral, Identifier,
  BinaryExpr, UnaryExpr, AssignExpr, MemberExpr, IndexExpr, CallExpr,
  ArrayLiteral, DictLiteral,
  Program, Block, LetStatement, FnDeclaration, ClassDeclaration,
  ReturnStatement, IfStatement, WhileStatement, ForStatement,
  BreakStatement, ContinueStatement, ExprStatement, ImportStatement,
}
