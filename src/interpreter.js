const { Environment } = require('./environment')
const path = require('path')
const fs   = require('fs')

// ─── Control flow signals ──────────────────────────────────────────────────
class ReturnSignal  { constructor(value) { this.value = value } }
class BreakSignal   {}
class ContinueSignal {}

// ─── Runtime Types ─────────────────────────────────────────────────────────
class LangFunction {
  constructor(name, params, body, closure) {
    this.name    = name ?? '<anonymous>'
    this.params  = params
    this.body    = body
    this.closure = closure
  }
  toString() { return `<fn ${this.name}>` }
}

class LangClass {
  constructor(name, superclass, methods) {
    this.name       = name
    this.superclass = superclass
    this.methods    = methods   // Map<string, LangFunction>
  }
  toString() { return `<class ${this.name}>` }

  findMethod(name) {
    if (this.methods.has(name)) return this.methods.get(name)
    if (this.superclass) return this.superclass.findMethod(name)
    return null
  }
}

class LangInstance {
  constructor(klass) {
    this.klass  = klass
    this.fields = new Map()
  }
  toString() {
    if (this.fields.has('__str__')) {
      // handled at call site
    }
    return `<${this.klass.name} instance>`
  }
  get(name) {
    if (this.fields.has(name)) return this.fields.get(name)
    const method = this.klass.findMethod(name)
    if (method) return this.bindMethod(method)
    throw new Error(`[Runtime]: '${this.klass.name}' has no attribute '${name}'`)
  }
  set(name, value) { this.fields.set(name, value); return value }
  bindMethod(fn) {
    // Return a new function with 'self' pre-bound in its closure
    const env = fn.closure.child()
    env.define('self', this)
    return new LangFunction(fn.name, fn.params, fn.body, env)
  }
}

// ─── Standard Library ──────────────────────────────────────────────────────
function buildStdLib() {
  const env = new Environment()

  const native = (fn) => fn

  env.define('print',   native((...args) => { console.log(...args.map(stringify)); return null }))
  env.define('println', native((...args) => { process.stdout.write(args.map(stringify).join('') + '\n'); return null }))
  env.define('input',   native(() => { return '' })) // sync input not easy in Node; placeholder
  env.define('str',     native((v) => stringify(v)))
  env.define('num',     native((v) => parseFloat(v)))
  env.define('bool',    native((v) => !!v))
  env.define('len',     native((v) => {
    if (Array.isArray(v)) return v.length
    if (typeof v === 'string') return v.length
    if (v instanceof Map) return v.size
    throw new Error('[Runtime]: len() requires string, array, or dict')
  }))
  env.define('type',    native((v) => {
    if (v === null)            return 'null'
    if (typeof v === 'number') return 'number'
    if (typeof v === 'string') return 'string'
    if (typeof v === 'boolean') return 'bool'
    if (Array.isArray(v))      return 'array'
    if (v instanceof Map)      return 'dict'
    if (v instanceof LangFunction) return 'function'
    if (v instanceof LangClass)    return 'class'
    if (v instanceof LangInstance) return v.klass.name
    return 'unknown'
  }))
  env.define('range', native((start, end, step = 1) => {
    const result = []
    if (end === undefined) { end = start; start = 0 }
    for (let i = start; i < end; i += step) result.push(i)
    return result
  }))
  env.define('push',   native((arr, val) => { arr.push(val); return arr }))
  env.define('pop',    native((arr) => arr.pop()))
  env.define('keys',   native((d) => [...d.keys()]))
  env.define('values', native((d) => [...d.values()]))
  env.define('split',  native((str, sep = '') => str.split(sep)))
  env.define('join',   native((arr, sep = '') => arr.join(sep)))
  env.define('upper',  native((s) => s.toUpperCase()))
  env.define('lower',  native((s) => s.toLowerCase()))
  env.define('trim',   native((s) => s.trim()))
  env.define('floor',  native((n) => Math.floor(n)))
  env.define('ceil',   native((n) => Math.ceil(n)))
  env.define('round',  native((n) => Math.round(n)))
  env.define('abs',    native((n) => Math.abs(n)))
  env.define('sqrt',   native((n) => Math.sqrt(n)))
  env.define('random', native(() => Math.random()))
  env.define('max',    native((...args) => Math.max(...args)))
  env.define('min',    native((...args) => Math.min(...args)))

  return env
}

function stringify(v) {
  if (v === null)             return 'null'
  if (typeof v === 'boolean') return v ? 'true' : 'false'
  if (Array.isArray(v))       return '[' + v.map(stringify).join(', ') + ']'
  if (v instanceof Map) {
    const pairs = [...v.entries()].map(([k, val]) => `${stringify(k)}: ${stringify(val)}`)
    return '{' + pairs.join(', ') + '}'
  }
  if (v instanceof LangInstance) {
    const toStr = v.klass.findMethod('__str__')
    if (toStr) {
      const bound = v.bindMethod(toStr)
      // Can't call interpreter here; fall back
    }
    return v.toString()
  }
  return String(v)
}

// ─── Interpreter ───────────────────────────────────────────────────────────
class Interpreter {
  constructor(baseDir = process.cwd()) {
    this.globals = buildStdLib()
    this.baseDir = baseDir
  }

  run(program) {
    return this.execBlock(program.body, this.globals)
  }

  execBlock(stmts, env) {
    let result = null
    for (const stmt of stmts) {
      result = this.exec(stmt, env)
      if (result instanceof ReturnSignal) return result
      if (result instanceof BreakSignal)  return result
      if (result instanceof ContinueSignal) return result
    }
    return result
  }

  exec(node, env) {
    switch (node.type) {

      case 'Program':
        return this.execBlock(node.body, env)

      case 'Block':
        return this.execBlock(node.body, env)

      case 'LetStatement': {
        const value = node.value ? this.eval(node.value, env) : null
        env.define(node.name, value)
        return null
      }

      case 'FnDeclaration': {
        const fn = new LangFunction(node.name, node.params, node.body, env)
        if (node.name) env.define(node.name, fn)
        return fn
      }

      case 'ClassDeclaration': {
        let superclass = null
        if (node.superclass) {
          superclass = env.get(node.superclass)
          if (!(superclass instanceof LangClass))
            throw new Error(`[Runtime]: '${node.superclass}' is not a class`)
        }
        const methods = new Map()
        for (const stmt of node.body.body) {
          if (stmt.type === 'FnDeclaration') {
            methods.set(stmt.name, new LangFunction(stmt.name, stmt.params, stmt.body, env))
          }
        }
        const klass = new LangClass(node.name, superclass, methods)
        env.define(node.name, klass)
        return null
      }

      case 'ReturnStatement': {
        const value = node.value ? this.eval(node.value, env) : null
        return new ReturnSignal(value)
      }

      case 'IfStatement': {
        if (this.isTruthy(this.eval(node.condition, env))) {
          const scope = env.child()
          return this.execBlock(node.consequent.body, scope)
        }
        for (const alt of node.alternates) {
          if (this.isTruthy(this.eval(alt.condition, env))) {
            return this.execBlock(alt.body.body, env.child())
          }
        }
        if (node.final) return this.execBlock(node.final.body, env.child())
        return null
      }

      case 'WhileStatement': {
        while (this.isTruthy(this.eval(node.condition, env))) {
          const result = this.execBlock(node.body.body, env.child())
          if (result instanceof BreakSignal)    break
          if (result instanceof ReturnSignal)   return result
          if (result instanceof ContinueSignal) continue
        }
        return null
      }

      case 'ForStatement': {
        const iterable = this.eval(node.iterable, env)
        const items = Array.isArray(iterable)
          ? iterable
          : (typeof iterable === 'string' ? iterable.split('') : [...iterable])
        for (const item of items) {
          const scope = env.child()
          scope.define(node.variable, item)
          const result = this.execBlock(node.body.body, scope)
          if (result instanceof BreakSignal)    break
          if (result instanceof ReturnSignal)   return result
          if (result instanceof ContinueSignal) continue
        }
        return null
      }

      case 'BreakStatement':    return new BreakSignal()
      case 'ContinueStatement': return new ContinueSignal()

      case 'ExprStatement':
        return this.eval(node.expr, env)

      case 'ImportStatement': {
        const filePath = path.resolve(this.baseDir, node.path + '.tp')
        if (!fs.existsSync(filePath))
          throw new Error(`[Runtime]: Cannot import '${node.path}': file not found`)
        const src = fs.readFileSync(filePath, 'utf8')
        const { Lexer } = require('./lexer')
        const { Parser } = require('./parser')
        const tokens  = new Lexer(src).tokenize()
        const program = new Parser(tokens).parse()
        const modInterp = new Interpreter(path.dirname(filePath))
        modInterp.globals.parent = this.globals
        modInterp.run(program)
        // merge module exports back (anything defined at top level)
        for (const [k, v] of modInterp.globals.vars) {
          env.define(k, v)
        }
        return null
      }

      default:
        throw new Error(`[Interpreter]: Unknown statement type '${node.type}'`)
    }
  }

  eval(node, env) {
    switch (node.type) {

      case 'NumberLiteral': return node.value
      case 'StringLiteral': return node.value
      case 'BoolLiteral':   return node.value
      case 'NullLiteral':   return null

      case 'Identifier':
        return env.get(node.name, node.line)

      case 'ArrayLiteral':
        return node.elements.map(e => this.eval(e, env))

      case 'DictLiteral': {
        const map = new Map()
        for (const [k, v] of node.pairs) map.set(this.eval(k, env), this.eval(v, env))
        return map
      }

      case 'BinaryExpr': {
        const l = this.eval(node.left, env)
        const r = this.eval(node.right, env)
        switch (node.op) {
          case '+':   return typeof l === 'string' || typeof r === 'string'
                        ? stringify(l) + stringify(r) : l + r
          case '-':   return l - r
          case '*':   return l * r
          case '/':   if (r === 0) throw new Error('[Runtime]: Division by zero'); return l / r
          case '%':   return l % r
          case '==':  return l === r
          case '!=':  return l !== r
          case '<':   return l < r
          case '>':   return l > r
          case '<=':  return l <= r
          case '>=':  return l >= r
          case 'and': return this.isTruthy(l) ? r : l
          case 'or':  return this.isTruthy(l) ? l : r
          default: throw new Error(`[Runtime]: Unknown operator '${node.op}'`)
        }
      }

      case 'UnaryExpr': {
        const val = this.eval(node.operand, env)
        if (node.op === '-')   return -val
        if (node.op === 'not') return !this.isTruthy(val)
        break
      }

      case 'AssignExpr': {
        const value = this.eval(node.value, env)
        return this.assign(node.target, value, node.op, env)
      }

      case 'MemberExpr': {
        const obj = this.eval(node.object, env)
        if (obj instanceof LangInstance) return obj.get(node.property)
        if (obj instanceof Map)          return obj.get(node.property) ?? null
        // Built-in array/string methods
        return this.getMemberBuiltin(obj, node.property)
      }

      case 'IndexExpr': {
        const obj = this.eval(node.object, env)
        const idx = this.eval(node.index, env)
        if (Array.isArray(obj)) return obj[idx] ?? null
        if (obj instanceof Map) return obj.get(idx) ?? null
        if (typeof obj === 'string') return obj[idx] ?? null
        throw new Error('[Runtime]: Index operation on non-indexable type')
      }

      case 'CallExpr': {
        const callee = this.eval(node.callee, env)
        const args   = node.args.map(a => this.eval(a, env))
        return this.callFunction(callee, args, node.line)
      }

      case 'FnDeclaration':
        return this.exec(node, env)

      default:
        throw new Error(`[Interpreter]: Unknown expression type '${node.type}'`)
    }
  }

  assign(target, value, op, env) {
    const compute = (old) => {
      if (op === '+=') return typeof old === 'string' ? old + stringify(value) : old + value
      if (op === '-=') return old - value
      return value
    }

    if (target.type === 'Identifier') {
      if (env.has(target.name)) {
        const old = env.get(target.name, target.line)
        const next = compute(old)
        env.set(target.name, next, target.line)
        return next
      } else {
        // allow implicit declaration at current scope
        env.define(target.name, compute(null))
        return env.get(target.name)
      }
    }
    if (target.type === 'MemberExpr') {
      const obj = this.eval(target.object, env)
      if (obj instanceof LangInstance) {
        const old  = obj.fields.has(target.property) ? obj.fields.get(target.property) : null
        const next = compute(old)
        obj.set(target.property, next)
        return next
      }
      if (obj instanceof Map) {
        const old  = obj.get(target.property)
        obj.set(target.property, compute(old))
        return obj.get(target.property)
      }
    }
    if (target.type === 'IndexExpr') {
      const obj = this.eval(target.object, env)
      const idx = this.eval(target.index, env)
      if (Array.isArray(obj)) { obj[idx] = compute(obj[idx]); return obj[idx] }
      if (obj instanceof Map) { obj.set(idx, compute(obj.get(idx))); return obj.get(idx) }
    }
    throw new Error('[Runtime]: Invalid assignment target')
  }

  callFunction(callee, args, line) {
    // Native JS function
    if (typeof callee === 'function') return callee(...args) ?? null

    // User-defined function
    if (callee instanceof LangFunction) {
      const scope = callee.closure.child()
      callee.params.forEach((p, i) => scope.define(p, args[i] ?? null))
      const result = this.execBlock(callee.body.body, scope)
      if (result instanceof ReturnSignal) return result.value
      return null
    }

    // Class instantiation
    if (callee instanceof LangClass) {
      const instance = new LangInstance(callee)
      const init = callee.findMethod('init')
      if (init) {
        const bound = instance.bindMethod(init)
        this.callFunction(bound, args, line)
      }
      return instance
    }

    throw new Error(`[Runtime] Line ${line}: '${stringify(callee)}' is not callable`)
  }

  getMemberBuiltin(obj, prop) {
    if (Array.isArray(obj)) {
      const methods = {
        push:    (...a) => { obj.push(...a); return obj },
        pop:     ()    => obj.pop(),
        length:  obj.length,
        map:     (fn)  => obj.map(v => this.callFunction(fn, [v])),
        filter:  (fn)  => obj.filter(v => this.isTruthy(this.callFunction(fn, [v]))),
        forEach: (fn)  => { obj.forEach(v => this.callFunction(fn, [v])); return null },
        join:    (sep) => obj.join(sep ?? ','),
        reverse: ()    => [...obj].reverse(),
        includes:(v)   => obj.includes(v),
        indexOf: (v)   => obj.indexOf(v),
      }
      return methods[prop] ?? null
    }
    if (typeof obj === 'string') {
      const methods = {
        length:    obj.length,
        upper:     () => obj.toUpperCase(),
        lower:     () => obj.toLowerCase(),
        trim:      () => obj.trim(),
        split:     (sep) => obj.split(sep ?? ''),
        includes:  (s)   => obj.includes(s),
        startsWith:(s)   => obj.startsWith(s),
        endsWith:  (s)   => obj.endsWith(s),
        replace:   (a,b) => obj.replace(a, b),
        indexOf:   (s)   => obj.indexOf(s),
      }
      return methods[prop] ?? null
    }
    return null
  }

  isTruthy(v) {
    if (v === null || v === false) return false
    if (typeof v === 'number' && v === 0) return false
    if (typeof v === 'string' && v === '') return false
    if (Array.isArray(v) && v.length === 0) return false
    return true
  }
}

module.exports = { Interpreter, LangFunction, LangClass, LangInstance, stringify }
