// compiler_modules.c — the `import` statement and everything it needs to
// resolve relative paths and avoid importing the same file twice. All of
// the tracking state here is private to this file; compile() only ever
// touches it through modulesBeginScript()/resetImportTracking().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compiler_internal.h"

#define MAX_IMPORTS 64
static char* importedPaths[MAX_IMPORTS];
static int importedPathCount = 0;
static char* currentImportBaseDir = NULL;

static char* dupString(const char* s) {
    size_t len = strlen(s);
    char* copy = malloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

static char* dirNameOf(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    size_t len = lastSlash ? (size_t)(lastSlash - path + 1) : 0;
    char* dir = malloc(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char* resolveImportPath(const char* givenPath) {
    if (givenPath[0] == '/') {
        return dupString(givenPath);
    }
    const char* base = currentImportBaseDir ? currentImportBaseDir : "";
    size_t baseLen = strlen(base);
    size_t givenLen = strlen(givenPath);
    char* result = malloc(baseLen + givenLen + 1);
    memcpy(result, base, baseLen);
    memcpy(result + baseLen, givenPath, givenLen + 1);
    return result;
}

void resetImportTracking(void) {
    for (int i = 0; i < importedPathCount; i++) {
        free(importedPaths[i]);
        importedPaths[i] = NULL;
    }
    importedPathCount = 0;
}

// Called once at the start of compile(). Resets import tracking for a
// fresh script, then seeds it with the running script's own path (so a
// script can never accidentally re-import itself) and derives the base
// directory that relative `import "..."` paths resolve against.
void modulesBeginScript(const char* scriptPath) {
    resetImportTracking();
    free(currentImportBaseDir);
    currentImportBaseDir = scriptPath ? dirNameOf(scriptPath) : dupString("");
    if (scriptPath && importedPathCount < MAX_IMPORTS) {
        importedPaths[importedPathCount++] = dupString(scriptPath);
    }
}

void importStatement(void) {
    if (parser.current.type != TOKEN_STRING) {
        errorAt(&parser.current, "Expect a file path string after 'import'.");
        return;
    }
    advance();

    ObjString* rawPath = copyStringWithEscapes(parser.previous.start + 1, parser.previous.length - 2);
    char* fullPath = resolveImportPath(rawPath->chars);

    for (int i = 0; i < importedPathCount; i++) {
        if (strcmp(importedPaths[i], fullPath) == 0) {
            free(fullPath);
            return;
        }
    }
    if (importedPathCount >= MAX_IMPORTS) {
        errorAt(&parser.previous, "Too many imported files.");
        free(fullPath);
        return;
    }
    importedPaths[importedPathCount++] = fullPath;

    FILE* f = fopen(fullPath, "rb");
    if (!f) {
        errorAt(&parser.previous, "Could not open imported file.");
        return;
    }
    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        errorAt(&parser.previous, "Could not determine size of imported file.");
        return;
    }
    rewind(f);
    char* source = malloc((size_t)size + 1);
    if (!source) {
        fclose(f);
        errorAt(&parser.previous, "Out of memory reading imported file.");
        return;
    }
    size_t bytesRead = fread(source, sizeof(char), (size_t)size, f);
    source[bytesRead] = '\0';
    fclose(f);

    char* newBaseDir = dirNameOf(fullPath);
    char* savedBaseDir = currentImportBaseDir;
    currentImportBaseDir = newBaseDir;

    Scanner savedScanner = scanner;
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;

    initScanner(source);
    advance();
    skipNewlinesAndDedents();
    while (parser.current.type != TOKEN_EOF) {
        statement();
        // Use skipNewlinesAndDedents here too — imported files may also
        // contain multiline expressions that leave stray DEDENTs.
        skipNewlinesAndDedents();
    }

    scanner = savedScanner;
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    currentImportBaseDir = savedBaseDir;
    free(newBaseDir);
    free(source);
}
