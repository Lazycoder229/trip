// ─── Language Info ──────────────────────────────────────────────────────────
// Single source of truth for anything that describes the Trip language to
// *external tooling* — the IDE's syntax highlighter, hover docs, and any
// future linter or doc generator.
//
// Nothing outside this file should hardcode a keyword or builtin name.
// Add a feature once (in lexer.js or interpreter.js), and it shows up here
// automatically — you only ever write a human description here, never a name.
//
// This file is also the seam for the day Trip becomes its own published
// package: this is the metadata API a consumer (e.g. the IDE, as a
// dependency of `trip`) would import.

const { KEYWORDS }   = require('./lexer')
const { buildStdLib } = require('./interpreter')

// Builtin names come straight from the real standard library — if you add
// `env.define('foo', ...)` in interpreter.js, 'foo' appears here with no
// extra step. Only the *description* below is something you add by hand.
function getBuiltinNames() {
  return [...buildStdLib().vars.keys()]
}

// kind, signature, description — used for hover tooltips.
// Missing entries simply get no tooltip; the keyword/builtin still works,
// it just won't have docs until someone fills this in.
const DOCS = {
  let:      ['keyword', '`let name = value`', 'Declares a new variable in the current scope.'],
  fn:       ['keyword', '`fn name(params) { ... }`', 'Declares a function.'],
  return:   ['keyword', '`return value`', 'Returns a value from the enclosing function.'],
  if:       ['keyword', '`if cond { ... }`', 'Conditional branch.'],
  else:     ['keyword', '`else { ... }`', 'Fallback branch for an `if`.'],
  elif:     ['keyword', '`elif cond { ... }`', 'Additional conditional branch.'],
  while:    ['keyword', '`while cond { ... }`', 'Loops while the condition is true.'],
  for:      ['keyword', '`for item in iterable { ... }`', 'Iterates over an iterable.'],
  in:       ['keyword', 'used with `for`', 'Specifies the iterable in a for-loop.'],
  class:    ['keyword', '`class Name { ... }`', 'Declares a class.'],
  self:     ['keyword', 'instance reference', 'Refers to the current class instance.'],
  new:      ['keyword', '`new ClassName(...)`', 'Instantiates a class.'],
  import:   ['keyword', '`import "module"`', 'Imports another module.'],
  break:    ['keyword', '`break`', 'Exits the nearest enclosing loop.'],
  continue: ['keyword', '`continue`', 'Skips to the next loop iteration.'],
  true:     ['constant', 'boolean literal', 'The boolean value true.'],
  false:    ['constant', 'boolean literal', 'The boolean value false.'],
  null:     ['constant', 'null literal', 'Represents the absence of a value.'],
  and:      ['operator', '`a and b`', 'Logical AND.'],
  or:       ['operator', '`a or b`', 'Logical OR.'],
  not:      ['operator', '`not a`', 'Logical NOT.'],

  print:    ['builtin', '`print(value)`', 'Writes a value to stdout (no trailing newline).'],
  println:  ['builtin', '`println(value)`', 'Writes a value to stdout with a trailing newline.'],
  input:    ['builtin', '`input()`', 'Placeholder — synchronous stdin input is not yet implemented.'],
  str:      ['builtin', '`str(x) -> string`', 'Converts a value to a string.'],
  num:      ['builtin', '`num(x) -> number`', 'Converts a value to a number.'],
  bool:     ['builtin', '`bool(x) -> bool`', 'Converts a value to a boolean.'],
  len:      ['builtin', '`len(x) -> num`', 'Returns the length of a string, array, or map.'],
  type:     ['builtin', '`type(x) -> string`', 'Returns the runtime type name of a value.'],
  range:    ['builtin', '`range(start, end, step?)`', 'Produces a sequence of numbers.'],
  memory:   ['builtin', '`memory()`', 'Debug: prints every global variable and its value.'],
  push:     ['builtin', '`push(array, item)`', 'Appends an item to an array.'],
  pop:      ['builtin', '`pop(array)`', 'Removes and returns the last item of an array.'],
  keys:     ['builtin', '`keys(map) -> array`', "Returns a map's keys."],
  values:   ['builtin', '`values(map) -> array`', "Returns a map's values."],
  split:    ['builtin', '`split(str, sep) -> array`', 'Splits a string into an array.'],
  join:     ['builtin', '`join(array, sep) -> string`', 'Joins an array into a string.'],
  upper:    ['builtin', '`upper(s) -> string`', 'Uppercases a string.'],
  lower:    ['builtin', '`lower(s) -> string`', 'Lowercases a string.'],
  trim:     ['builtin', '`trim(s) -> string`', 'Removes leading/trailing whitespace.'],
  floor:    ['builtin', '`floor(n) -> number`', 'Rounds down.'],
  ceil:     ['builtin', '`ceil(n) -> number`', 'Rounds up.'],
  round:    ['builtin', '`round(n) -> number`', 'Rounds to the nearest integer.'],
  abs:      ['builtin', '`abs(n) -> number`', 'Absolute value.'],
  sqrt:     ['builtin', '`sqrt(n) -> number`', 'Square root.'],
  random:   ['builtin', '`random() -> number`', 'Random float between 0 and 1.'],
  max:      ['builtin', '`max(a, b, ...) -> number`', 'Largest of the given values.'],
  min:      ['builtin', '`min(a, b, ...) -> number`', 'Smallest of the given values.'],
}

function getLanguageInfo() {
  return {
    keywords: [...KEYWORDS],
    builtins: getBuiltinNames(),
    docs: DOCS,
  }
}

module.exports = { getLanguageInfo }
