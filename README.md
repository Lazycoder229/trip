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
