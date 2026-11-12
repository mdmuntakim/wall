/* start.S - Wall runtime entry point (Linux AArch64).
 *
 * The kernel enters here with SP pointing at:
 *   [sp]     argc
 *   [sp+8]   argv[0]
 *   ...
 *   NULL
 *   envp...
 *
 * We pass argc/argv to main(), then exit through the exit_group syscall.
 */
    .text
    .global _start
    .type _start, %function
_start:
    ldr x0, [sp]          /* argc */
    add x1, sp, #8        /* argv */
    bl  main              /* int main(int, char**) */
    mov w0, w0            /* status */
    mov x8, #94           /* exit_group */
    svc #0
1:  b 1b
    .size _start, .-_start
