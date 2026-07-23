# Trip Language

Trip is a **bytecode-compiled, dynamically-typed scripting language** with Python-style indentation blocks. It compiles to a custom virtual machine (the TVM — Trip Virtual Machine) using a single-pass Pratt parser that emits bytecode directly with no intermediate AST. Source files use the `.tp` extension.

---

## Table of Contents

- [Getting Started](#getting-started)
- [Syntax Overview](#syntax-overview)
- [Variables & Scope](#variables--scope)
- [Types](#types)
- [String Literals](#string-literals)
- [Operators](#operators)
- [Control Flow](#control-flow)
- [Functions](#functions)
- [Object-Oriented Programming](#object-oriented-programming)
- [Error Handling](#error-handling)
- [Concurrency](#concurrency)
- [Collections](#collections)
- [String Methods](#string-methods)
- [List Methods](#list-methods)
- [Dict Methods](#dict-methods)
- [Standard Library](#standard-library)
- [Networking](#networking)
- [MySQL Database](#mysql-database)
- [Module System](#module-system)
- [Tooling](#tooling)
- [VM Architecture](#vm-architecture)

---

## Getting Started

```
trip              # Start the interactive REPL
trip script.tp    # Run a .tp source file
trip test         # Discover and run *_test.tp / *.test.tp files
```

A minimal program:

```trip
outn("Hello, world!")
```

---

## Syntax Overview

- **Indentation-based blocks** — Python-style INDENT/DEDENT; no braces for blocks
- **No semicolons** — newlines are statement terminators
- **C-family operators** — `!`, `!=`, `++`, `--`, `+=`, etc.
- **Comments:**
  ```trip
  # This is a line comment
  /* This is a
     block comment */
  ```
- Multi-line expressions inside `(`, `[`, or `{` do **not** require backslash continuation — the scanner suppresses newlines while any bracket pair is open

---

## Variables & Scope

```trip
let x = 10          # mutable
const PI = 3.14159  # immutable — reassignment is a compile-time error

x = 20              # ok
PI = 1              # ERROR: Cannot reassign a const variable
```

- Lexical block scope — every indented block introduces a new scope
- Full **closure capture via upvalues**: inner functions capture outer variables; closed-over values are heap-allocated when the inner function outlives the enclosing scope
- Global and local variable resolution happens at compile time

---

## Types

| Type | Description |
|------|-------------|
| `bool` | `true` or `false` |
| `number` | IEEE 754 double — covers integers, floats, and all numeric math |
| `char` | Single character, e.g. `'a'`, `'\n'` |
| `nil` | Absent / null value |
| `string` | Heap-allocated, immutable sequence of characters |
| `list` | Dynamic array |
| `dict` | Hash table (string or value keys) |
| `class` / `instance` | User-defined objects |
| `closure` | First-class function value |
| `socket` | TCP or TLS socket handle |
| `db_conn` | MySQL connection or prepared-statement handle |

Use `typeof` to get the type name at runtime:

```trip
outn(typeof 42)        # "number"
outn(typeof "hello")   # "string"
outn(typeof [1, 2])    # "list"
outn(typeof nil)       # "nil"
```

---

## String Literals

Trip has four distinct string literal forms:

### Regular strings
```trip
let name = "Alice"
let tab  = "column1\tcolumn2"
let path = "C:\\Users\\Alice"
```
Supports escape sequences: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`, `\'`.

### F-strings (interpolation)
```trip
let age = 30
let msg = f"Hello, {name}! You are {age * 2} years younger than 90."
```
Any expression — including function calls and nested strings — is valid inside `{}`.

### Triple-quoted strings
```trip
let html = """
    <div>
        <p>Hello</p>
    </div>
    """
```
- One leading and one trailing newline are stripped automatically
- The minimum indentation of all non-empty lines is removed (equivalent to `textwrap.dedent` in Python)
- Supports the same escape sequences as regular strings

### Raw strings
```trip
let pattern = r"\d+\.\d+"   # backslashes are literal — no escape processing
```

### Char literals
```trip
let ch  = 'A'
let nl  = '\n'
let tab = '\t'
```

---

## Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+ - * / %` |
| Comparison | `== != < <= > >=` |
| Logical | `and or not !` |
| Assignment | `= += -= *= /=` |
| Increment / Decrement | `++ --` (postfix) |
| Range | `..` (used in `for x in start..end`) |
| Variadic spread | `...` (in function parameter lists) |
| Property access | `.name` |
| Index | `[i]` |
| Type inspection | `typeof` |

---

## Control Flow

### if / elif / else
```trip
if score >= 90
    outn("A")
elif score >= 80
    outn("B")
elif score >= 70
    outn("C")
else
    outn("F")
```

### while loop
```trip
let i = 0
while i < 5
    outn(i)
    i++
```

**Loop `else` clause** — runs only if the loop completed without `break`:
```trip
while i < 10
    if i == 7
        break
    i++
else
    outn("Loop finished without break")
```

### for — collection iteration
```trip
let fruits = ["apple", "banana", "cherry"]
for fruit in fruits
    outn(fruit)
```

### for — range iteration
```trip
for i in 0..10        # 0 inclusive, 10 exclusive
    outn(i)
```

Range loops also support `break`, `continue`, and an `else` clause.

### match / case
```trip
match status
    case 200
        outn("OK")
    case 404
        outn("Not Found")
    case 500
        outn("Server Error")
    else
        outn("Unknown status")
```
Uses value equality. Up to 256 cases per `match` block.

### break / continue
```trip
for i in 0..100
    if i % 2 == 0
        continue       # skip even numbers
    if i > 9
        break          # stop after 9
    outn(i)
```

---

## Functions

### Named functions
```trip
fn greet(name)
    outn(f"Hello, {name}!")

greet("Alice")
```

### Default parameter values
Only compile-time constants (numbers, strings, bools, nil, chars, negative numbers) are accepted as defaults. All required parameters must come before defaulted ones.

```trip
fn connect(host, port = 8080, secure = false)
    outn(f"Connecting to {host}:{port}")

connect("localhost")            # port = 8080, secure = false
connect("example.com", 443, true)
```

### Variadic parameters
```trip
fn sum(...nums)
    let total = 0
    for n in nums
        total += n
    return total

outn(sum(1, 2, 3, 4, 5))   # 15
```
`...params` must be the last parameter and cannot have a default value.

### Lambda — arrow form
```trip
let double = fn(x) => x * 2
let add    = fn(a, b) => a + b

outn(double(5))    # 10
outn(add(3, 4))    # 7
```

### Lambda — block form
```trip
let process = fn(x)
    let y = x * 2
    return y + 1

outn(process(5))   # 11
```

### First-class functions
```trip
fn apply(f, value)
    return f(value)

let result = apply(fn(x) => x * x, 6)
outn(result)   # 36
```

### Closures
```trip
fn makeCounter()
    let count = 0
    return fn()
        count++
        return count

let counter = makeCounter()
outn(counter())   # 1
outn(counter())   # 2
outn(counter())   # 3
```

---

## Object-Oriented Programming

```trip
class Animal
    fn init(name, sound)
        self.name  = name
        self.sound = sound

    fn speak()
        outn(f"{self.name} says {self.sound}!")

    fn getName()
        return self.name

let dog = Animal("Rex", "woof")
dog.speak()                  # Rex says woof!
outn(dog.getName())          # Rex
dog.name = "Max"             # property set
outn(dog.name)               # Max
```

- `class Name:` with an indented block of method definitions
- `fn init(...)` is the constructor — returns `self` automatically
- `self` is the implicit receiver inside every method (no `this`)
- Property get (`instance.prop`) and set (`instance.prop = val`) work directly
- Classes are first-class values and can be stored in variables, passed to functions, and returned

---

## Error Handling

### try / catch
```trip
try
    let result = riskyOperation()
    outn(result)
catch err
    outn(f"Caught: {err}")
```

### try / catch / finally
```trip
try
    openConnection()
    doWork()
catch err
    outn(f"Error: {err}")
finally
    closeConnection()   # always runs
```

### try / finally (no catch)
```trip
try
    doWork()
finally
    cleanup()           # always runs; errors propagate after
```

### throw
```trip
fn divide(a, b)
    if b == 0
        throw "Division by zero"
    return a / b

try
    outn(divide(10, 0))
catch err
    outn(err)   # Division by zero
```

Any value — string, number, dict, instance — can be thrown.

---

## Concurrency

Trip uses a **cooperative fiber scheduler** (green threads). Fibers share one OS thread and yield to each other explicitly or when blocked on I/O.

### spawn
```trip
fn worker(id)
    outn(f"Worker {id} starting")
    yield()
    outn(f"Worker {id} done")

spawn worker(1)
spawn worker(2)
waitAll()
```

### async / await
```trip
async fn fetchData(url)
    let resp = await httpGet(url)
    return resp

let data = fetchData("https://api.example.com/data")
waitAll()
```

### Built-in concurrency functions

| Function | Description |
|----------|-------------|
| `spawn fn(args)` | Launch a function as a new fiber |
| `yield()` | Suspend current fiber; let others run |
| `waitAll()` | Block until all spawned fibers complete |

The I/O reactor uses `poll()` for socket-backed fibers and `curl_multi` for HTTP fibers, so blocking on network I/O automatically yields to other fibers.

---

## Collections

### List
```trip
let nums = [10, 20, 30, 40]
nums.append(50)
outn(nums[0])          # 10
nums[1] = 99
outn(len(nums))        # 5
```

### Dict
```trip
let person = {"name": "Alice", "age": 30}
outn(person["name"])   # Alice
person["city"] = "NY"
outn(person.has("age"))   # true
```

---

## String Methods

```trip
let s = "  Hello, World!  "

s.len()              # character count
s.upper()            # "  HELLO, WORLD!  "
s.lower()            # "  hello, world!  "
s.trim()             # "Hello, World!"
s.split(", ")        # ["Hello", "World!  "]  (after trim)
s.contains("World")  # true
s.startsWith("  H")  # true
s.endsWith("  ")     # true
s.replace("World", "Trip")
s.slice(2, 7)        # "Hello"
s.find("World")      # index or -1
s.repeat(2)
s.chars()            # list of char values
s.format(...)        # printf-style formatting
s.padLeft(20)
s.padRight(20)
s.reverse()
s.isDigit()          # true if all chars are digits
s.isAlpha()          # true if all chars are alphabetic
s.isUpper()
s.isLower()
```

---

## List Methods

```trip
let lst = [3, 1, 4, 1, 5]

lst.append(9)          # add to end
lst.push(2)            # add to front
lst.pop()              # remove and return last
lst.insert(2, 99)      # insert 99 at index 2
lst.remove(1)          # remove first occurrence of 1
lst.contains(4)        # true
lst.reverse()
lst.sort()
lst.slice(1, 3)        # sublist
lst.join(", ")         # "3, 1, 4, 1, 5" (as string)
lst.len()
lst.clear()
lst.indexOf(4)         # index or -1
lst.flatten()          # flatten nested lists one level
lst.zip([10, 20, 30])  # [[3,10],[1,20],[4,30],...]
lst.enumerate()        # [[0,3],[1,1],[2,4],...]

lst.map(fn(x) => x * 2)
lst.filter(fn(x) => x > 2)
lst.reduce(fn(acc, x) => acc + x, 0)
lst.forEach(fn(x) => outn(x))
```

---

## Dict Methods

```trip
let d = {"a": 1, "b": 2}

d.get("a")         # 1
d.get("z", 0)      # 0 (default)
d.set("c", 3)
d.del("a")
d.keys()           # ["b", "c"]
d.values()         # [2, 3]
d.has("b")         # true
d.len()            # 2
d.clear()
```

---

## Standard Library

### Output & Input
```trip
out("no newline")
outn("with newline")
let line = input()      # reads one line from stdin
```

### Math
```trip
sqrt(16)          # 4.0
pow(2, 10)        # 1024.0
abs(-7)           # 7
min(3, 5)         # 3
max(3, 5)         # 5
floor(3.7)        # 3.0
ceil(3.2)         # 4.0
round(3.5)        # 4.0
random()          # float in [0, 1)
randomInt(1, 6)   # integer in [1, 6]
```

### Type conversion
```trip
int("42")         # 42
float("3.14")     # 3.14
str(100)          # "100"
char(65)          # 'A'
ord('A')          # 65
len("hello")      # 5
type(42)          # "number"
range(5)          # [0, 1, 2, 3, 4]
range(2, 8)       # [2, 3, 4, 5, 6, 7]
```

### File I/O
```trip
let content = readFile("data.txt")
writeFile("out.txt", "hello\n")
appendFile("log.txt", "entry\n")
fileExists("data.txt")    # true / false
deleteFile("temp.txt")
```

### System
```trip
let argv    = args()          # list of command-line argument strings
let home    = getEnv("HOME")  # environment variable or nil
let myPath  = scriptPath()    # absolute path of the running script
```

### JSON
```trip
let obj  = jsonParse('{"name": "Alice", "age": 30}')
let json = jsonStringify(obj)
outn(obj["name"])   # Alice
```

### Regex
```trip
regexMatch("hello 123", r"\d+")           # ["123"] or nil
regexMatchAll("a1 b2 c3", r"[a-z]\d")    # [["a1"], ["b2"], ["c3"]]
regexTest("hello", r"^h")                 # true
regexReplace("foo bar", r"\s+", "-")      # "foo-bar"
regexReplaceAll("aabbcc", r"(.)\1", "$1") # "abc"
regexSplit("one,two,,three", r",+")       # ["one", "two", "three"]
```

### Time & Date
```trip
let ts = time()                            # Unix timestamp (float, seconds)
let ms = timeMs()                          # Unix timestamp (float, milliseconds)
sleep(500)                                 # pause 500 ms

let now   = dateNow()                      # dict: {year, month, day, hour, minute, second, weekday}
let utc   = dateUTC()
let local = dateLocal(ts)
let fmt   = dateFormat(ts, "%Y-%m-%d")
let t2    = dateParse("2025-01-15", "%Y-%m-%d")
let t3    = dateMake(2025, 1, 15, 9, 0, 0)
```

---

## Networking

All networking is built in — no imports required.

### HTTP Client
```trip
let resp = httpGet("https://api.example.com/users")
let body = httpPost("https://api.example.com/login", {"user": "alice", "pass": "secret"})
httpPut(url, payload)
httpDelete(url)
httpPatch(url, payload)
```

### TCP
```trip
let server = tcpListen(8080)
let client = tcpAccept(server)
let data   = tcpRead(client)
tcpWrite(client, "HTTP/1.1 200 OK\r\n\r\nHello!")
tcpClose(client)
tcpClose(server)
```

### TLS
```trip
let server = tlsListen(443, "cert.pem", "key.pem")
let client = tlsAccept(server)
let data   = tlsRead(client)
tlsWrite(client, response)
tlsClose(client)

# TLS client
let conn = tlsConnect("example.com", 443)
tlsWrite(conn, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
let reply = tlsRead(conn)
tlsClose(conn)
```

### HTTP Server (raw)
```trip
let server = tcpListen(8080)

fn handleClient(sock)
    let raw = tcpRead(sock)
    let req = httpParse(raw)           # {method, path, headers, body, query}
    let resp = httpResponse(200, {"Content-Type": "text/plain"}, "Hello!")
    tcpWrite(sock, resp)
    tcpClose(sock)

while true
    let client = tcpAccept(server)
    spawn handleClient(client)
```

### Chunked HTTP & SSE
```trip
httpChunkedStart(sock, 200, {"Content-Type": "text/event-stream"})
httpChunkWrite(sock, "data chunk 1")
httpChunkWrite(sock, "data chunk 2")
httpChunkEnd(sock)

sseWrite(sock, "hello", "message", "1")
```

### WebSocket
```trip
let raw = tcpRead(client)
let req = httpParse(raw)
wsHandshake(client, req)

let msg = wsRead(client)
wsWrite(client, "pong")
wsClose(client)
```

---

## MySQL Database

A full database driver is built in — no external library imports required.

### Basic usage
```trip
let db = mysqlConnect("localhost", "user", "pass", "mydb")

# SELECT — returns list of dicts
let rows = mysqlQuery(db, "SELECT * FROM users WHERE active = 1")
for row in rows
    outn(row["name"])

# INSERT / UPDATE / DELETE — returns affected row count
let n = mysqlQuery(db, "INSERT INTO log (msg) VALUES ('hello')")
let id = mysqlLastInsertId(db)

mysqlClose(db)
```

### Escape & safety
```trip
let safe = mysqlEscape(db, userInput)
mysqlQuery(db, f"SELECT * FROM users WHERE name = '{safe}'")
```

### Prepared statements
```trip
let stmt = mysqlPrepare(db, "INSERT INTO items (name, qty) VALUES (?, ?)")
mysqlExecute(stmt, ["widget", 42])
mysqlStmtClose(stmt)
```

### Transactions
```trip
mysqlBegin(db)
try
    mysqlQuery(db, "UPDATE accounts SET balance = balance - 100 WHERE id = 1")
    mysqlQuery(db, "UPDATE accounts SET balance = balance + 100 WHERE id = 2")
    mysqlCommit(db)
catch err
    mysqlRollback(db)
    throw err
```

### Batch insert
```trip
let rows = [["Alice", 25], ["Bob", 30], ["Carol", 28]]
mysqlInsertBatch(db, "users", rows)
```

### Connection pool
```trip
let pool = mysqlPoolCreate("localhost", "user", "pass", "mydb", 10)
let conn = mysqlPoolGet(pool)
mysqlQuery(conn, "SELECT 1")
mysqlPoolRelease(pool, conn)
mysqlPoolClose(pool)
```

### Additional helpers
```trip
mysqlPing(db)             # bool — checks if connection is alive
mysqlAffectedRows(db)     # rows affected by last DML
mysqlError(db)            # last error string or nil
mysqlQueryMulti(db, sql)  # run multiple statements; returns list of result sets
```

---

## Module System

```trip
import "utils/math.tp"
import "shared/logger.tp"
```

- Paths are relative to the **importing file's directory**
- Absolute paths are also accepted
- Declarations from the imported file become available in the current global scope
- Importing the same file more than once is a no-op (tracked up to 64 files)
- The import is resolved and inlined at **compile time** (not runtime)

---

## Tooling

### REPL
```
$ trip
> let x = 10
> outn(x * 2)
20
```

### Script runner
```
$ trip hello.tp
```
Only `.tp` files are accepted.

### Test runner
```
$ trip test             # runs all *_test.tp / *.test.tp in current directory
$ trip test tests/      # runs all test files under tests/
$ trip test mytest.tp   # runs a single test file
```
Reports `PASS` or `FAIL` per file.

---

## VM Architecture

| Component | Details |
|-----------|---------|
| **Parser** | Single-pass Pratt (precedence-climbing) parser; no separate AST |
| **Compiler** | Emits bytecode directly during parsing |
| **VM** | Register-less stack VM with explicit call frames |
| **Closures** | Upvalue-based: open upvalues point into live stack frames; closed upvalues are heap-copied on scope exit |
| **GC** | Mark-and-sweep over a singly-linked object list |
| **String interning** | All strings are stored in a global hash table; equality is pointer comparison |
| **Globals** | Stored in a hash table keyed by interned string |
| **Scheduler** | Cooperative fiber scheduler with a FIFO ready queue and a blocked-fiber I/O reactor |
| **HTTP I/O** | Powered by `libcurl` with `curl_multi` for non-blocking requests |
| **TLS** | Powered by OpenSSL |
| **Regex** | POSIX extended regular expressions (`<regex.h>`) |

---

## License

See `LICENSE.txt` included with the source distribution.