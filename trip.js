#!/usr/bin/env node
const fs   = require('fs')
const path = require('path')
const { Lexer }       = require('./src/lexer')
const { Parser }      = require('./src/parser')
const { Interpreter } = require('./src/interpreter')

function run(source, filePath = process.cwd()) {
  try {
    const tokens      = new Lexer(source).tokenize()
    const ast         = new Parser(tokens).parse()
    const interpreter = new Interpreter(path.dirname(path.resolve(filePath)))
    interpreter.run(ast)
  } catch (err) {
    console.error('\x1b[31m' + err.message + '\x1b[0m')
    process.exit(1)
  }
}

// ─── REPL ──────────────────────────────────────────────────────────────────
function repl() {
  const { Interpreter } = require('./src/interpreter')
  const interp = new Interpreter()

  console.log('🌀  Welcome to Trip REPL. Type exit to quit.\n')

  // Use raw stdin instead of readline to avoid pkg issues
  process.stdout.write('>>> ')
  process.stdin.setEncoding('utf8')
  process.stdin.resume()

  let buffer = ''
  process.stdin.on('data', (chunk) => {
    buffer += chunk
    const lines = buffer.split('\n')
    buffer = lines.pop() // keep incomplete line in buffer

    for (const raw of lines) {
      const line = raw.replace(/\r/g, '').trimEnd()
      if (line.trim() === 'exit') {
        console.log('Bye!')
        process.exit(0)
      }
      if (line.trim() === '') {
        process.stdout.write('>>> ')
        continue
      }
      try {
        const tokens = new Lexer(line).tokenize()
        const ast    = new Parser(tokens).parse()
        const result = interp.run(ast)
        if (result !== null && result !== undefined) console.log('=>', result)
      } catch (e) {
        console.error('\x1b[31m' + e.message + '\x1b[0m')
      }
      process.stdout.write('>>> ')
    }
  })
}

// ─── Entry ─────────────────────────────────────────────────────────────────
const args = process.argv.slice(2)

if (args.length === 0) {
  repl()
} else {
  const filePath = path.resolve(args[0])
  if (!fs.existsSync(filePath)) {
    console.error(`File not found: ${filePath}`)
    process.exit(1)
  }
  const source = fs.readFileSync(filePath, 'utf8')
  run(source, filePath)
}