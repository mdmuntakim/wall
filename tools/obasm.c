/* obasm - Object Assembler.
 *
 * Usage: obasm <input.s> [-o <output.o>] [--target=<triple>]
 *
 * Assembles LLVM textual assembly (.s) into a native object file (.o).
 * Pipeline: llvm-as <input.s> -> bitcode, clang -c <bitcode> -> object.
 * The backend used is selected with --target=<triple>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int run_cmd(const char *cmd) {
  int rc = system(cmd);
  if (rc != 0) return -1;
  return 0;
}

static int assemble(const char *in, const char *bc, const char *out, const char *mt) {
  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "llvm-as -o %s %s", bc, in);
  if (run_cmd(cmd) != 0) return -1;
  if (mt && mt[0]) snprintf(cmd, sizeof(cmd), "clang -c -mtriple=%s -o %s %s", mt, out, bc);
  else snprintf(cmd, sizeof(cmd), "clang -c -o %s %s", out, bc);
  if (run_cmd(cmd) != 0) return -1;
  return 0;
}

static void print_usage(FILE *f) {
  fprintf(f,
    "obasm 1.0.0\n"
    "Usage: obasm <input.s> [-o <output.o>] [--target=<triple>]\n"
    "\n"
    "Assembles LLVM textual assembly into a native object file.\n"
    "\n"
    "Options:\n"
    "  -o <filename>        Set output filename (default: <input>.o)\n"
    "  --target=<triple>    Set the target triple (default: llvm default)\n"
    "  -h, --help           Print this help message and exit\n");
}

int main(int argc, char **argv) {
  const char *in = NULL;
  const char *out = NULL;
  const char *mt = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0) { if (i + 1 < argc) out = argv[++i]; }
    else if (strncmp(argv[i], "--target=", 9) == 0) { mt = argv[i] + 9; }
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { print_usage(stdout); return 0; }
    else if (argv[i][0] != '-') { in = argv[i]; }
  }
  if (!in) { fprintf(stderr, "obasm: error: no input file\n"); print_usage(stderr); return 1; }
  if (!out) {
    const char *dot = strrchr(in, '.');
    size_t n = dot ? (size_t)(dot - in) : strlen(in);
    char *o = (char *)malloc(n + 3);
    memcpy(o, in, n);
    memcpy(o + n, ".o", 2);
    out = o;
  }
  char bc[1024];
  snprintf(bc, sizeof(bc), "%s.bc", in);
  if (assemble(in, bc, out, mt) != 0) {
    fprintf(stderr, "obasm: error: failed to assemble '%s'\n", in);
    return 1;
  }
  return 0;
}
