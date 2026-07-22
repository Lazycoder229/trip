# ─────────────────────────────────────────────────────────────────────────────
#  Trip Makefile — Windows (MSYS2 MinGW64)
# ─────────────────────────────────────────────────────────────────────────────

CC       = gcc
TARGET   = trip.exe

SRC_DIR  = src
BUILD    = build
DIST     = dist

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/chunk.c \
       $(SRC_DIR)/memory.c \
       $(SRC_DIR)/object.c \
       $(SRC_DIR)/scanner.c \
       $(SRC_DIR)/table.c \
       $(SRC_DIR)/value.c \
       $(SRC_DIR)/cjson.c \
	    $(SRC_DIR)/compiler/compiler_concurrency.c \
	    $(SRC_DIR)/compiler/compiler_core.c \
		  $(SRC_DIR)/compiler/compiler_exceptions.c \
	 $(SRC_DIR)/compiler/compiler_expressions.c \
	   $(SRC_DIR)/compiler/compiler_functions.c \
	    $(SRC_DIR)/compiler/compiler_modules.c \
		 $(SRC_DIR)/compiler/compiler_scope.c \
		  $(SRC_DIR)/compiler/compiler_statements.c \
		   $(SRC_DIR)/compiler/compiler.c \
       $(SRC_DIR)/tvm/vm_helpers.c \
       $(SRC_DIR)/tvm/vm_core.c \
       $(SRC_DIR)/tvm/vm_exec.c \
       $(SRC_DIR)/tvm/scheduler.c \
       $(SRC_DIR)/tvm/method_str.c \
       $(SRC_DIR)/tvm/method_list.c \
       $(SRC_DIR)/tvm/method_dict.c \
       $(SRC_DIR)/tvm/builtin_core.c \
       $(SRC_DIR)/tvm/builtin_io.c \
       $(SRC_DIR)/tvm/builtin_json.c \
       $(SRC_DIR)/tvm/builtin_time.c \
       $(SRC_DIR)/tvm/builtin_regex.c \
       $(SRC_DIR)/tvm/net_http.c \
       $(SRC_DIR)/tvm/net_tcp.c \
       $(SRC_DIR)/tvm/net_tls.c \
       $(SRC_DIR)/tvm/net_server.c \
       $(SRC_DIR)/tvm/net_ws.c \

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

# ── Release Flags ─────────────────────────────────────────────────────────────
CFLAGS  = -O2 -Wall -Wextra -std=c11 \
          -Wno-unused-parameter \
          -D_POSIX_C_SOURCE=200809L \
          -D_XOPEN_SOURCE=700 \
          -DPCRE2_CODE_UNIT_WIDTH=8

LDFLAGS = -lm -lcurl -lssl -lcrypto -lws2_32 -lpcre2-8

# IMPORTANT: this must match whichever MSYS2 environment you actually
# installed curl/openssl/pcre2 into (check with `which gcc` — if it prints
# /mingw64/bin/gcc.exe you're in MINGW64, and packages installed via
# `pacman -S mingw-w64-x86_64-*` land in /mingw64/bin, NOT /ucrt64/bin).
# Pointing this at the wrong environment doesn't error — `make bundle`
# just silently skips any DLL it can't find at this path, producing an
# incomplete dist/ folder that only fails once an end user runs it.
MINGW = /mingw64/bin
NSIS  = makensis

# Dr. Memory install root — adjust if you installed it somewhere else.
# Using bin64 since our release build is 64-bit (MINGW64); if you ever
# ship a 32-bit build, point this at bin/ instead — mismatching the two
# silently produces garbage/incomplete results, not an error.
DRMEMORY = /c/Program Files (x86)/Dr. Memory/bin64/drmemory.exe

# Suppresses known false positives (OpenSSL asm stack probing, CRT
# stat()/readdir() via FindFirstFile/FindNextFile) so real bugs in trip's
# own code aren't buried in noise. See the file itself for details on
# what's suppressed and why. Keep this checked into the repo.
DRMEMORY_SUPPRESS = drmemory-suppressions.txt

# ── Unit tests ────────────────────────────────────────────────────────────────
# Lightweight, dependency-free (no cmocka/Unity) — see tests/test.h.
# Each test binary links ONLY the .c files it actually needs.
#
# table.c/memory.c/compiler.c need a real (but minimal) VM global to link
# against — see tests/test_support.c, which provides `VM vm;` plus faithful
# copies of the handful of one-line vm_core.c hooks these modules call
# (vmTrackObject/vmFindInternedString/vmInternString/tripCloseSocketHandle/
# vmGetScriptPath), without pulling in curl/openssl/the fiber scheduler —
# none of which a hash-table/allocator/compiler test needs.
TEST_DIR   = tests
TEST_BUILD = $(BUILD)/tests
TEST_CFLAGS = -std=c11 -Wall -Wextra -Werror

TEST_BINS = $(TEST_BUILD)/test_scanner \
            $(TEST_BUILD)/test_table \
            $(TEST_BUILD)/test_memory \
            $(TEST_BUILD)/test_compiler

$(TEST_BUILD)/test_scanner: $(TEST_DIR)/test_scanner.c $(SRC_DIR)/scanner.c $(TEST_DIR)/test.h
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_scanner.c $(SRC_DIR)/scanner.c

$(TEST_BUILD)/test_table: $(TEST_DIR)/test_table.c $(TEST_DIR)/test_support.c \
                          $(SRC_DIR)/table.c $(SRC_DIR)/object.c $(SRC_DIR)/memory.c \
                          $(SRC_DIR)/chunk.c $(SRC_DIR)/value.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_table.c $(TEST_DIR)/test_support.c \
		$(SRC_DIR)/table.c $(SRC_DIR)/object.c $(SRC_DIR)/memory.c \
		$(SRC_DIR)/chunk.c $(SRC_DIR)/value.c

$(TEST_BUILD)/test_memory: $(TEST_DIR)/test_memory.c $(TEST_DIR)/test_support.c \
                           $(SRC_DIR)/memory.c $(SRC_DIR)/object.c $(SRC_DIR)/table.c \
                           $(SRC_DIR)/chunk.c $(SRC_DIR)/value.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_memory.c $(TEST_DIR)/test_support.c \
		$(SRC_DIR)/memory.c $(SRC_DIR)/object.c $(SRC_DIR)/table.c \
		$(SRC_DIR)/chunk.c $(SRC_DIR)/value.c

$(TEST_BUILD)/test_compiler: $(TEST_DIR)/test_compiler.c $(TEST_DIR)/test_support.c \
                             $(SRC_DIR)/compiler/compiler_core.c \
                             $(SRC_DIR)/compiler/compiler_concurrency.c \
                             $(SRC_DIR)/compiler/compiler_exceptions.c \
                             $(SRC_DIR)/compiler/compiler_expressions.c \
                             $(SRC_DIR)/compiler/compiler_functions.c \
                             $(SRC_DIR)/compiler/compiler_modules.c \
                             $(SRC_DIR)/compiler/compiler_scope.c \
                             $(SRC_DIR)/compiler/compiler_statements.c \
                             $(SRC_DIR)/scanner.c $(SRC_DIR)/object.c \
                             $(SRC_DIR)/memory.c $(SRC_DIR)/table.c $(SRC_DIR)/chunk.c \
                             $(SRC_DIR)/value.c
	@mkdir -p $(TEST_BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_compiler.c $(TEST_DIR)/test_support.c \
        $(SRC_DIR)/compiler/compiler_core.c \
        $(SRC_DIR)/compiler/compiler_concurrency.c \
        $(SRC_DIR)/compiler/compiler_exceptions.c \
        $(SRC_DIR)/compiler/compiler_expressions.c \
        $(SRC_DIR)/compiler/compiler_functions.c \
        $(SRC_DIR)/compiler/compiler_modules.c \
        $(SRC_DIR)/compiler/compiler_scope.c \
        $(SRC_DIR)/compiler/compiler_statements.c \
        $(SRC_DIR)/scanner.c $(SRC_DIR)/object.c \
        $(SRC_DIR)/memory.c $(SRC_DIR)/table.c $(SRC_DIR)/chunk.c $(SRC_DIR)/value.c

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "── $$t ──"; \
		./$$t || status=1; \
		echo ""; \
	done; \
	exit $$status

# ── Cross-compiler test matrix ────────────────────────────────────────────────
# `test:` alone always uses whatever $(CC) currently is (gcc, by default) —
# it does NOT exercise Clang unless called as `make test CC=clang`. GCC and
# Clang diverge on some warnings/UB diagnostics, so running the same suite
# under both is worth doing in CI, not just ad hoc.
#
# Each pass does a clean test-rebuild so stale .o's from the other compiler
# are never linked into the wrong run.
test-all-compilers:
	@echo "════ Testing with GCC ════"
	@$(MAKE) test-clean
	@$(MAKE) test CC=gcc
	@echo ""
	@echo "════ Testing with Clang ════"
	@$(MAKE) test-clean
	@$(MAKE) test CC=clang
	@$(MAKE) test-clean

# ── .tp-level tests (via the native `trip test` subcommand) ──────────────
# Unlike test:, this doesn't build a separate test binary — `trip test`
# lives inside $(TARGET) itself, so this just builds the real trip.exe
# and points it at tests/test_modules/.
test-tp: $(TARGET)
	@./$(TARGET) test tests/test_modules

# Runs both the C-level unit tests AND the .tp-level tests in one go.
test-all: test test-tp
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all debug sanitize bundle installer release clean help test test-tp test-all test-all-compilers drmemory drmemory-file

# ── Build ─────────────────────────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  ✔ Built: $(TARGET)"
	@echo ""

# ── Debug Build ───────────────────────────────────────────────────────────────
debug: CC = clang
debug: CFLAGS = -g3 -O0 -Wall -Wextra -Werror -std=c11 \
                -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L \
                -D_XOPEN_SOURCE=700 \
                -DPCRE2_CODE_UNIT_WIDTH=8
debug: clean $(TARGET)

# ── Sanitizer Build ───────────────────────────────────────────────────────────
# UBSan only, in TRAP MODE (-fsanitize-undefined-trap-on-error).
#
# We'd prefer full ASan+UBSan here, but it can't link in this environment:
# MSYS2's MINGW64 Clang package ships WITHOUT the compiler-rt sanitizer
# runtime (libclang_rt.asan_dynamic*, ubsan runtime thunks, etc.) — this is
# an upstream packaging decision, not something fixable via flags. Those
# runtime libs only exist in the separate CLANG64 MSYS2 environment (a
# different Clang+LLD+UCRT toolchain), which would mean a second, parallel
# dependency tree (its own curl/openssl/pcre2 builds) just for this target.
# Not worth it for a single sanitizer pass.
#
# Trap mode sidesteps this: instead of calling into a runtime handler
# (__ubsan_handle_*) to print a diagnostic, each check compiles to a plain
# __builtin_trap() — no runtime library needed at all, so it links cleanly
# with stock MINGW64 Clang. Trade-off: on violation you get a crash (illegal
# instruction) instead of a descriptive message. Good enough as a pass/fail
# CI gate; for the exact file/line, rerun the same binary under gdb and get
# a backtrace at the trap.
#
# This still only catches undefined behavior (signed overflow, null deref,
# misaligned access, bad shifts, etc.) — NOT heap overflows or
# use-after-free, which is what ASan would add. For that class of bug in
# memory.c/object.c/table.c, either run the core logic (non-networking
# files) through a real ASan build on Linux/WSL2, or run the already-built
# Windows trip.exe through Dr. Memory (https://drmemory.org/) — see the
# drmemory: target below, which does exactly that.
#
# Requires Clang from the SAME MSYS2 environment as gcc (verify with
# `which gcc clang` — both should print /mingw64/bin/...). See `make help`.
sanitize: CC = clang
sanitize: CFLAGS = -g3 -O0 -Wall -Wextra -std=c11 \
                    -fsanitize=undefined -fsanitize-undefined-trap-on-error \
                    -fno-omit-frame-pointer \
                    -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L \
                    -D_XOPEN_SOURCE=700 \
                    -DPCRE2_CODE_UNIT_WIDTH=8
sanitize: LDFLAGS += -fsanitize=undefined -fsanitize-undefined-trap-on-error
sanitize: clean $(TARGET)
	@echo ""
	@echo "  ✔ Built with UBSan (trap mode): $(TARGET)"
	@echo "  Run it normally — on a violation it crashes (illegal instruction)"
	@echo "  instead of printing a message. Rerun under gdb for a backtrace:"
	@echo "      gdb --args ./$(TARGET) test tests/test_modules"
	@echo "      (gdb) run"
	@echo "      (gdb) bt          # once it traps"
	@echo ""

# ── Compile object files ──────────────────────────────────────────────────────
$(BUILD)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Bundle DLLs ───────────────────────────────────────────────────────────────
# Recursively resolves transitive DLL dependencies until a fixed point is
# reached (no new DLLs found in a full pass). This also ALWAYS copies fresh
# from $(MINGW), overwriting whatever is already in $(DIST) — a previous
# version of this target only copied a DLL if it wasn't already present in
# dist/, which meant that once a DLL landed in dist/ once, every future
# `make bundle` silently kept that stale copy forever, even after
# `pacman -Syu` updated the real one in MSYS2. That let dist/ end up with,
# e.g., a fresh libcurl-4.dll paired with a MONTHS-OLD
# libngtcp2_crypto_ossl-0.dll — an ABI mismatch between "fresh" and "stale"
# deps in the same bundle, which manifests on the end user's machine as a
# confusing "entry point not found" error instead of a clean "DLL not found".
#
# A missing DLL isn't always obvious either — if the end user happens to
# have MSYS2 installed and on PATH, Windows silently falls back to loading
# a DIFFERENT (and possibly incompatible/newer) copy from there instead of
# failing cleanly. Always-overwrite avoids both failure modes: dist/ is
# guaranteed to be a true, self-consistent snapshot of the CURRENT MSYS2
# packages on every build.
#
# IMPORTANT: $(TARGET) (e.g. trip.exe) is built directly in the project
# ROOT by the rule above, with no DLLs sitting next to it. Once it's
# copied into $(DIST), that root-level copy becomes a dangling landmine:
# cmd.exe checks the CURRENT DIRECTORY before PATH when resolving a bare
# command name, so anyone who cd's into the project root and types
# `trip ...` (or `triplang ...`) unknowingly runs THIS DLL-less loose
# binary instead of the properly bundled/installed one — producing
# exactly the same "entry point not found" symptom as a stale DLL, even
# when the real installed copy is 100% correct. We delete it here right
# after copying so only ONE canonical build artifact survives per build
# (the one in dist/, with its DLLs alongside it).
bundle: $(TARGET)
	@mkdir -p $(DIST)
	@cp -f $(TARGET) $(DIST)/
	@rm -f $(TARGET)
	@echo "Collecting DLLs (recursively, until no new ones are found)..."
	@changed=1; \
	while [ "$$changed" = "1" ]; do \
		changed=0; \
		for f in $(DIST)/$(TARGET) $(DIST)/*.dll; do \
			[ -f "$$f" ] || continue; \
			for dll in $$(objdump -p "$$f" 2>/dev/null | grep "DLL Name" | awk '{print $$3}'); do \
				if [ -f "$(MINGW)/$$dll" ]; then \
					if [ ! -f "$(DIST)/$$dll" ] || ! cmp -s "$(MINGW)/$$dll" "$(DIST)/$$dll"; then \
						cp -f "$(MINGW)/$$dll" $(DIST)/ && echo "  ✔ $$dll" && changed=1; \
					fi; \
				fi; \
			done; \
		done; \
	done
	@echo ""
	@echo "  ✔ Bundle ready: $(DIST)/"
	@ls $(DIST)/
	@echo ""

# ── Dr. Memory pass ──────────────────────────────────────────────────────────
# Runs the bundled release binary (with its DLLs, from dist/) through
# Dr. Memory for heap overflow / use-after-free / uninitialized-read
# detection — the class of bug UBSan trap mode (sanitize:) does NOT catch.
#
# Depends on `bundle`, not `all`: Dr. Memory needs to resolve the same DLLs
# an end user's machine would load (curl/openssl/pcre2/etc.), so it must run
# against dist/$(TARGET), never the loose root-level exe (which `bundle`
# deletes anyway — see the comment on that target).
#
# -batch:  non-interactive, no GUI popup at the end — required for this to
#          be runnable unattended/in CI, same reasoning as -batch elsewhere.
# -ignore_kernel: skips Dr. Memory's attempt to auto-generate syscall info
#          for this exact Windows build (it doesn't ship pre-built syscall
#          tables for every build number). Without this, an unrecognized
#          build pops a "System call information is missing" dialog and
#          restarts the run. Trade-off: slightly less precision tracking
#          uninitialized memory across a few specific syscalls — acceptable
#          here since those are exactly the FindFirstFile/FindNextFile
#          syscalls we're already suppressing as known false positives.
# -logdir: fixed, predictable report location instead of the default
#          timestamped folder under %USERPROFILE%\dr_memory\, so CI/scripts
#          always know where to look.
#
# Runs the full .tp-level suite by default. For a faster, targeted pass
# while chasing a specific bug (e.g. in the GC or socket code), use
# `make drmemory-file FILE=tests/test_modules/gc_stress.tp` instead.
drmemory: bundle
	@test -f "$(DRMEMORY)" || (echo "❌ Dr. Memory not found at $(DRMEMORY)" && exit 1)
	@test -f "$(DRMEMORY_SUPPRESS)" || (echo "❌ Suppression file not found at $(DRMEMORY_SUPPRESS)" && exit 1)
	@echo "════ Running Dr. Memory against dist/$(TARGET) (full test_modules) ════"
	@rm -rf drmemory_logs
	@mkdir -p drmemory_logs
	"$(DRMEMORY)" -batch -ignore_kernel -logdir drmemory_logs -suppress "$(DRMEMORY_SUPPRESS)" -- ./$(DIST)/$(TARGET) test tests/test_modules
	@echo ""
	@echo "  ✔ Dr. Memory pass complete — see drmemory_logs/ for results.txt"
	@echo ""

# Targeted variant: point FILE at a single .tp script instead of the whole
# tests/test_modules directory. Much faster turnaround when you're chasing
# one suspected leak/overflow (e.g. in GC or net_tcp.c) instead of re-running
# the entire suite through the instrumented binary every time.
#
#   make drmemory-file FILE=tests/test_modules/gc_stress.tp
drmemory-file: bundle
	@test -f "$(DRMEMORY)" || (echo "❌ Dr. Memory not found at $(DRMEMORY)" && exit 1)
	@test -f "$(DRMEMORY_SUPPRESS)" || (echo "❌ Suppression file not found at $(DRMEMORY_SUPPRESS)" && exit 1)
	@test -n "$(FILE)" || (echo "❌ Usage: make drmemory-file FILE=path/to/script.tp" && exit 1)
	@test -f "$(FILE)" || (echo "❌ File not found: $(FILE)" && exit 1)
	@echo "════ Running Dr. Memory against dist/$(TARGET) on $(FILE) ════"
	@rm -rf drmemory_logs
	@mkdir -p drmemory_logs
	"$(DRMEMORY)" -batch -ignore_kernel -logdir drmemory_logs -suppress "$(DRMEMORY_SUPPRESS)" -- ./$(DIST)/$(TARGET) run $(FILE)
	@echo ""
	@echo "  ✔ Dr. Memory pass complete — see drmemory_logs/ for results.txt"
	@echo ""

# ── Installer ────────────────────────────────────────────────────────────────
installer: bundle
	@test -f dist/trip.exe || (echo "❌ dist/trip.exe missing — run 'make bundle' first" && exit 1)
	@if [ ! -f LICENSE.txt ]; then \
		echo "LICENSE.txt not found. Add it to the project root before building installer."; \
		exit 1; \
	fi
	@echo "Building installer..."
	$(NSIS) installer.nsi
	@echo ""
	@echo "  ✔ Trip-Installer.exe READY"
	@echo ""

# ── Release ───────────────────────────────────────────────────────────────────
# Full gated pipeline: nothing reaches dist/ unless every stage below passes.
#
# Each stage is a separate `$(MAKE)` sub-invocation (not a plain prerequisite),
# with a `clean` between stages that touch $(TARGET). This is deliberate: if
# `sanitize` and `installer` were just listed as prerequisites in ONE `make`
# run, Make would see trip.exe already sitting there after the sanitize stage
# (freshly built, just with Clang+UBSan-trap-mode instrumentation) and, since
# nothing touched $(OBJS) afterward, consider it up-to-date — silently
# shipping the UBSan-instrumented debug binary as the "release" build instead
# of rebuilding it with the real GCC release flags. Separate sub-`make`
# invocations avoid that: each one starts clean, with no leftover variable
# context or build artifacts from the previous stage.
#
# Stage 2 and 3 also run tests/test_modules directly against the binary each stage just
# built (`./$(TARGET) test tests/test_modules`, not `$(MAKE) test-tp`) — calling the
# binary directly, rather than through Make, guarantees we're exercising the
# EXACT file that stage just produced, not triggering some other rebuild.
# Stage 3's binary is left untouched afterward and is the same file `bundle`
# picks up in stage 4, so the binary that ships is the literal one just
# tested — not a nominally-identical rebuild of it.
#
# Dr. Memory is deliberately NOT a stage here: it's a real-instruction-level
# instrumented run, meaningfully slower than the UBSan trap-mode pass in
# stage 2, so gating every release on it would slow down the pipeline a lot
# for marginal extra coverage on a build that's already passing ASan-class
# checks in spirit via UBSan. Run `make drmemory` separately (manually, or
# as its own nightly/weekly CI job) instead of folding it in here.
release:
	@echo ""
	@echo "════ [1/4] Unit tests — GCC + Clang ════"
	@$(MAKE) test-all-compilers
	@echo ""
	@echo "════ [2/4] Sanitizer pass — Clang + UBSan (trap mode), running tests/test_modules through it ════"
	@$(MAKE) sanitize
	@./$(TARGET) test tests/test_modules
	@$(MAKE) clean
	@echo ""
	@echo "════ [3/4] Building release binary (GCC) and running tests/test_modules against it ════"
	@$(MAKE) all
	@./$(TARGET) test tests/test_modules
	@echo ""
	@echo "════ [4/4] Bundling + installer (same binary just tested above) ════"
	@$(MAKE) installer
	@echo ""
	@echo "  ╔══════════════════════════════════════════╗"
	@echo "  ║   Trip Installer Ready 🚀            ║"
	@echo "  ║   Just ship the .exe to users            ║"
	@echo "  ╚══════════════════════════════════════════╝"
	@echo ""

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD) $(DIST)
	rm -f $(TARGET) Trip-Installer.exe
	@echo "✔ Cleaned."

test-clean:
	rm -rf $(TEST_BUILD)

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  make                    → build optimized trip.exe (GCC)"
	@echo "  make debug              → build with -Werror and debug symbols (Clang)"
	@echo "  make sanitize           → Clang build with UBSan (trap mode) — catches"
	@echo "                            undefined behavior in memory.c/object.c/net_*.c."
	@echo "                            NOT full ASan (MINGW64 Clang can't link it — see"
	@echo "                            comment on the sanitize: target). For heap"
	@echo "                            overflow/use-after-free detection, use"
	@echo "                            'make drmemory' instead."
	@echo "  make bundle             → collect DLLs (always fresh from MSYS2, no stale copies)"
	@echo "                            and removes the loose root-level exe so it can't"
	@echo "                            shadow the installed copy when running from the"
	@echo "                            project root"
	@echo "  make drmemory           → run dist/trip.exe through Dr. Memory against the"
	@echo "                            full tests/test_modules suite (heap overflow /"
	@echo "                            use-after-free / uninitialized-read detection)"
	@echo "  make drmemory-file FILE=path/to/script.tp"
	@echo "                          → same as above, but only against one .tp script"
	@echo "                            — much faster for chasing a specific bug"
	@echo "  make installer          → build installer"
	@echo "  make release            → FULL gated pipeline: unit tests (GCC+Clang) →"
	@echo "                            sanitizer pass (Clang+UBSan trap mode, tests/test_modules"
	@echo "                            run through it) → release build (GCC, tests/test_modules"
	@echo "                            run through it) → bundle + installer."
	@echo "                            Stops at the first failing stage — nothing"
	@echo "                            reaches dist/ unless everything passes."
	@echo "                            (Dr. Memory is NOT part of this — run it separately"
	@echo "                            with 'make drmemory', it's much slower.)"
	@echo "  make test               → build and run C unit tests (tests/) with \$$(CC)"
	@echo "                            (defaults to GCC — use CC=clang to switch)"
	@echo "  make test-all-compilers → run the full test suite under BOTH GCC and"
	@echo "                            Clang in one pass (recommended for CI)"
	@echo "  make clean              → clean build"
	@echo ""
	@echo "  Run this in the MINGW64 shell (not UCRT64/CLANG64) so 'which gcc'"
	@echo "  prints /mingw64/bin/gcc.exe — must match the MINGW= path above."
	@echo ""
	@echo "  IMPORTANT: 'clang' must ALSO resolve to /mingw64/bin/clang (the"
	@echo "  mingw-w64-x86_64-clang package), NOT a CLANG64-environment install."
	@echo "  CLANG64 uses a different runtime/ABI than MINGW64's gcc, so mixing"
	@echo "  them silently produces the same kind of stale/incompatible-DLL"
	@echo "  symptoms described in the bundle: target above. Verify with:"
	@echo "      which gcc clang    (both should print /mingw64/bin/...)"
	@echo "  MSYS2 deps: pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-pcre2 mingw-w64-x86_64-clang"
	@echo ""