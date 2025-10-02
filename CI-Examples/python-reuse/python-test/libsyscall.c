#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdint.h>

// A generic wrapper for syscalls with exactly 3 arguments.
// Takes: syscall number, and 3 arguments (uintptr_t for safety).
// Returns: syscall result (long). On error, returns -1 and sets errno.
long do_syscall3(long n, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return syscall(n, a1, a2, a3);
}