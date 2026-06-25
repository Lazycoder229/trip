#!/usr/bin/env node
const fs   = require('fs')
const path = require('path')
const { performance } = require('perf_hooks')
const { Lexer }       = require('./src/lexer')
const { Parser }      = require('./src/parser')
const { Interpreter } = require('./src/interpreter')

// Reports how long Trip itself took (lex + parse + execute), as opposed to
// how long the OS took to spawn+boot this Node process. Sent over the IPC
// channel (process.send), not stdout — so it never mixes with the script's
// own print()/println() output. process.send only exists when the parent
// spawned us with an 'ipc' stdio pipe; the IDE does, plain `node trip.js`
// from a terminal does not, so this is a no-op there.
function reportExecTime(ms) {
  if (typeof process.send === 'function') {
    try { process.send({ type: 'trip:exec-time', ms }) } catch (_) { /* parent gone, ignore */ }
  }
}

function run(source, filePath = process.cwd()) {
  const start = performance.now()
  try {
    const tokens      = new Lexer(source).tokenize()
    const ast         = new Parser(tokens).parse()
    const interpreter = new Interpreter(path.dirname(path.resolve(filePath)))
    interpreter.run(ast)
    reportExecTime(performance.now() - start)
  } catch (err) {
    reportExecTime(performance.now() - start)
    console.error('\x1b[31m' + err.message + '\x1b[0m')
    process.exit(1)
  }
}

// ─── REPL ──────────────────────────────────────────────────────────────────
function repl() {
  const { Interpreter } = require('./src/interpreter')
  const interp = new Interpreter()

  console.log('Welcome to Trip REPL. Type exit to quit.')
  console.log('  Multi-line blocks: finish with a blank line to run.\n')

  process.stdin.setEncoding('utf8')
  process.stdin.resume()

  let stdinBuf   = ''
  let multiLines = []   // lines collected for the current block
  let inBlock    = false

  function prompt() {
    process.stdout.write(inBlock ? '... ' : '>>> ')
  }

  function executeBlock() {
    const source = multiLines.join('\n')
    multiLines = []
    inBlock    = false
    if (!source.trim()) { prompt(); return }
    try {
      const tokens = new Lexer(source).tokenize()
      const ast    = new Parser(tokens).parse()
      const result = interp.run(ast)
      if (result !== null && result !== undefined) console.log('=>', result)
    } catch (e) {
      console.error('\x1b[31m' + e.message + '\x1b[0m')
    }
    prompt()
  }

  prompt()

  process.stdin.on('data', (chunk) => {
    stdinBuf += chunk
    const lines = stdinBuf.split('\n')
    stdinBuf = lines.pop() // keep any incomplete trailing chunk

    for (const raw of lines) {
      const line = raw.replace(/\r/g, '').trimEnd()

      // 'exit' always quits, even mid-block
      if (line.trim() === 'exit') {
        console.log('Bye!')
        process.exit(0)
      }

      if (inBlock) {
        // Blank line = done typing the block, execute it now
        if (line.trim() === '') {
          executeBlock()
        } else {
          multiLines.push(line)
          prompt()
        }
      } else {
        if (line.trim() === '') { prompt(); continue }

        multiLines.push(line)

        // A line ending with ':' (ignoring inline comments) opens a block —
        // switch to '...' mode and wait for lines + blank line to terminate
        const stripped = line.replace(/#.*$/, '').trimEnd()
        if (stripped.endsWith(':')) {
          inBlock = true
          prompt()
        } else {
          // Plain single-line statement — execute right away
          executeBlock()
        }
      }
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