const { Environment } = require('./environment')
const path = require('path')
const fs   = require('fs')

// ─── Control flow signals ──────────────────────────────────────────────────
class ReturnSignal   { constructor(value) { this.value = value } }
class BreakSignal    {}
class ContinueSignal {}
class ThrowSignal    { constructor(value) { this.value = value } }
class YieldSignal    { constructor(value) { this.value = value } }

// Wraps the return value of an async fn so await can unwrap it synchronously.
// This keeps the interpreter purely synchronous while giving async/await semantics
// for Trip-to-Trip async calls (no real I/O pending, so no actual blocking needed).
class AsyncValue     { constructor(value) { this.value = value } }

// ─── TripError ─────────────────────────────────────────────────────────────
class TripError extends Error {
  constructor(phase, message, line = null) {
    const loc = line != null ? ` Line ${line}:` : ''
    super(`[${phase}]${loc} ${message}`)
    this.phase    = phase
    this.tripMsg  = message
    this.tripLine = line
  }
}

// ─── Runtime Types ─────────────────────────────────────────────────────────
class LangFunction {
  constructor(name, params, body, closure, defaults = {}, hasRest = false, restName = null) {
    this.name     = name ?? '<anonymous>'
    this.params   = params
    this.body     = body
    this.closure  = closure
    this.defaults = defaults
    this.hasRest  = hasRest
    this.restName = restName
  }
  toString() { return `<fn ${this.name}>` }
}

class LangClass {
  constructor(name, superclass, methods, statics) {
    this.name       = name
    this.superclass = superclass
    this.methods    = methods
    this.statics    = statics
  }
  toString() { return `<class ${this.name}>` }

  findMethod(name) {
    if (this.methods.has(name)) return this.methods.get(name)
    if (this.superclass) return this.superclass.findMethod(name)
    return null
  }

  findStatic(name) {
    if (this.statics.has(name)) return this.statics.get(name)
    if (this.superclass) return this.superclass.findStatic(name)
    return null
  }
}

// A generator wraps a LangFunction that contains yield statements.
// Calling next() resumes execution until the next yield or return.
class LangGenerator {
  constructor(fn, args, interp) {
    this.fn     = fn
    this.args   = args
    this.interp = interp
    this.done   = false
    this._iter  = this._run()
  }

  *_run() {
    const scope = this.fn.closure.child()
    const bindable = this.fn.params.filter(p => p !== 'self')
    bindable.forEach((p, i) => {
      let val = this.args[i]
      if (val === undefined) val = (p in this.fn.defaults) ? this.interp.eval(this.fn.defaults[p], scope) : null
      scope.define(p, val)
    })
    yield* this.interp.execGenerator(this.fn.body.body, scope)
  }

  next() {
    const r = this._iter.next()
    if (r.done) this.done = true
    return r
  }

  [Symbol.iterator]() {
    return { next: () => this.next() }
  }
}

class LangInstance {
  constructor(klass) {
    this.klass  = klass
    this.fields = new Map()
  }
  toString() { return `<${this.klass.name} instance>` }
  get(name, line = null) {
    if (this.fields.has(name)) return this.fields.get(name)
    const method = this.klass.findMethod(name)
    if (method) return this.bindMethod(method)
    throw new TripError('Runtime', `'${this.klass.name}' has no attribute '${name}'`, line)
  }
  set(name, value) { this.fields.set(name, value); return value }
  bindMethod(fn) {
    const env = fn.closure.child()
    env.define('self', this)
    return new LangFunction(fn.name, fn.params, fn.body, env, fn.defaults, fn.hasRest, fn.restName)
  }
}

// ─── Standard Library ──────────────────────────────────────────────────────
function buildStdLib(interp) {
  const env = new Environment()

  env.define('out',   (...args) => { process.stdout.write(args.map(v => interp.stringify(v)).join('') + '\n'); return null })
  env.define('outn', (...args) => { process.stdout.write(args.map(v => interp.stringify(v)).join(' ') + '\n'); return null })
  env.define('inp',   () => '')
  env.define('str',     (v) => interp.stringify(v))
  env.define('num',     (v) => parseFloat(v))
  env.define('int',     (v) => Math.trunc(typeof v === 'string' ? parseFloat(v) : v))
  env.define('float',   (v) => parseFloat(v))
  env.define('bool',    (v) => !!v)
  env.define('len', (v) => {
    if (Array.isArray(v))      return v.length
    if (typeof v === 'string') return v.length
    if (v instanceof Map)      return v.size
    throw new TripError('Runtime', 'len() requires string, array, or dict')
  })
  env.define('type', (v) => {
    if (v === null)                return 'null'
    if (typeof v === 'number')     return Number.isInteger(v) ? 'int' : 'float'
    if (typeof v === 'string')     return 'string'
    if (typeof v === 'boolean')    return 'bool'
    if (Array.isArray(v))          return 'array'
    if (v instanceof Map)          return 'dict'
    if (v instanceof LangFunction) return 'function'
    if (v instanceof LangClass)    return 'class'
    if (v instanceof LangGenerator) return 'generator'
    if (v instanceof LangInstance) return v.klass.name
    return 'unknown'
  })
  env.define('range', (start, end, step) => {
    if (end === undefined) { end = start; start = 0 }
    if (step === undefined) step = 1
    return (function*() {
      for (let i = start; i < end; i += step) yield i
    })()
  })
  env.define('memory', () => {
    for (const [k, v] of interp.globals.vars)
      console.log(`${k} = ${interp.stringify(v)} (${typeof v})`)
    return null
  })
  env.define('push',   (arr, val) => { arr.push(val); return arr })
  env.define('pop',    (arr) => arr.pop())
  env.define('keys',   (d) => [...d.keys()])
  env.define('values', (d) => [...d.values()])
  env.define('split',  (str, sep = '') => str.split(sep))
  env.define('join',   (arr, sep = '') => arr.join(sep))
  env.define('upper',  (s) => s.toUpperCase())
  env.define('lower',  (s) => s.toLowerCase())
  env.define('trim',   (s) => s.trim())
  env.define('floor',  (n) => Math.floor(n))
  env.define('ceil',   (n) => Math.ceil(n))
  env.define('round',  (n) => Math.round(n))
  env.define('abs',    (n) => Math.abs(n))
  env.define('sqrt',   (n) => Math.sqrt(n))
  env.define('random', () => Math.random())
  env.define('max',    (...args) => Math.max(...args))
  env.define('min',    (...args) => Math.min(...args))

  // New Phase 2 stdlib additions
  env.define('format', (s, ...args) => {
    let i = 0
    return s.replace(/\{\}/g, () => interp.stringify(args[i++] ?? null))
  })
  env.define('padStart', (s, n, ch = ' ') => String(s).padStart(n, ch))
  env.define('padEnd',   (s, n, ch = ' ') => String(s).padEnd(n, ch))
  env.define('repeat',   (s, n) => String(s).repeat(n))
  env.define('contains', (container, item) => {
    if (Array.isArray(container)) return container.includes(item)
    if (typeof container === 'string') return container.includes(item)
    if (container instanceof Map) return container.has(item)
    return false
  })
  env.define('sorted', (arr, key) => {
    const copy = [...arr]
    if (key && (key instanceof LangFunction || typeof key === 'function')) {
      copy.sort((a, b) => {
        const ka = interp.callFunction(key, [a])
        const kb = interp.callFunction(key, [b])
        return ka < kb ? -1 : ka > kb ? 1 : 0
      })
    } else {
      copy.sort((a, b) => a < b ? -1 : a > b ? 1 : 0)
    }
    return copy
  })
  env.define('reversed', (arr) => [...arr].reverse())
  env.define('enumerate', (arr) => arr.map((v, i) => [i, v]))
  env.define('zip', (a, b) => {
    const len = Math.min(a.length, b.length)
    return Array.from({ length: len }, (_, i) => [a[i], b[i]])
  })
  env.define('map',    (fn, arr) => arr.map(v => interp.callFunction(fn, [v])))
  env.define('filter', (fn, arr) => arr.filter(v => interp.isTruthy(interp.callFunction(fn, [v]))))
  env.define('reduce', (fn, arr, init) => {
    let acc = init ?? arr[0]
    const start = init !== undefined ? 0 : 1
    for (let i = start; i < arr.length; i++) acc = interp.callFunction(fn, [acc, arr[i]])
    return acc
  })
  env.define('chr', (n) => String.fromCharCode(n))
  env.define('ord', (s) => s.charCodeAt(0))
  env.define('hex', (n) => n.toString(16))
  env.define('pow', (base, exp) => Math.pow(base, exp))
  env.define('log', (n, base) => base ? Math.log(n) / Math.log(base) : Math.log(n))
  env.define('sin', (n) => Math.sin(n))
  env.define('cos', (n) => Math.cos(n))
  env.define('PI',  Math.PI)
  env.define('E',   Math.E)
  env.define('Infinity', Infinity)
  env.define('NaN', NaN)
  env.define('isNaN', (n) => Number.isNaN(n))

  // ─── Error base class ────────────────────────────────────────────────────
  // Makes `class MyError(Error):` work — Error is a real Trip class with
  // an `init(msg)` method and a `message` field.
  const errorMethods = new Map()
  const errorInitFn = {
    name: 'init', params: ['self', 'message'], body: { body: [] },
    closure: env, defaults: {}, hasRest: false, restName: null,
    isAsync: false,
    // Native init: set self.message = message
    _native: (instance, args) => {
      instance.fields.set('message', args[0] ?? null)
      instance.fields.set('name',    instance.klass.name)
    }
  }
  errorMethods.set('init', errorInitFn)
  const ErrorClass = new LangClass('Error', null, errorMethods, new Map())
  env.define('Error', ErrorClass)

  // ─── file object ─────────────────────────────────────────────────────────
  const fs   = require('fs')
  const path = require('path')
  const fileObj = new Map()
  fileObj.set('read',   (p)    => fs.readFileSync(p, 'utf8'))
  fileObj.set('write',  (p, d) => { fs.writeFileSync(p, d ?? '', 'utf8'); return null })
  fileObj.set('append', (p, d) => { fs.appendFileSync(p, d ?? '', 'utf8'); return null })
  fileObj.set('exists', (p)    => fs.existsSync(p))
  fileObj.set('delete', (p)    => { fs.unlinkSync(p); return null })
  fileObj.set('lines',  (p)    => fs.readFileSync(p, 'utf8').split('\n'))
  env.define('file', fileObj)

  // ─── datetime ────────────────────────────────────────────────────────────
  const dateObj = new Map()
  dateObj.set('now',     ()     => Date.now())
  dateObj.set('string',  (ms)   => new Date(ms ?? Date.now()).toISOString())
  dateObj.set('year',    (ms)   => new Date(ms ?? Date.now()).getFullYear())
  dateObj.set('month',   (ms)   => new Date(ms ?? Date.now()).getMonth() + 1)
  dateObj.set('day',     (ms)   => new Date(ms ?? Date.now()).getDate())
  dateObj.set('hour',    (ms)   => new Date(ms ?? Date.now()).getHours())
  dateObj.set('minute',  (ms)   => new Date(ms ?? Date.now()).getMinutes())
  dateObj.set('second',  (ms)   => new Date(ms ?? Date.now()).getSeconds())
  env.define('datetime', dateObj)

  return env
}

// ─── Const guard set ────────────────────────────────────────────────────────
// Tracks which names in each env are const.
// We store consts in a WeakMap keyed on Environment instances.
const CONSTS = new WeakMap()
function markConst(env, name) {
  if (!CONSTS.has(env)) CONSTS.set(env, new Set())
  CONSTS.get(env).add(name)
}
function isConst(env, name) {
  // Walk chain
  let e = env
  while (e) {
    if (CONSTS.has(e) && CONSTS.get(e).has(name)) return true
    e = e.parent
  }
  return false
}

// ─── Interpreter ───────────────────────────────────────────────────────────
class Interpreter {
  constructor(baseDir = process.cwd()) {
    this.globals = buildStdLib(this)
    this.baseDir = baseDir
  }

  stringify(v) {
    if (v === null)             return 'null'
    if (typeof v === 'boolean') return v ? 'true' : 'false'
    if (Array.isArray(v))       return '[' + v.map(x => this.stringify(x)).join(', ') + ']'
    if (v instanceof Map) {
      const pairs = [...v.entries()].map(([k, val]) => `${this.stringify(k)}: ${this.stringify(val)}`)
      return '{' + pairs.join(', ') + '}'
    }
    if (v instanceof LangGenerator) return '<generator>'
    if (v instanceof LangInstance) {
      const toStr = v.klass.findMethod('__str__')
      if (toStr) {
        const bound = v.bindMethod(toStr)
        const result = this.callFunction(bound, [])
        return typeof result === 'string' ? result : String(result)
      }
      return v.toString()
    }
    return String(v)
  }

  run(program) {
    return this.execBlock(program.body, this.globals)
  }

  execBlock(stmts, env) {
    let result = null
    for (const stmt of stmts) {
      result = this.exec(stmt, env)
      if (result instanceof ReturnSignal)   return result
      if (result instanceof BreakSignal)    return result
      if (result instanceof ContinueSignal) return result
      if (result instanceof ThrowSignal)    return result
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

      case 'ConstStatement': {
        const value = this.eval(node.value, env)
        env.define(node.name, value)
        markConst(env, node.name)
        return null
      }

      case 'FnDeclaration': {
        const fn = new LangFunction(
          node.name, node.params, node.body, env,
          node.defaults ?? {}, node.hasRest ?? false, node.restName ?? null
        )
        fn.isAsync = node.isAsync ?? false
        if (node.name) env.define(node.name, fn)
        return fn
      }

      case 'ClassDeclaration': {
        let superclass = null
        if (node.superclass) {
          superclass = env.get(node.superclass)
          if (!(superclass instanceof LangClass))
            throw new TripError('Runtime', `'${node.superclass}' is not a class`, node.line)
        }
        const methods = new Map()
        const statics = new Map()
        for (const stmt of node.body.body) {
          if (stmt.type === 'FnDeclaration') {
            const fn = new LangFunction(
              stmt.name, stmt.params, stmt.body, env,
              stmt.defaults ?? {}, stmt.hasRest ?? false, stmt.restName ?? null
            )
            if (stmt.isStatic) statics.set(stmt.name, fn)
            else               methods.set(stmt.name, fn)
          }
        }
        const klass = new LangClass(node.name, superclass, methods, statics)
        env.define(node.name, klass)
        return null
      }

      case 'ReturnStatement': {
        const value = node.value ? this.eval(node.value, env) : null
        return new ReturnSignal(value)
      }

      case 'IfStatement': {
        if (this.isTruthy(this.eval(node.condition, env))) {
          return this.execBlock(node.consequent.body, env.child())
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
        const loopScope = env.child()
        while (this.isTruthy(this.eval(node.condition, env))) {
          loopScope.vars.clear()
          const result = this.execBlock(node.body.body, loopScope)
          if (result instanceof BreakSignal)    break
          if (result instanceof ReturnSignal)   return result
          if (result instanceof ContinueSignal) continue
          if (result instanceof ThrowSignal)    return result
        }
        return null
      }

      case 'ForStatement': {
        const iterable = this.eval(node.iterable, env)
        let iterSource
        if (typeof iterable === 'string') {
          iterSource = iterable
        } else if (iterable && typeof iterable[Symbol.iterator] === 'function') {
          iterSource = iterable
        } else {
          iterSource = []
        }
        const loopScope = env.child()
        for (const item of iterSource) {
          loopScope.vars.clear()
          loopScope.define(node.variable, item)
          const result = this.execBlock(node.body.body, loopScope)
          if (result instanceof BreakSignal)    break
          if (result instanceof ReturnSignal)   return result
          if (result instanceof ContinueSignal) continue
          if (result instanceof ThrowSignal)    return result
        }
        return null
      }

      case 'BreakStatement':    return new BreakSignal()
      case 'ContinueStatement': return new ContinueSignal()

      case 'ThrowStatement': {
        const value = this.eval(node.value, env)
        return new ThrowSignal(value)
      }

      case 'TryCatchStatement': {
        let result = null
        let threw  = null

        try {
          result = this.execBlock(node.tryBlock.body, env.child())
          if (result instanceof ThrowSignal) {
            threw  = result.value
            result = null
          }
        } catch (err) {
          threw = (err instanceof TripError) ? err.tripMsg : err.message
        }

        if (threw !== null && node.catchBlock) {
          const catchScope = env.child()
          if (node.catchVar) catchScope.define(node.catchVar, threw)
          result = this.execBlock(node.catchBlock.body, catchScope)
        }

        if (node.finallyBlock) {
          const fr = this.execBlock(node.finallyBlock.body, env.child())
          if (fr instanceof ReturnSignal || fr instanceof BreakSignal) return fr
          if (fr instanceof ThrowSignal) return fr
        }

        return result
      }

      case 'ExprStatement':
        return this.eval(node.expr, env)

      case 'ImportStatement': {
        // Resolve path: stdlib/* paths are relative to interpreter dir,
        // everything else relative to the running script's dir.
        let filePath
        if (node.path.startsWith('stdlib/')) {
          const interpDir = path.dirname(require.resolve('./interpreter'))
          filePath = path.resolve(interpDir, node.path + '.tp')
        } else {
          filePath = path.resolve(this.baseDir, node.path + '.tp')
        }
        if (!fs.existsSync(filePath))
          throw new TripError('Runtime', `Cannot import '${node.path}': file not found`, node.line)
        const src = fs.readFileSync(filePath, 'utf8')
        const { Lexer }  = require('./lexer')
        const { Parser } = require('./parser')
        const tokens  = new Lexer(src).tokenize()
        const program = new Parser(tokens).parse()
        const modInterp = new Interpreter(path.dirname(filePath))
        modInterp.globals.parent = this.globals
        modInterp.run(program)
        for (const [k, v] of modInterp.globals.vars) {
          env.define(k, v)
        }
        return null
      }


      // ─── @decorator ─────────────────────────────────────────────────────
      // @dec        fn foo()  →  foo = dec(foo)
      // @dec(args)  fn foo()  →  foo = dec(args)(foo)
      case 'DecoratedFn': {
        let fn = this.exec(node.fn, env)
        for (let i = node.decorators.length - 1; i >= 0; i--) {
          const { name, args } = node.decorators[i]
          const dec      = env.get(name, node.line)
          const evalArgs = args.map(a => this.eval(a, env))
          if (evalArgs.length > 0) {
            // @dec(args) → factory pattern: dec(args) returns the actual decorator
            const factory = this.callFunction(dec, evalArgs, node.line)
            fn = this.callFunction(factory, [fn], node.line)
          } else {
            // @dec → direct decorator: dec(fn)
            fn = this.callFunction(dec, [fn], node.line)
          }
        }
        if (node.fn.name) env.define(node.fn.name, fn)
        return fn
      }

      case 'MatchStatement': {
        const subject = this.eval(node.subject, env)
        for (const { pattern, body } of node.cases) {
          const pval = this.eval(pattern, env)
          if (subject === pval) {
            return this.execBlock(body.body, env.child())
          }
        }
        if (node.elseBody) return this.execBlock(node.elseBody.body, env.child())
        return null
      }

      default:
        throw new TripError('Runtime', `Unknown statement type '${node.type}'`, node.line)
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

      // ─── f"Hello {name}!" ───────────────────────────────────────────────
      case 'FStringExpr': {
        const { Lexer }  = require('./lexer')
        const { Parser } = require('./parser')
        let out = ''
        for (const part of node.parts) {
          if (part.kind === 'text') {
            out += part.value
          } else {
            // Parse and evaluate the embedded expression
            try {
              const tokens = new Lexer(part.value).tokenize()
              const program = new Parser(tokens).parse()
              const exprNode = program.body[0]?.expr ?? program.body[0]
              const val = this.eval(exprNode, env)
              out += this.stringify(val)
            } catch (e) {
              throw new TripError('Runtime', `f-string expression error: ${e.message}`, node.line)
            }
          }
        }
        return out
      }

      // ─── Ternary: value if condition else fallback ───────────────────────
      case 'TernaryExpr': {
        const cond = this.eval(node.condition, env)
        return this.isTruthy(cond)
          ? this.eval(node.consequent, env)
          : this.eval(node.alternate, env)
      }

      // ─── Null coalescing: x ?? "default" ───────────────────────────────
      case 'NullCoalesceExpr': {
        const left = this.eval(node.left, env)
        return left !== null && left !== undefined
          ? left
          : this.eval(node.right, env)
      }

      // ─── x in container ────────────────────────────────────────────────
      case 'InExpr': {
        const item      = this.eval(node.item, env)
        const container = this.eval(node.container, env)
        if (Array.isArray(container)) return container.includes(item)
        if (typeof container === 'string') return container.includes(item)
        if (container instanceof Map) return container.has(item)
        throw new TripError('Runtime', "'in' requires array, string, or dict", node.line)
      }

      // ─── x is typeName ─────────────────────────────────────────────────
      case 'IsExpr': {
        const val      = this.eval(node.value, env)
        const typeName = node.typeName
        const actual   = (() => {
          if (val === null)                return 'null'
          if (typeof val === 'number')     return Number.isInteger(val) ? 'int' : 'float'
          if (typeof val === 'string')     return 'string'
          if (typeof val === 'boolean')    return 'bool'
          if (Array.isArray(val))          return 'array'
          if (val instanceof Map)          return 'dict'
          if (val instanceof LangFunction) return 'function'
          if (val instanceof LangClass)    return 'class'
          if (val instanceof LangInstance) return val.klass.name
          return 'unknown'
        })()
        // 'num' matches both int and float
        if (typeName === 'num') return actual === 'int' || actual === 'float'
        return actual === typeName
      }

      // ─── ++ / -- ───────────────────────────────────────────────────────
      case 'IncrDecrExpr': {
        const target = node.target
        const delta  = node.op === '++' ? 1 : -1
        const old    = this.eval(target, env)
        const next   = old + delta
        this.assign(target, next, '=', env)
        return node.prefix ? next : old
      }

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
                        ? this.stringify(l) + this.stringify(r) : l + r
          case '-':   return l - r
          case '*':   return l * r
          case '/':
            if (r === 0) throw new TripError('Runtime', 'Division by zero', node.line)
            return l / r
          case '%':   return l % r
          case '**':  return Math.pow(l, r)
          case '==':  return l === r
          case '!=':  return l !== r
          case '<':   return l < r
          case '>':   return l > r
          case '<=':  return l <= r
          case '>=':  return l >= r
          case 'and': return this.isTruthy(l) ? r : l
          case 'or':  return this.isTruthy(l) ? l : r
          default: throw new TripError('Runtime', `Unknown operator '${node.op}'`, node.line)
        }
      }

      case 'UnaryExpr': {
        const val = this.eval(node.operand, env)
        if (node.op === '-')   return -val
        if (node.op === 'not') return !this.isTruthy(val)
        break
      }

      case 'AssignExpr': {
        // Const guard
        if (node.target.type === 'Identifier' && isConst(env, node.target.name)) {
          throw new TripError('Runtime', `Cannot reassign const '${node.target.name}'`, node.target.line)
        }
        const value = this.eval(node.value, env)
        return this.assign(node.target, value, node.op, env)
      }

      case 'MemberExpr': {
        if (node.object.type === 'Identifier' && node.object.name === 'super') {
          const selfVal    = env.get('self')
          const superclass = selfVal.klass.superclass
          if (!superclass) throw new TripError('Runtime', 'No superclass to call', node.line)
          const method = superclass.findMethod(node.property)
          if (!method) throw new TripError('Runtime', `Superclass has no method '${node.property}'`, node.line)
          return selfVal.bindMethod(method)
        }

        const obj = this.eval(node.object, env)

        if (obj instanceof LangClass) {
          const staticFn = obj.findStatic(node.property)
          if (staticFn) return staticFn
          throw new TripError('Runtime', `Class '${obj.name}' has no static member '${node.property}'`, node.line)
        }

        if (obj instanceof LangInstance) return obj.get(node.property, node.line)
        if (obj instanceof Map)          return obj.get(node.property) ?? null
        return this.getMemberBuiltin(obj, node.property, node.line)
      }

      case 'IndexExpr': {
        const obj = this.eval(node.object, env)
        const idx = this.eval(node.index, env)
        if (Array.isArray(obj))      return obj[idx] ?? null
        if (obj instanceof Map)      return obj.get(idx) ?? null
        if (typeof obj === 'string') return obj[idx] ?? null
        throw new TripError('Runtime', 'Index operation on non-indexable type', node.line)
      }

      case 'CallExpr': {
        const callee = this.eval(node.callee, env)
        const args   = node.args.map(a => this.eval(a, env))
        return this.callFunction(callee, args, node.line)
      }

      case 'FnDeclaration':
        return this.exec(node, env)

      case 'YieldExpr': {
        const val = node.value ? this.eval(node.value, env) : null
        return new YieldSignal(val)
      }

      case 'AwaitExpr': {
        const val = this.eval(node.value, env)
        // Unwrap our synchronous async wrapper
        if (val instanceof AsyncValue) return val.value
        // Plain value passed to await — just return it (no-op await)
        return val
      }


      case 'ListComprehension': {
        const result   = []
        const iterable = this.eval(node.iterable, env)
        let iterSource = typeof iterable === 'string' ? iterable
          : (iterable && typeof iterable[Symbol.iterator] === 'function') ? iterable : []
        for (const item of iterSource) {
          const scope = env.child()
          scope.define(node.variable, item)
          if (node.condition && !this.isTruthy(this.eval(node.condition, scope))) continue
          result.push(this.eval(node.expr, scope))
        }
        return result
      }

      case 'DictComprehension': {
        const result   = new Map()
        const iterable = this.eval(node.iterable, env)
        let iterSource = typeof iterable === 'string' ? iterable
          : (iterable && typeof iterable[Symbol.iterator] === 'function') ? iterable : []
        for (const item of iterSource) {
          const scope = env.child()
          scope.define(node.variable, item)
          if (node.condition && !this.isTruthy(this.eval(node.condition, scope))) continue
          const k = this.eval(node.keyExpr, scope)
          const v = this.eval(node.valExpr, scope)
          result.set(k, v)
        }
        return result
      }

      default:
        throw new TripError('Runtime', `Unknown expression type '${node.type}'`, node.line)
    }
  }

  assign(target, value, op, env) {
    const compute = (old) => {
      if (op === '+=') return typeof old === 'string' ? old + this.stringify(value) : old + value
      if (op === '-=') return old - value
      if (op === '*=') return old * value
      if (op === '/=') {
        if (value === 0) throw new TripError('Runtime', 'Division by zero', target.line)
        return old / value
      }
      return value
    }

    if (target.type === 'Identifier') {
      if (op === '=') {
        if (env.has(target.name)) {
          env.updateIn(target.name, value)
          return value
        } else {
          env.define(target.name, value)
          return value
        }
      } else {
        if (env.has(target.name)) {
          const old  = env.get(target.name, target.line)
          const next = compute(old)
          env.updateIn(target.name, next)
          return next
        } else {
          env.define(target.name, compute(null))
          return env.get(target.name)
        }
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
    throw new TripError('Runtime', 'Invalid assignment target', target.line)
  }

  callFunction(callee, args, line) {
    if (typeof callee === 'function') return callee(...args) ?? null

    if (callee instanceof LangFunction) {
      // If the fn body contains yield → return a generator instead of executing
      if (this._fnIsGenerator(callee)) {
        return new LangGenerator(callee, args, this)
      }

      const scope = callee.closure.child()
      const bindableParams = callee.params.filter(p => p !== 'self')

      bindableParams.forEach((p, i) => {
        // Use provided arg, or default value, or null
        let val = args[i]
        if (val === undefined) {
          if (p in callee.defaults) {
            val = this.eval(callee.defaults[p], scope)
          } else {
            val = null
          }
        }
        scope.define(p, val)
      })

      // Rest parameter: collect remaining args
      if (callee.hasRest && callee.restName) {
        const restArgs = args.slice(bindableParams.length)
        scope.define(callee.restName, restArgs)
      }

      const result = this.execBlock(callee.body.body, scope)
      if (result instanceof ReturnSignal) {
        const retVal = result.value
        if (callee.isAsync) return new AsyncValue(retVal)
        return retVal
      }
      if (result instanceof ThrowSignal) {
        const msg = this.stringify(result.value)
        throw new TripError('Runtime', `Uncaught throw: ${msg}`, line)
      }
      return callee.isAsync ? new AsyncValue(null) : null
    }

    if (callee instanceof LangClass) {
      const instance = new LangInstance(callee)
      const init = callee.findMethod('init')
      if (init) {
        // Native init (e.g. Error base class)
        if (init._native) {
          init._native(instance, args)
        } else {
          const bound = instance.bindMethod(init)
          this.callFunction(bound, args, line)
        }
      }
      return instance
    }

    throw new TripError('Runtime', `'${this.stringify(callee)}' is not callable`, line)
  }

  getMemberBuiltin(obj, prop, line = null) {
    if (Array.isArray(obj)) {
      const methods = {
        push:     (...a) => { obj.push(...a); return obj },
        pop:      ()     => obj.pop(),
        length:   obj.length,
        map:      (fn)   => obj.map(v => this.callFunction(fn, [v])),
        filter:   (fn)   => obj.filter(v => this.isTruthy(this.callFunction(fn, [v]))),
        forEach:  (fn)   => { obj.forEach(v => this.callFunction(fn, [v])); return null },
        join:     (sep)  => obj.join(sep ?? ','),
        reverse:  ()     => [...obj].reverse(),
        includes: (v)    => obj.includes(v),
        indexOf:  (v)    => obj.indexOf(v),
        slice:    (a, b) => obj.slice(a, b),
        sort:     (fn)   => {
          const copy = [...obj]
          if (fn) copy.sort((a, b) => this.callFunction(fn, [a, b]))
          else copy.sort((a, b) => a < b ? -1 : a > b ? 1 : 0)
          return copy
        },
        flat:     ()     => obj.flat(),
        find:     (fn)   => obj.find(v => this.isTruthy(this.callFunction(fn, [v]))) ?? null,
        some:     (fn)   => obj.some(v => this.isTruthy(this.callFunction(fn, [v]))),
        every:    (fn)   => obj.every(v => this.isTruthy(this.callFunction(fn, [v]))),
      }
      return methods[prop] ?? null
    }
    if (typeof obj === 'string') {
      const methods = {
        length:     obj.length,
        upper:      () => obj.toUpperCase(),
        lower:      () => obj.toLowerCase(),
        trim:       () => obj.trim(),
        split:      (sep) => obj.split(sep ?? ''),
        includes:   (s)   => obj.includes(s),
        startsWith: (s)   => obj.startsWith(s),
        endsWith:   (s)   => obj.endsWith(s),
        replace:    (a,b) => obj.replace(a, b),
        replaceAll: (a,b) => obj.replaceAll(a, b),
        indexOf:    (s)   => obj.indexOf(s),
        slice:      (a,b) => obj.slice(a,b),
        repeat:     (n)   => obj.repeat(n),
        padStart:   (n,c) => obj.padStart(n, c ?? ' '),
        padEnd:     (n,c) => obj.padEnd(n, c ?? ' '),
        chars:      () => [...obj],
      }
      return methods[prop] ?? null
    }
    if (obj instanceof Map) {
      const methods = {
        has:    (k) => obj.has(k),
        get:    (k) => obj.get(k) ?? null,
        set:    (k, v) => { obj.set(k, v); return obj },
        delete: (k) => { obj.delete(k); return obj },
        keys:   () => [...obj.keys()],
        values: () => [...obj.values()],
        size:   obj.size,
      }
      return methods[prop] ?? null
    }
    return null
  }

  // ─── Generator execution ──────────────────────────────────────────────────
  // Walks statements as a JS generator, yielding values from YieldExpr nodes.
  *execGenerator(stmts, env) {
    for (const stmt of stmts) {
      const result = yield* this._execGenNode(stmt, env)
      if (result instanceof ReturnSignal) return
      if (result instanceof BreakSignal)  return result
    }
  }

  *_execGenNode(node, env) {
    if (node.type === 'ExprStatement') {
      const val = yield* this._evalGenExpr(node.expr, env)
      return val
    }
    if (node.type === 'Block') {
      yield* this.execGenerator(node.body, env)
      return
    }
    if (node.type === 'IfStatement') {
      if (this.isTruthy(this.eval(node.condition, env))) {
        yield* this.execGenerator(node.consequent.body, env.child())
      } else {
        for (const alt of node.alternates) {
          if (this.isTruthy(this.eval(alt.condition, env))) {
            yield* this.execGenerator(alt.body.body, env.child()); return
          }
        }
        if (node.final) yield* this.execGenerator(node.final.body, env.child())
      }
      return
    }
    if (node.type === 'WhileStatement') {
      while (this.isTruthy(this.eval(node.condition, env))) {
        const r = yield* this.execGenerator(node.body.body, env.child())
        if (r instanceof BreakSignal) break
      }
      return
    }
    if (node.type === 'ForStatement') {
      const iterable = this.eval(node.iterable, env)
      const src = typeof iterable === 'string' ? iterable
        : (iterable && typeof iterable[Symbol.iterator] === 'function') ? iterable : []
      for (const item of src) {
        const scope = env.child()
        scope.define(node.variable, item)
        const r = yield* this.execGenerator(node.body.body, scope)
        if (r instanceof BreakSignal) break
      }
      return
    }
    // For all other statement types fall back to normal exec
    return this.exec(node, env)
  }

  *_evalGenExpr(node, env) {
    if (node.type === 'YieldExpr') {
      const val = node.value ? this.eval(node.value, env) : null
      yield val
      return val
    }
    return this.eval(node, env)
  }

  // Checks if a LangFunction's body contains any YieldExpr (makes it a generator)
  _fnIsGenerator(fn) {
    return this._bodyHasYield(fn.body.body)
  }

  _bodyHasYield(stmts) {
    for (const stmt of stmts) {
      if (this._nodeHasYield(stmt)) return true
    }
    return false
  }

  _nodeHasYield(node) {
    if (!node || typeof node !== 'object') return false
    if (node.type === 'YieldExpr') return true
    // Don't descend into nested fn declarations (their yields are their own)
    if (node.type === 'FnDeclaration') return false
    for (const val of Object.values(node)) {
      if (Array.isArray(val) && val.some(v => this._nodeHasYield(v))) return true
      if (val && typeof val === 'object' && val.type && this._nodeHasYield(val)) return true
    }
    return false
  }

  isTruthy(v) {
    if (v === null || v === false)          return false
    if (typeof v === 'number' && v === 0)   return false
    if (typeof v === 'string' && v === '')  return false
    if (Array.isArray(v) && v.length === 0) return false
    return true
  }
}

module.exports = { Interpreter, LangFunction, LangClass, LangInstance, buildStdLib, TripError }