// ─── Environment (Scope) ───────────────────────────────────────────────────
// Each scope has a reference to its enclosing scope.
// Variable lookup walks up the chain until found.

class Environment {
  constructor(parent = null) {
    this.vars   = new Map()
    this.parent = parent
  }

  // Define a new variable in the current scope
  define(name, value) {
    this.vars.set(name, value)
    return value
  }

  // Look up a variable, walking up the scope chain
  get(name, line) {
    if (this.vars.has(name)) return this.vars.get(name)
    if (this.parent) return this.parent.get(name, line)
    throw new Error(`[Runtime] Line ${line ?? '?'}: Undefined variable '${name}'`)
  }

  // Assign to an existing variable (must already exist somewhere in chain)
  set(name, value, line) {
    if (this.vars.has(name)) { this.vars.set(name, value); return value }
    if (this.parent) return this.parent.set(name, value, line)
    throw new Error(`[Runtime] Line ${line ?? '?'}: Cannot assign to undeclared variable '${name}'`)
  }

  // Check without throwing
  has(name) {
    if (this.vars.has(name)) return true
    if (this.parent) return this.parent.has(name)
    return false
  }

  // PERF: update an existing variable in ONE chain walk instead of three
  // (the old path was: has()[walk] + get()[walk] + set()[walk]).
  // Returns the new value, or undefined if the name wasn't found anywhere.
  updateIn(name, newValue) {
    let e = this
    while (e) {
      if (e.vars.has(name)) { e.vars.set(name, newValue); return newValue }
      e = e.parent
    }
    return undefined // not found — caller falls back to define()
  }

  child() {
    return new Environment(this)
  }
}

module.exports = { Environment }
