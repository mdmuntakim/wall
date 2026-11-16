// syscall.s - raw Linux system-call wrappers for the Wall self-hosted
// runtime (AArch64).  This is the ONLY assembly in the Wall implementation
// path: everything above these wrappers is .wa.  Assembled by objc (or any
// AArch64 assembler), linked by the Wall linker.
//
// Each wrapper follows the C ABI: arguments in x0..x5, return in x0.

    .text

// exit(code) - never returns.  exit_group(94) terminates every thread.
    .globl rt_exit
    .type rt_exit, %function
rt_exit:
    mov x8, 94
    svc #0
    .size rt_exit, .-rt_exit

// write(fd, buf, count) -> i64 bytes written or negative errno
    .globl rt_write
    .type rt_write, %function
rt_write:
    mov x8, 64
    svc #0
    ret
    .size rt_write, .-rt_write

// read(fd, buf, count) -> i64 bytes read or negative errno
    .globl rt_read
    .type rt_read, %function
rt_read:
    mov x8, 63
    svc #0
    ret
    .size rt_read, .-rt_read

// close(fd) -> i64
    .globl rt_close
    .type rt_close, %function
rt_close:
    mov x8, 57
    svc #0
    ret
    .size rt_close, .-rt_close

// open(path, flags, mode) -> i64 fd or negative errno
// (wraps openat with dirfd = AT_FDCWD)
    .globl rt_open
    .type rt_open, %function
rt_open:
    mov x8, 56              // __NR_openat
    mov x4, x0              // path
    mov x0, -100            // dirfd = AT_FDCWD
    mov x1, x4              // path
    // x2 = flags, x3 = mode as passed
    svc #0
    ret
    .size rt_open, .-rt_open

// brk(addr) -> i64 current break (0 argument queries)
    .globl rt_brk
    .type rt_brk, %function
rt_brk:
    mov x8, 214             // __NR_brk
    svc #0
    ret
    .size rt_brk, .-rt_brk

// _start - process entry.  The kernel hands us argc at [sp], argv at [sp+8].
// Call main(argc, argv) and exit with its result.
    .globl _start
    .type _start, %function
_start:
    ldr x0, [sp]            // argc
    add x1, sp, 8           // argv
    bl main
    mov x8, 94              // exit_group(result)
    svc #0
    .size _start, .-_start
