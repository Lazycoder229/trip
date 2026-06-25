const { Environment } = require('./environment')
const path = require('path')
const fs   = require('fs')

// ─── Control flow signals ──────────────────────────────────────────────────
class ReturnSignal   { constructor(value) { this.value = value } }
class BreakSignal    {}
class ContinueSignal {}
class ThrowSignal    { constructor(value) { this.value = value } }

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

class LangInstance {
  constructor(klass) {
    this.klass  = klass
    this.fields = new Map()
  }
  toString() { return `<${this.klass.name} instance>` }
  get(name) {
    if (this.fields.has(name)) return this.fields.get(name)
    const method = this.klass.findMethod(name)
    if (method) return this.bindMethod(method)
    throw new Error(`[Runtime]: '${this.klass.name}' has no attribute '${name}'`)
  }
  set(name, value) { this.fields.set(name, value); return value }
  bindMethod(fn) {
    const env = fn.closure.child()
    env.define('self', this)
    return new LangFunction(fn.name, fn.params, fn.body, env)
  }
}

// ─── Standard Library ──────────────────────────────────────────────────────
// NOTE: accepts the interpreter instance so print/str can invoke __str__
function buildStdLib(interp) {
  const env = new Environment()

  env.define('print',   (...args) => { process.stdout.write(args.map(v => interp.stringify(v)).join(' ') + '\n'); return null })
  env.define('println', (...args) => { process.stdout.write(args.map(v => interp.stringify(v)).join('') + '\n'); return null })
  env.define('input',   () => '')
  env.define('str',     (v) => interp.stringify(v))
  env.define('num',     (v) => parseFloat(v))
  env.define('int',     (v) => Math.trunc(typeof v === 'string' ? parseFloat(v) : v))
  env.define('float',   (v) => parseFloat(v))
  env.define('bool',    (v) => !!v)
  env.define('len', (v) => {
    if (Array.isArray(v))    return v.length
    if (typeof v === 'string') return v.length
    if (v instanceof Map)    return v.size
    throw new Error('[Runtime]: len() requires string, array, or dict')
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
  // FIX: 'memory' now correctly references interp instead of an undefined variable
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

  return env
}

// ─── Interpreter ───────────────────────────────────────────────────────────
class Interpreter {
  constructor(baseDir = process.cwd()) {
    // FIX: pass 'this' so stdlib closures can call this.stringify()
    this.globals = buildStdLib(this)
    this.baseDir = baseDir
  }

  // FIX: stringify is now an instance method so it can call __str__ via callFunction
  stringify(v) {
    if (v === null)             return 'null'
    if (typeof v === 'boolean') return v ? 'true' : 'false'
    if (Array.isArray(v))       return '[' + v.map(x => this.stringify(x)).join(', ') + ']'
    if (v instanceof Map) {
      const pairs = [...v.entries()].map(([k, val]) => `${this.stringify(k)}: ${this.stringify(val)}`)
      return '{' + pairs.join(', ') + '}'
    }
    if (v instanceof LangInstance) {
      const toStr = v.klass.findMethod('__str__')
      if (toStr) {
        // FIX: we can now actually invoke __str__ since we're inside the interpreter
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
        const statics = new Map()
        for (const stmt of node.body.body) {
          if (stmt.type === 'FnDeclaration') {
            const fn = new LangFunction(stmt.name, stmt.params, stmt.body, env)
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
          // FIX: propagate throws out of loop bodies
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
          // FIX: propagate throws out of loop bodies
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
          // ThrowSignal from execBlock — treat as a caught throw
          if (result instanceof ThrowSignal) {
            threw  = result.value
            result = null
          }
        } catch (err) {
          // Native JS errors (e.g. divide by zero, undefined variable) surface here
          threw = err.message
        }

        if (threw !== null && node.catchBlock) {
          const catchScope = env.child()
          if (node.catchVar) catchScope.define(node.catchVar, threw)
          result = this.execBlock(node.catchBlock.body, catchScope)
        }

        if (node.finallyBlock) {
          // finally always runs; only hijacks control for return/break
          const fr = this.execBlock(node.finallyBlock.body, env.child())
          if (fr instanceof ReturnSignal || fr instanceof BreakSignal) return fr
          // FIX: a throw inside finally also propagates
          if (fr instanceof ThrowSignal) return fr
        }

        return result
      }

      case 'ExprStatement':
        return this.eval(node.expr, env)

      case 'ImportStatement': {
        const filePath = path.resolve(this.baseDir, node.path + '.tp')
        if (!fs.existsSync(filePath))
          throw new Error(`[Runtime]: Cannot import '${node.path}': file not found`)
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
                        ? this.stringify(l) + this.stringify(r) : l + r
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
        // FIX: super.method — resolve against superclass, bound to self
        if (node.object.type === 'Identifier' && node.object.name === 'super') {
          const selfVal    = env.get('self')
          const superclass = selfVal.klass.superclass
          if (!superclass) throw new Error('[Runtime]: No superclass to call')
          const method = superclass.findMethod(node.property)
          if (!method) throw new Error(`[Runtime]: Superclass has no method '${node.property}'`)
          return selfVal.bindMethod(method)
        }

        const obj = this.eval(node.object, env)

        // FIX: static method / property access on a class (ClassName.staticMethod())
        if (obj instanceof LangClass) {
          const staticFn = obj.findStatic(node.property)
          if (staticFn) return staticFn
          throw new Error(`[Runtime]: Class '${obj.name}' has no static member '${node.property}'`)
        }

        if (obj instanceof LangInstance) return obj.get(node.property)
        if (obj instanceof Map)          return obj.get(node.property) ?? null
        return this.getMemberBuiltin(obj, node.property)
      }

      case 'IndexExpr': {
        const obj = this.eval(node.object, env)
        const idx = this.eval(node.index, env)
        if (Array.isArray(obj))      return obj[idx] ?? null
        if (obj instanceof Map)      return obj.get(idx) ?? null
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
      if (op === '+=') return typeof old === 'string' ? old + this.stringify(value) : old + value
      if (op === '-=') return old - value
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
    throw new Error('[Runtime]: Invalid assignment target')
  }

  callFunction(callee, args, line) {
    if (typeof callee === 'function') return callee(...args) ?? null

    if (callee instanceof LangFunction) {
      const scope = callee.closure.child()
      const bindableParams = callee.params.filter(p => p !== 'self')
      bindableParams.forEach((p, i) => scope.define(p, args[i] ?? null))
      const result = this.execBlock(callee.body.body, scope)
      if (result instanceof ReturnSignal) return result.value
      // FIX: re-surface ThrowSignal as a real JS error so the JS try/catch
      // in TryCatchStatement can intercept it, AND so uncaught throws
      // produce a proper error message instead of silently returning null
      if (result instanceof ThrowSignal) {
        const msg = this.stringify(result.value)
        throw new Error(msg)
      }
      return null
    }

    if (callee instanceof LangClass) {
      const instance = new LangInstance(callee)
      const init = callee.findMethod('init')
      if (init) {
        const bound = instance.bindMethod(init)
        this.callFunction(bound, args, line)
      }
      return instance
    }

    throw new Error(`[Runtime] Line ${line}: '${this.stringify(callee)}' is not callable`)
  }

  getMemberBuiltin(obj, prop) {
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
        indexOf:    (s)   => obj.indexOf(s),
      }
      return methods[prop] ?? null
    }
    return null
  }

  isTruthy(v) {
    if (v === null || v === false)                    return false
    if (typeof v === 'number' && v === 0)             return false
    if (typeof v === 'string' && v === '')            return false
    if (Array.isArray(v) && v.length === 0)           return false
    return true
  }
}

module.exports = { Interpreter, LangFunction, LangClass, LangInstance, buildStdLib }