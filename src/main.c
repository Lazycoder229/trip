#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "vm.h"

// dirent.h + sys/stat.h are used by `trip test <dir>` to walk a directory
// looking for "*_test.tp" files. Both are available on POSIX and on
// MinGW-w64 (the toolchain the project's Makefile already targets for
// Windows), so this doesn't add a new platform dependency beyond what
// bundle/installer already assumes.

// The Interactive Prompt
static void repl() {
    char line[1024];
    for (;;) {
        printf("> "); // The classic language prompt

        // Read a line of input
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        // Send it to the VM to be compiled and executed!
        interpret(line);
    }
}

// Reads an entire file into a freshly malloc'd, null-terminated buffer.
// The caller owns the returned pointer and must free() it.
static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }

    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

// Runs a whole source file as one program. Multi-line constructs like
// if/else blocks only make sense this way — a line-at-a-time REPL has
// no notion of "this block isn't finished yet".
static void runFile(const char* path) {
    char* source = readFile(path);
    InterpretResult result = interpret(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) exit(65);
    if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

// Only ".tp" source files are allowed through runFile(). Without this,
// `./triplang /etc/passwd` or any other arbitrary file on disk would get
// read and fed straight into the compiler — harmless here today since a
// compile error just aborts, but it's the wrong habit to build into a
// language's entry point as the VM grows closer to doing real file I/O.
static bool hasTpExtension(const char* path) {
    size_t len = strlen(path);
    const char* ext = ".tp";
    size_t extLen = strlen(ext);
    return len > extLen && strcmp(path + (len - extLen), ext) == 0;
}

static bool hasSuffix(const char* name, const char* suffix) {
    size_t len = strlen(name);
    size_t suffixLen = strlen(suffix);
    return len > suffixLen && strcmp(name + (len - suffixLen), suffix) == 0;
}

// A file counts as a test if it's named either "<name>_test.tp" (Go-style,
// what the original convention here used) or "<name>.test.tp" (Jest-style —
// what people coming from a JS background naturally reach for). Supporting
// both means an end user doesn't need to know which one Trip "prefers".
static bool isTestFile(const char* name) {
    return hasSuffix(name, "_test.tp") || hasSuffix(name, ".test.tp");
}

// ── `trip test [path]` ────────────────────────────────────────────────────
//
// Runs every "*_test.tp" or "*.test.tp" file under `path` (or just `path`
// itself if it's a single file). With no `path` argument, defaults to the
// current directory, so an end user can just `cd` into their project and
// run `trip test`. Reports PASS/FAIL per file based on the same
// InterpretResult that runFile() already uses for exit codes — a test
// file that throws an uncaught error is a FAIL, same as any other
// uncaught runtime error.
//
// No shell/bash required, unlike a wrapper script — this matters because
// the installer only ships trip.exe; an end user on plain Windows has no
// guarantee of having bash available the way a dev in the MSYS2 shell
// does.

// Runs one "*_test.tp" file in a completely fresh VM so that one test
// file's globals/heap state can never leak into the next — the same
// isolation goal tests/test_support.c's resetTestVM() serves for the C
// unit tests, just at the whole-VM granularity instead of per-TEST().
static bool runOneTest(const char* path) {
    printf("  %s ... ", path);
    fflush(stdout);

    freeVM();
    initVM();
    vmSetArgs(0, NULL, path);

    char* source = readFile(path);
    InterpretResult result = interpret(source);
    free(source);

    if (result == INTERPRET_OK) {
        printf("PASS\n");
        return true;
    }
    printf("FAIL (%s)\n", result == INTERPRET_COMPILE_ERROR
                               ? "compile error"
                               : "runtime error — an assert*() likely threw");
    return false;
}

// Recursively walks `dirPath` collecting and running every "*_test.tp"
// file it finds. Returns the number of failures; increments *outTotal
// once per test file it runs, so the caller can print "N passed, M of T
// failed" without a second traversal.
static int runTestsInDir(const char* dirPath, int* outTotal) {
    DIR* dir = opendir(dirPath);
    if (dir == NULL) {
        fprintf(stderr, "Error: could not open directory \"%s\".\n", dirPath);
        exit(74);
    }

    int failures = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, entry->d_name);

        struct stat st;
        if (stat(fullPath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            failures += runTestsInDir(fullPath, outTotal);
        } else if (isTestFile(entry->d_name)) {
            (*outTotal)++;
            if (!runOneTest(fullPath)) failures++;
        }
    }

    closedir(dir);
    return failures;
}

// Entry point for the `test` subcommand. Exits directly (rather than
// returning) since it manages its own sequence of VM resets and the
// normal single-VM run/REPL path below doesn't apply once we're here.
static void runTestCommand(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Error: \"%s\" does not exist.\n", path);
        exit(74);
    }

    int total = 0;
    int failures;

    if (S_ISDIR(st.st_mode)) {
        failures = runTestsInDir(path, &total);
    } else {
        total = 1;
        failures = runOneTest(path) ? 0 : 1;
    }

    printf("\n%d passed, %d failed, %d total\n", total - failures, failures, total);
    freeVM();
    exit(failures == 0 ? 0 : 1);
}

int main(int argc, const char* argv[]) {
    initVM();

    // `trip test [file_test.tp | file.test.tp | directory]`
    // With no path given, defaults to "." — the end-user workflow is
    // "sit in your project folder and type `trip test`", the same as
    // `go test ./...` or a bare `phpunit`/`jest` invocation, not
    // "look up the exact folder name and pass it every time".
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        const char* testPath = (argc >= 3) ? argv[2] : ".";
        runTestCommand(testPath); // exits internally, never returns
    }

    // `trip run <file.tp> [args...]` — explicit form.
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: trip run <file.tp> [args...]\n");
            freeVM();
            exit(64);
        }
        if (!hasTpExtension(argv[2])) {
            fprintf(stderr, "Error: Trip only runs \".tp\" source files (got \"%s\").\n", argv[2]);
            freeVM();
            exit(64);
        }
        vmSetArgs(argc - 3, argc > 3 ? &argv[3] : NULL, argv[2]);
        runFile(argv[2]);
        freeVM();
        return 0;
    }

    if (argc >= 2) {
        // Backward-compatible bare form: `trip file.tp [args...]`, kept
        // working so existing scripts/docs/installer shortcuts that
        // predate the `run`/`test` subcommands don't break.
        if (!hasTpExtension(argv[1])) {
            fprintf(stderr, "Error: Trip only runs \".tp\" source files (got \"%s\").\n", argv[1]);
            freeVM();
            exit(64);
        }
        vmSetArgs(argc - 2, argc > 2 ? &argv[2] : NULL, argv[1]);
        runFile(argv[1]);
    } else {
        printf("Trip Language REPL\n");
        printf("Type any Trip expression or statement and press Enter.\n");
        printf("Press Ctrl+C or Ctrl+D to exit.\n\n");

        repl();
    }

    freeVM();
    return 0;
}