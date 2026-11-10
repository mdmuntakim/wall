/* wall - Wall Compiler driver.
 *
 * Command line driver for the Wall compiler. It loads a backend shared
 * library (libwall.so by default) exposing `wall_version` and
 * `wall_compile` and drives the compilation to LLVM IR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include "wall.h"

static void *g_lib = NULL;
static int (*g_wc)(const char *, int, char **, char **) = NULL;
static int (*g_wv)(char *, int) = NULL;
static const char *g_b_dir = NULL;
static int tmpseq = 0;

static void print_version(void) {
  char buf[256];
  if (g_wv) { g_wv(buf, (int)sizeof(buf)); }
  else { snprintf(buf, sizeof(buf), "Wall Compiler version 1.0.0"); }
  printf("%s\n", buf);
}

static void print_usage(FILE *f) {
  fprintf(f,
    "Wall Compiler version 1.0.0\n"
    "Usage: wall [options] <input file>\n"
    "\n"
    "Wall is a modern systems programming language that compiles to LLVM IR.\n"
    "\n"
    "Arguments:\n"
    "  <input file>        Wall source file (.wa)\n"
    "\n"
    "Options:\n"
    "  -h, --help           Print this help message and exit\n"
    "  -v, --version        Print version information and exit\n"
    "  -o <filename>        Set output filename (default: tmp_<pid>_<n>.s)\n"
    "  -S, --emit-llvm      Emit LLVM IR (assembly) to the output filename\n"
    "  -O <level>           Optimization level: 0, 1, 2, 3, s, S or z (default 0)\n"
    "  -std=<standard>      Set language standard (e.g. wall21)\n"
    "  -I <dir>             Add directory to the include search path\n"
    "  -L <dir>             Add directory to the library search path\n"
    "  -l <lib>             Library to link against (backend plugins)\n"
    "  --target=<triple>    Set the target triple (default: aarch64-unknown-linux-android)\n"
    "  --backend=<path>     Backend plugin shared object (default: libwall.so)\n"
    "  -B <dir>             Backend search directory (default: ./)\n"
    "  -Wall                Enable all warnings\n"
    "  -Wextra             Enable extra warnings\n"
    "  -Werror             Treat warnings as errors\n"
    "\n"
    "The backend shared object exports:\n"
    "  int wall_version(char *buf, int buflen);\n"
    "  int wall_compile(const char *source, int optimize_level,\n"
    "                   char **out_ir, char **err);\n"
    "\n");
}

static int load_backend(const char *explicit_path) {
  const char *cands[16];
  int nc = 0;
  const char *env = getenv("WALL_BACKEND");
  if (env && env[0]) cands[nc++] = env;
  if (explicit_path && explicit_path[0]) cands[nc++] = explicit_path;
  if (g_b_dir && g_b_dir[0]) {
    static char p[1024];
    snprintf(p, sizeof(p), "%s/libwall.so", g_b_dir);
    cands[nc++] = p;
  }
  {
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = 0;
      char *slash = strrchr(exe, '/');
      if (slash) { *slash = 0; static char p[1024]; snprintf(p, sizeof(p), "%s/libwall.so", exe); cands[nc++] = p; }
    }
  }
  cands[nc++] = "libwall.so";
  for (int i = 0; i < nc; i++) {
    void *h = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
    if (h) { g_lib = h; break; }
  }
  if (!g_lib) { fprintf(stderr, "wall: error: unable to load backend shared library: %s\n", dlerror()); return -1; }
  g_wc = (int (*)(const char *, int, char **, char **))dlsym(g_lib, "wall_compile");
  g_wv = (int (*)(char *, int))dlsym(g_lib, "wall_version");
  if (!g_wc || !g_wv) { fprintf(stderr, "wall: error: backend library missing wall_compile/wall_version symbols\n"); return -1; }
  return 0;
}

static char *read_file(const char *path, int *ok) {
  *ok = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)sz + 1);
  size_t rd = fread(buf, 1, (size_t)sz, f);
  buf[rd] = 0;
  fclose(f);
  buf[rd] = 0;
  *ok = 1;
  return buf;
}

static int write_file(const char *path, const char *data) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fputs(data, f);
  fclose(f);
  return 0;
}

int main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  int emit_llvm = 0;
  int opt_level = 0;
  const char *backend_arg = NULL;
  int ninputs = 0;
  const char **inputs = (const char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof(char *));
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { print_usage(stdout); return 0; }
    else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) { if (load_backend(NULL) == 0) print_version(); else { fprintf(stderr, "wall: error: cannot load backend\n"); return 1; } return 0; }
    else if (strcmp(a, "-o") == 0) { if (i + 1 < argc) output = argv[++i]; }
    else if (strcmp(a, "-S") == 0 || strcmp(a, "--emit-llvm") == 0) { emit_llvm = 1; }
    else if (strncmp(a, "-O", 2) == 0) { opt_level = atoi(a + 2); if (opt_level < 0) opt_level = 0; }
    else if (strncmp(a, "-I", 2) == 0) { /* include path */ }
    else if (strncmp(a, "-L", 2) == 0) { /* library path */ }
    else if (strncmp(a, "-l", 2) == 0) { /* link library */ }
    else if (strncmp(a, "-B", 2) == 0) { g_b_dir = a + 2; }
    else if (strncmp(a, "--backend=", 10) == 0) { backend_arg = a + 10; }
    else if (strncmp(a, "--target=", 9) == 0) { /* target triple */ }
    else if (strcmp(a, "-Wall") == 0 || strcmp(a, "-Wextra") == 0 || strcmp(a, "-Werror") == 0) { /* warnings */ }
    else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "wall: error: unknown argument '%s'\n", a); print_usage(stderr); return 1; }
    else { inputs[ninputs++] = argv[i]; if (ninputs == 1) input = argv[i]; }
  }
  if (ninputs == 0) { fprintf(stderr, "wall: error: no input files\n"); print_usage(stderr); return 1; }
  if (ninputs > 1 && !output) { /* write per input */ }
  if (load_backend(backend_arg) != 0) { return 1; }
  for (int i = 0; i < ninputs; i++) {
    int ok = 0;
    char *src = read_file(inputs[i], &ok);
    if (!ok) { fprintf(stderr, "wall: error: cannot open file '%s'\n", inputs[i]); return 1; }
    char *ir = NULL;
    char *err = NULL;
    int rc = g_wc(src, opt_level, &ir, &err);
    if (rc != 0) { fprintf(stderr, "wall: error: %s\n", err ? err : "internal error"); return 1; }
    char outpath[2048];
    if (ninputs == 1 && output) snprintf(outpath, sizeof(outpath), "%s", output);
    else snprintf(outpath, sizeof(outpath), "tmp_%d_%d.s", (int)getpid(), tmpseq++);
    if (write_file(outpath, ir) != 0) { fprintf(stderr, "wall: error: cannot write output file '%s'\n", outpath); return 1; }
  }
  return 0;
}
