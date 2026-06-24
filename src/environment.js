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
    // If not found anywhere, create in current scope (for self.x style property sets handled by interpreter)
    throw new Error(`[Runtime] Line ${line ?? '?'}: Cannot assign to undeclared variable '${name}'`)
  }

  // Check without throwing
  has(name) {
    if (this.vars.has(name)) return true
    if (this.parent) return this.parent.has(name)
    return false
  }

  child() {
    return new Environment(this)
  }
}

module.exports = { Environment }
