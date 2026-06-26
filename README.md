# Trip Language

A simple, expressive scripting language that runs on Node.js.

## Download

Grab the latest standalone executable from [Releases](../../releases):
- `trip.exe` — Windows
- `trip-linux` — Linux

No Node.js required!

## Usage

```
trip yourfile.tp     # run a file
trip                 # start REPL
```

## Build from Source

Requires Node.js 18+.

```bash
git clone https://github.com/Lazycoder229/trip
cd trip
node trip.js yourfile.tp
```

To build standalone executables:
```bash
npm install -g pkg
pkg trip.js --targets node18-win-x64 --output trip.exe
```
## Download
- [⬇ trip.exe (Windows)](https://github.com/Lazycoder229/trip/releases/download/v1.0.0/trip.exe)
- [⬇ trip-linux (Linux)](https://github.com/Lazycoder229/trip/releases/download/v1.0.0/trip-linux)

Upgrade Path — Suggested Sequence
🟢 Phase 1 — Basic QOL (Madali, mabilis)
1. const keyword        — immutable variables
2. ++ / --              — increment/decrement
3. != already exists    — okay na
4. String interpolation — f"Hello {name}"
5. Multiline expressions — hindi kailangang isang linya lang
6. ** operator          — exponentiation (2 ** 8 = 256)
🟡 Phase 2 — Language Completeness (Medium)
7.  Default params      — fn greet(name = "World"):
8.  Spread/rest         — fn sum(*args):
9.  Multiple return     — return x, y
10. Ternary expression  — let x = a if condition else b
11. Null coalescing     — x ?? "default"
12. In operator         — if x in array:
13. Type checking       — x is int
14. String methods++    — format, padStart, padEnd, repeat
🟠 Phase 3 — Advanced Features (Harder)
15. List comprehension  — [x * 2 for x in range(10)]
16. Dict comprehension  — {k: v for k, v in pairs}
17. Generators          — yield keyword
18. Decorators          — @decorator syntax
19. Match/switch        — match x: case 1:
20. Async/await         — async fn fetch():
🔴 Phase 4 — Ecosystem (Malaki, optional)
21. Standard library expansion
    - math module
    - string module  
    - array module
    - datetime module
22. File I/O            — file.read(), file.write()
23. Error types         — class MyError(Error):
24. Package system      — import from "stdlib/math"
25. Type hints          — fn add(a: int, b: int) -> int: