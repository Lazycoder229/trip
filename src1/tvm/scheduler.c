#include "tvm.h"

// Monotonic milliseconds, for idle-connection deadlines.
long long nowMs(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// ── Ready Queue Helpers ──────────────────────────────────────────────────
void enqueueReady(Fiber* f) {
    f->next = NULL;
    if (vm.readyTail) {
        vm.readyTail->next = f;
        vm.readyTail = f;
    } else {
        vm.readyHead = vm.readyTail = f;
    }
}

Fiber* dequeueReady(void) {
    Fiber* f = vm.readyHead;
    if (!f) return NULL;
    vm.readyHead = f->next;
    if (!vm.readyHead) vm.readyTail = NULL;
    f->next = NULL;
    return f;
}

// ── I/O Reactor (Blocked Queue) ──────────────────────────────────────────
void enqueueBlocked(Fiber* f) {
    f->next = NULL;
    if (vm.blockedTail) {
        vm.blockedTail->next = f;
        vm.blockedTail = f;
    } else {
        vm.blockedHead = vm.blockedTail = f;
    }
}

// Unlinks a specific fiber from the blocked list. Needed before moving a
// fiber straight to the ready queue from somewhere other than the normal
// pollBlockedFibers() sweep (e.g. httpMultiPoll() completion), since a
// fiber must never be linked into both lists at once (see Fiber::next).
void dequeueBlocked(Fiber* target) {
    Fiber* prev = NULL;
    Fiber* f = vm.blockedHead;
    while (f != NULL) {
        if (f == target) {
            if (prev) prev->next = f->next; else vm.blockedHead = f->next;
            if (f == vm.blockedTail) vm.blockedTail = prev;
            f->next = NULL;
            return;
        }
        prev = f;
        f = f->next;
    }
}

void pollBlockedFibers(void) {
    httpMultiPoll();

    // Fibers blocked on an HTTP request (waitFd == TRIP_INVALID_SOCKET) are
    // driven by curl_multi via httpMultiPoll() above, not by poll() — they
    // have no real fd to wait on. Only count/track fibers with a real fd
    // for the pollfd array below.
    int n = 0;
    for (Fiber* f = vm.blockedHead; f != NULL; f = f->next) {
        if (f->waitFd != TRIP_INVALID_SOCKET) n++;
    }

    // Nothing with a real fd to wait on. If there's still an HTTP request
    // in flight, don't return — that would send us straight back into
    // nextFiberToRun() -> pollBlockedFibers() in a tight spin with no
    // progress. Instead, just return; the caller (nextFiberToRun()) already
    // loops calling us, and httpMultiPoll() above continues to progress
    // curl on every call — but to avoid a pure busy spin, briefly rest.
    if (n == 0) {
        if (httpHasPending()) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, NULL);
#endif
        }
        return;
    }

    struct pollfd* fds = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)n);
    if (!fds) return;

    long long soonestDeadline = -1;
    int i = 0;
    for (Fiber* f = vm.blockedHead; f != NULL; f = f->next) {
        if (f->waitFd == TRIP_INVALID_SOCKET) continue;
        fds[i].fd     = (int)f->waitFd;
        fds[i].events = f->waitForWrite ? POLLOUT : POLLIN;
        fds[i].revents = 0;
        if (f->waitDeadlineMs >= 0 &&
            (soonestDeadline < 0 || f->waitDeadlineMs < soonestDeadline)) {
            soonestDeadline = f->waitDeadlineMs;
        }
        i++;
    }

    int timeoutArg = -1;
    if (soonestDeadline >= 0) {
        long long remaining = soonestDeadline - nowMs();
        timeoutArg = (int)(remaining > 0 ? remaining : 0);
    }
    // If there's an HTTP request in flight alongside real-fd waits, cap the
    // wait so we come back and re-drive httpMultiPoll() regularly instead
    // of blocking until a TCP fd (which may never fire) wakes us up.
    if (httpHasPending() && (timeoutArg < 0 || timeoutArg > 20)) {
        timeoutArg = 20;
    }

    int r = poll(fds, (nfds_t)n, timeoutArg);
    long long now = nowMs();

    Fiber* prev = NULL;
    Fiber* f = vm.blockedHead;
    i = 0;
    while (f != NULL) {
        Fiber* nextF = f->next;
        if (f->waitFd == TRIP_INVALID_SOCKET) {
            // HTTP-blocked fiber wasn't part of the poll() set at all;
            // httpMultiPoll() (called above) is solely responsible for
            // moving it to the ready queue when its request completes.
            prev = f;
            f = nextF;
            continue;
        }
        bool ready   = r > 0 && (fds[i].revents & (fds[i].events | POLLERR | POLLHUP | POLLNVAL)) != 0;
        bool expired = f->waitDeadlineMs >= 0 && now >= f->waitDeadlineMs;
        if (ready || expired) {
            if (prev) prev->next = nextF; else vm.blockedHead = nextF;
            if (f == vm.blockedTail) vm.blockedTail = prev;
            f->timedOut       = expired && !ready;
            f->waitFd         = TRIP_INVALID_SOCKET;
            f->waitDeadlineMs = -1;
            f->state = FIBER_READY;
            enqueueReady(f);
        } else {
            prev = f;
        }
        f = nextF;
        i++;
    }
    free(fds);
}

Fiber* nextFiberToRun(void) {
    while (vm.readyHead == NULL) {
        if (vm.blockedHead == NULL) return NULL;
        pollBlockedFibers();
    }
    return dequeueReady();
}

// ── waitAll() implementation ─────────────────────────────────────────────
// Runs the scheduler until there are no more fibers other than the caller
// (main fiber) in any queue. Called by BUILTIN_WAIT_ALL.
// Returns INTERPRET_OK when all spawned fibers are done, or
// INTERPRET_RUNTIME_ERROR if any fiber crashed.
InterpretResult runUntilAllDone(void) {
    Fiber* main = vm.current;

    while (vm.readyHead != NULL || vm.blockedHead != NULL) {
        // If there are ready fibers, run them all first.
        while (vm.readyHead != NULL) {
            Fiber* next = dequeueReady();
            if (next == main) {
                // Main fiber is back in the ready queue — that means
                // everyone else is done. Re-seat it as current and return.
                vm.current = main;
                vm.current->state = FIBER_RUNNING;
                return INTERPRET_OK;
            }
            next->state = FIBER_RUNNING;
            vm.current = next;
            InterpretResult res = run();
            // run() returns INTERPRET_YIELD on every fiber switch —
            // that's normal, just keep looping.
            if (res == INTERPRET_RUNTIME_ERROR) {
                // Fiber crashed. Record it, keep going so other fibers finish.
                vm.anyFiberCrashed = true;
            }
        }
        // No ready fibers — poll I/O to unblock any waiting ones.
        if (vm.blockedHead != NULL) {
            pollBlockedFibers();
        }
    }

    // All spawned fibers finished. Restore main as current.
    vm.current = main;
    vm.current->state = FIBER_RUNNING;
    return vm.anyFiberCrashed ? INTERPRET_RUNTIME_ERROR : INTERPRET_OK;
}

InterpretResult blockCurrentFiberOnIO(TripSocketHandle fd, bool forWrite, long timeoutMs) {
    vm.current->state = FIBER_BLOCKED_IO;
    vm.current->waitFd = fd;
    vm.current->waitForWrite = forWrite;
    vm.current->waitDeadlineMs = timeoutMs < 0 ? -1 : nowMs() + timeoutMs;
    vm.current->timedOut = false;
    enqueueBlocked(vm.current);

    Fiber* next = nextFiberToRun();
    next->state = FIBER_RUNNING;
    vm.current = next;
    return INTERPRET_YIELD;
}