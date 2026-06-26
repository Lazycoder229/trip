#!/usr/bin/env node
const fs   = require('fs')
const path = require('path')
const { performance } = require('perf_hooks')
const { Lexer }       = require('./src/lexer')
const { Parser }      = require('./src/parser')
const { Interpreter, TripError } = require('./src/interpreter')

function reportExecTime(ms) {
  if (typeof process.send === 'function') {
    try { process.send({ type: 'trip:exec-time', ms }) } catch (_) {}
  }
}

// ─── Colored error output ──────────────────────────────────────────────────
// ─── Colored error output ──────────────────────────────────────────────────
function printError(err) {
  if (err instanceof TripError) {
    const location = err.tripLine != null ? ` Line ${err.tripLine} →` : ''
    console.error(`[${err.phase} Error]${location} ${err.tripMsg}`)
    return
  }

  const structured = err.message.match(/^\[(Lexer|Parser|Runtime)\](?: Line (\d+):)? (.+)$/)
  if (structured) {
    const [, phase, line, msg] = structured
    const location = line ? ` Line ${line} →` : ''
    console.error(`[${phase} Error]${location} ${msg}`)
    return
  }

  console.error(`[Internal Error] ${err.message}`)
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
    printError(err)
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
  let multiLines = []
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
      printError(e)
    }
    prompt()
  }

  prompt()

  process.stdin.on('data', (chunk) => {
    stdinBuf += chunk
    const lines = stdinBuf.split('\n')
    stdinBuf = lines.pop()

    for (const raw of lines) {
      const line = raw.replace(/\r/g, '').trimEnd()

      if (line.trim() === 'exit') {
        console.log('Bye!')
        process.exit(0)
      }

      if (inBlock) {
        if (line.trim() === '') {
          executeBlock()
        } else {
          multiLines.push(line)
          prompt()
        }
      } else {
        if (line.trim() === '') { prompt(); continue }

        multiLines.push(line)

        const stripped = line.replace(/#.*$/, '').trimEnd()
        if (stripped.endsWith(':')) {
          inBlock = true
          prompt()
        } else {
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
    console.error(`\x1b[31m[Error]\x1b[0m File not found: ${filePath}`)
    process.exit(1)
  }
  const source = fs.readFileSync(filePath, 'utf8')
  run(source, filePath)
}