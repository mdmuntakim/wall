/* wall - Wall toolchain multicall driver.
 *
 * `wall` is the user-facing driver.  It loads libwall.so and dispatches to the
 * compiler, the object assembler and the linker that live in that shared
 * library:
 *
 *   wall file.wa                 compile, assemble and link (build)
 *   wall -S file.wa              emit LLVM IR only
 *   wall -c file.wa              emit a relocatable object
 *   wall as file.s -o file.o     assemble (also via the obasm symlink)
 *   wall link file.o -o prog     link    (also via the linker symlink)
 *   wall run file.wa -- args     build and run
 *
 * Compatibility symlinks are dispatched by argv[0]: `wallc` behaves like
 * `wall`, `obasm` like `wall as`, and `linker` like `wall link`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include "wall/wall.h"
#include "wall_log.h"

typedef int (*wall_compile_fn)(const char *, int, char **, char **);
typedef int (*wall_version_fn)(char *, int);
typedef int (*wall_assemble_fn)(int, char **);
typedef int (*wall_link_fn)(int, char **);
typedef void (*wall_opts_fn)(FILE *, const char *);
typedef const char *(*wall_str_fn)(void);

static void *g_lib;
static wall_compile_fn g_compile;
static wall_version_fn g_version;
static wall_assemble_fn g_assemble;
static wall_link_fn g_link;
static wall_opts_fn g_print_options;
static wall_str_fn g_build_comment;
static int g_verbose;

#define MAX_ARGS 512

typedef struct {
  const char *inputs[MAX_ARGS];
  int ninputs;
  const char *output;
  const char *target;
  const char *backend;
  const char *const *include_dirs;
  const char *include_dirs_storage[MAX_ARGS];
  int ninclude_dirs;
  const char *asflags[MAX_ARGS];
  int nasflags;
  const char *ldflags[MAX_ARGS];
  int nldflags;
  int opt_level;
  int emit_ir;
  int compile_only;
  int keep_temps;
  int werror;
  int shared;
  int static_link;
  int strip;
  int verbose;
  int want_help;
  int want_version;
  int want_help_all;
} Drv;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void base_dir(const char *p, char *out, size_t n) {
  const char *s = strrchr(p, '/');
  if (!s) { snprintf(out, n, "."); return; }
  size_t k = (size_t)(s - p);
  if (k == 0) k = 1;
  if (k >= n) k = n - 1;
  memcpy(out, p, k);
  out[k] = 0;
}

/* Output name for one input: basename with the extension replaced. */
static void replace_ext(const char *in, const char *ext, char *out, size_t n) {
  const char *b = base_name(in);
  const char *dot = strrchr(b, '.');
  size_t k = dot ? (size_t)(dot - b) : strlen(b);
  if (k + strlen(ext) + 1 > n) k = n - strlen(ext) - 1;
  memcpy(out, b, k);
  out[k] = 0;
  strncat(out, ext, n - strlen(out) - 1);
}

static int has_suffix(const char *p, const char *s) {
  size_t n = strlen(p), m = strlen(s);
  return n >= m && !strcmp(p + n - m, s);
}

static char *read_text(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *b = (char *)malloc((size_t)sz + 1);
  if (!b) { fclose(f); return NULL; }
  size_t rd = fread(b, 1, (size_t)sz, f);
  fclose(f);
  b[rd] = 0;
  return b;
}

static int write_text(const char *path, const char *s) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t n = strlen(s);
  int ok = fwrite(s, 1, n, f) == n;
  fclose(f);
  return ok ? 0 : -1;
}

static char *xstrdup(const char *s) {
  char *p = strdup(s ? s : "");
  if (!p) { WALL_LOGE("wall", "out of memory\n"); exit(1); }
  return p;
}

static void version(void) {
  char buf[512];
  if (g_version && g_version(buf, (int)sizeof(buf)) == 0 && buf[0]) {
    printf("%s\n", buf);
  } else if (g_build_comment && g_build_comment()[0]) {
    printf("%s\n", g_build_comment());
  } else {
    printf("wall version 1.0.0\n");
  }
}

static void usage(FILE *f) {
  fprintf(f,
    "wall version 1.0.0 - Wall toolchain driver\n"
    "Usage: wall [command] [options] <files...>\n"
    "\n"
    "Commands:\n"
    "  build (default)      Compile, assemble and link Wall sources\n"
    "  compile, cc          Compile Wall sources to LLVM IR or objects\n"
    "  as, assemble         Assemble LLVM IR (.s/.ll) to an object\n"
    "  link, ld             Link objects and archives into an executable\n"
    "  run                  Build a Wall program and run it\n"
    "  help                 Show this help\n"
    "  version              Show the version\n"
    "\n"
    "Common options:\n"
    "  -o <file>            Output file name\n"
    "  -S                   Emit LLVM IR only\n"
    "  -c                   Compile and assemble, do not link\n"
    "  -O0..3, -Os, -Oz     Optimisation level\n"
    "  -g                   Emit DWARF debug information\n"
    "  -I <dir>             Add a source include directory\n"
    "  -L <dir>             Add a link library search directory\n"
    "  -l <lib>             Link a library\n"
    "  -Wl,<opt>            Pass an option to the linker\n"
    "  -Wa,<opt>            Pass an option to the assembler\n"
    "  -march=, -mcpu=      Select target architecture / CPU\n"
    "  -mabi=, -mfloat-abi= Select the ABI / floating-point ABI\n"
    "  -mfpu=, -mthumb      Select FPU / Thumb mode\n"
    "  -m64, -m32           Select 64-bit / 32-bit mode\n"
    "  --target=<triple>    Select the target triple\n"
    "  -static              Produce a static executable\n"
    "  -shared              Produce a shared object\n"
    "  --keep-temps         Keep intermediate .s and .o files\n"
    "  -v, --verbose        Show the underlying commands\n"
    "  -h, --help           Show this help\n"
    "  -V, --version        Show the version\n"
    "  --help=all           List every accepted assembler/linker option\n");
}

/* ------------------------------------------------------------------ */
/* backend loading                                                    */
/* ------------------------------------------------------------------ */

static void try_backend(const char *path, const char **names, int *n) {
  if (path && path[0] && *n < 64) names[(*n)++] = path;
}

static int load_backend(const char *explicit_path) {
  const char *cands[64];
  int nc = 0;
  const char *env = getenv("WALL_BACKEND");
  try_backend(env, cands, &nc);
  try_backend(explicit_path, cands, &nc);

  char exe[PATH_MAX];
  ssize_t en = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (en > 0) {
    exe[en] = 0;
    char dir[PATH_MAX];
    base_dir(exe, dir, sizeof(dir));
    static char p1[PATH_MAX], p2[PATH_MAX], p3[PATH_MAX];
    snprintf(p1, sizeof(p1), "%s/libwall.so", dir);
    snprintf(p2, sizeof(p2), "%s/../lib/libwall.so", dir);
    snprintf(p3, sizeof(p3), "%s/../lib/wall/libwall.so", dir);
    try_backend(p1, cands, &nc);
    try_backend(p2, cands, &nc);
    try_backend(p3, cands, &nc);
  }
  const char *prefix = getenv("PREFIX");
  if (prefix && prefix[0]) {
    static char pp[PATH_MAX];
    snprintf(pp, sizeof(pp), "%s/lib/libwall.so", prefix);
    try_backend(pp, cands, &nc);
  }
  try_backend("./libwall.so", cands, &nc);
  try_backend("libwall.so", cands, &nc);

  const char *last = "not found";
  for (int i = 0; i < nc; i++) {
    void *h = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
    if (h) {
      g_lib = h;
      if (g_verbose) fprintf(stderr, "wall: loaded backend %s\n", cands[i]);
      break;
    }
    last = dlerror();
  }
  if (!g_lib) {
    WALL_LOGE("wall", "unable to load libwall.so: %s\n", last ? last : "not found");
    WALL_LOGN("wall", "set WALL_BACKEND or use --backend=<path>\n");
    return -1;
  }

  g_compile = (wall_compile_fn)dlsym(g_lib, "wall_compile");
  g_version = (wall_version_fn)dlsym(g_lib, "wall_version");
  g_assemble = (wall_assemble_fn)dlsym(g_lib, "wall_assemble");
  g_link = (wall_link_fn)dlsym(g_lib, "wall_link");
  g_print_options = (wall_opts_fn)dlsym(g_lib, "wall_print_options");
  g_build_comment = (wall_str_fn)dlsym(g_lib, "wall_build_comment");
  if (!g_compile || !g_version) {
    WALL_LOGE("wall", "backend is missing wall_compile/wall_version\n");
    return -1;
  }
  if (!g_assemble || !g_link) {
    WALL_LOGE("wall", "backend is missing wall_assemble/wall_link\n");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* argument helpers                                                   */
/* ------------------------------------------------------------------ */

static void add_ldflag(Drv *d, const char *s) {
  if (d->nldflags < MAX_ARGS) d->ldflags[d->nldflags++] = s;
}
static void add_asflag(Drv *d, const char *s) {
  if (d->nasflags < MAX_ARGS) d->asflags[d->nasflags++] = s;
}

/* Append every comma separated component of `list` as one flag. */
static void add_csv(Drv *d, const char *list, int to_linker) {
  char *copy = xstrdup(list);
  char *p = copy;
  while (p && *p) {
    char *comma = strchr(p, ',');
    if (comma) *comma = 0;
    if (*p) {
      if (to_linker) add_ldflag(d, xstrdup(p));
      else add_asflag(d, xstrdup(p));
    }
    p = comma ? comma + 1 : NULL;
  }
}

/* The compiler takes one output buffer.  We drive it per translation unit. */
static int compile_to_ir(const char *src, int opt, char **ir, char **err) {
  return g_compile(src, opt, ir, err);
}

/* Textual `include "file";` handling.  Includes are resolved relative to the
 * including file and then through -I directories.  The result is one source
 * buffer; diagnostics therefore use positions in the expanded text. */
typedef struct { const char *const *dirs; int ndirs; int depth; } Inc;

static int resolve_include(Inc *in, const char *from, const char *name, char *out, size_t n) {
  char dir[PATH_MAX];
  base_dir(from, dir, sizeof(dir));
  snprintf(out, n, "%s/%s", dir, name);
  if (access(out, R_OK) == 0) return 0;
  for (int i = 0; i < in->ndirs; i++) {
    snprintf(out, n, "%s/%s", in->dirs[i], name);
    if (access(out, R_OK) == 0) return 0;
  }
  return -1;
}

static char *expand_includes(Inc *in, const char *path) {
  if (in->depth > 32) {
    WALL_LOGE("wall", "include nesting too deep at '%s'\n", path);
    return NULL;
  }
  char *src = read_text(path);
  if (!src) {
    WALL_LOGE("wall", "cannot open '%s': %s\n", path, strerror(errno));
    return NULL;
  }
  size_t cap = strlen(src) + 256, len = 0;
  char *out = (char *)malloc(cap);
  if (!out) { free(src); return NULL; }
  out[0] = 0;
  char *line = src;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    size_t ll = nl ? (size_t)(nl - line) : strlen(line);
    char save = line[ll];
    line[ll] = 0;
    const char *t = line;
    while (*t == ' ' || *t == '\t' || *t == '\r') t++;
    int handled = 0;
    if (!strncmp(t, "include", 7) && (t[7] == ' ' || t[7] == '\t' || t[7] == '"' || t[7] == '<')) {
      const char *q = t + 7;
      while (*q == ' ' || *q == '\t') q++;
      char close = 0;
      if (*q == '"') { close = '"'; q++; }
      else if (*q == '<') { close = '>'; q++; }
      if (close) {
        const char *e = strchr(q, close);
        if (e && e > q) {
          char name[PATH_MAX];
          size_t kn = (size_t)(e - q);
          if (kn >= sizeof(name)) kn = sizeof(name) - 1;
          memcpy(name, q, kn); name[kn] = 0;
          const char *r = e + 1;
          while (*r == ' ' || *r == '\t') r++;
          if (*r == ';') r++;
          while (*r == ' ' || *r == '\t' || *r == '\r') r++;
          if (!*r) {
            char found[PATH_MAX];
            if (resolve_include(in, path, name, found, sizeof(found)) != 0) {
              WALL_LOGE("wall", "include file '%s' not found (from '%s')\n", name, path);
              line[ll] = save; free(out); free(src); return NULL;
            }
            in->depth++;
            char *sub = expand_includes(in, found);
            in->depth--;
            if (!sub) { line[ll] = save; free(out); free(src); return NULL; }
            size_t sn = strlen(sub);
            while (len + sn + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
            memcpy(out + len, sub, sn); len += sn;
            out[len++] = '\n'; out[len] = 0;
            free(sub);
            handled = 1;
          }
        }
      }
    }
    if (!handled) {
      size_t sn = ll;
      while (len + sn + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
      memcpy(out + len, line, sn); len += sn;
      out[len++] = '\n'; out[len] = 0;
    }
    line[ll] = save;
    if (!nl) break;
    line = nl + 1;
  }
  free(src);
  return out;
}

/* ------------------------------------------------------------------ */
/* child process helper                                               */
/* ------------------------------------------------------------------ */

static int run_child(char *const av[], int verbose) {
  if (verbose) {
    fprintf(stderr, "wall: exec");
    for (int i = 0; av[i]; i++) fprintf(stderr, " %s", av[i]);
    fprintf(stderr, "\n");
  }
  pid_t pid = fork();
  if (pid < 0) { WALL_LOGE("wall", "fork: %s\n", strerror(errno)); return 1; }
  if (pid == 0) { execvp(av[0], av); _exit(127); }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return 1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 1;
}

/* ------------------------------------------------------------------ */
/* tool invocations                                                   */
/* ------------------------------------------------------------------ */

static const char *build_comment(void) {
  return (g_build_comment && g_build_comment()[0]) ? g_build_comment() : WALL_VERSION_STRING;
}

static int invoke_assemble(Drv *d, const char *in, const char *out) {
  char *av[128];
  int n = 0;
  av[n++] = (char *)"obasm";
  av[n++] = (char *)in;
  av[n++] = (char *)"-o";
  av[n++] = (char *)out;
  if (d->target) { av[n++] = (char *)"--target"; av[n++] = (char *)d->target; }
  for (int i = 0; i < d->nasflags && n < 120; i++) av[n++] = (char *)d->asflags[i];
  av[n++] = (char *)"--comment";
  av[n++] = (char *)build_comment();
  av[n] = NULL;
  return g_assemble(n, av);
}

static int invoke_link(Drv *d, char **objs, int nobj, const char *out) {
  char *av[256];
  int n = 0;
  av[n++] = (char *)"linker";
  for (int i = 0; i < nobj && n < 240; i++) av[n++] = objs[i];
  av[n++] = (char *)"-o";
  av[n++] = (char *)out;
  if (d->target) {
    char t[512];
    snprintf(t, sizeof(t), "--target=%s", d->target);
    av[n++] = xstrdup(t);
  }
  for (int i = 0; i < d->nldflags && n < 248; i++) av[n++] = (char *)d->ldflags[i];
  av[n] = NULL;
  return g_link(n, av);
}

static int temp_path(char *out, size_t n, const char *tag) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0]) tmp = getenv("PREFIX");
  if (!tmp || !tmp[0]) tmp = "/data/data/com.termux/files/usr/tmp";
  snprintf(out, n, "%s/wall-%s-XXXXXX", tmp, tag);
  int fd = mkstemp(out);
  if (fd < 0) return -1;
  close(fd);
  unlink(out);
  return 0;
}

/* Compile one .wa file to IR and then to an object. */
static int compile_source(Drv *d, Inc *inc, const char *input, const char *objout) {
  char *src = expand_includes(inc, input);
  if (!src) return 1;
  char *ir = NULL, *err = NULL;
  int rc = compile_to_ir(src, d->opt_level, &ir, &err);
  free(src);
  if (rc != 0) {
    WALL_LOGE("wall", "%s: %s\n", input, err ? err : "compilation failed");
    free(err);
    return 1;
  }
  char irpath[PATH_MAX];
  if (d->keep_temps) {
    replace_ext(input, ".s", irpath, sizeof(irpath));
  } else if (temp_path(irpath, sizeof(irpath), "ir") != 0) {
    WALL_LOGE("wall", "cannot create a temporary file: %s\n", strerror(errno));
    free(ir);
    return 1;
  }
  if (write_text(irpath, ir) != 0) {
    WALL_LOGE("wall", "cannot write '%s'\n", irpath);
    if (!d->keep_temps) unlink(irpath);
    free(ir);
    return 1;
  }
  free(ir);
  int ar = invoke_assemble(d, irpath, objout);
  if (!d->keep_temps) unlink(irpath);
  if (ar != 0) {
    WALL_LOGE("wall", "assembly failed for '%s'\n", input);
    if (!d->keep_temps) unlink(objout);
    return 1;
  }
  return 0;
}

/* Compile one .wa file to IR text (for -S). */
static int compile_to_ir_file(Drv *d, Inc *inc, const char *input, const char *out) {
  char *src = expand_includes(inc, input);
  if (!src) return 1;
  char *ir = NULL, *err = NULL;
  int rc = compile_to_ir(src, d->opt_level, &ir, &err);
  free(src);
  if (rc != 0) {
    WALL_LOGE("wall", "%s: %s\n", input, err ? err : "compilation failed");
    free(err);
    return 1;
  }
  int w = write_text(out, ir);
  free(ir);
  if (w != 0) { WALL_LOGE("wall", "cannot write '%s'\n", out); return 1; }
  return 0;
}

/* ------------------------------------------------------------------ */
/* command implementations                                            */
/* ------------------------------------------------------------------ */

static int cmd_assemble(int argc, char **argv) {
  char *sub[256];
  int n = 0;
  sub[n++] = (char *)"obasm";
  for (int i = 0; i < argc && n < 255; i++) sub[n++] = argv[i];
  sub[n] = NULL;
  return g_assemble(n, sub);
}

static int cmd_link(int argc, char **argv) {
  char *sub[256];
  int n = 0;
  sub[n++] = (char *)"linker";
  for (int i = 0; i < argc && n < 255; i++) sub[n++] = argv[i];
  sub[n] = NULL;
  return g_link(n, sub);
}

static int parse_common(Drv *d, int argc, char **argv, int start) {
  for (int i = start; i < argc; i++) {
    const char *a = argv[i];
    const char *v = NULL;
    if (!strcmp(a, "--")) {
      for (i++; i < argc; i++) d->inputs[d->ninputs++] = argv[i];
      break;
    }
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { d->want_help = 1; continue; }
    if (!strcmp(a, "--help=all")) { d->want_help_all = 1; continue; }
    if (!strcmp(a, "-V") || !strcmp(a, "--version")) { d->want_version = 1; continue; }
    if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { d->verbose = 1; g_verbose = 1; continue; }
    if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      if (i + 1 >= argc) { WALL_LOGE("wall", "%s needs a file\n", a); return -1; }
      d->output = argv[++i]; continue;
    }
    if (!strcmp(a, "-S") || !strcmp(a, "--emit-llvm")) { d->emit_ir = 1; continue; }
    if (!strcmp(a, "-c")) { d->compile_only = 1; continue; }
    if (!strcmp(a, "--keep-temps") || !strcmp(a, "-save-temps")) { d->keep_temps = 1; continue; }
    if (!strcmp(a, "-g") || !strncmp(a, "-gdwarf", 7) || !strcmp(a, "-g0")) { add_asflag(d, a); continue; }
    if (!strcmp(a, "-w") || !strncmp(a, "-W", 2)) {
      if (!strncmp(a, "-Wl,", 4)) { add_csv(d, a + 4, 1); continue; }
      if (!strncmp(a, "-Wa,", 4)) { add_csv(d, a + 4, 0); continue; }
      if (!strncmp(a, "-Wp,", 4)) {
        WALL_LOGE("wall", "preprocessor options are not supported (%s)\n", a);
        return -1;
      }
      if (!strcmp(a, "-Werror")) d->werror = 1;
      /* Wall currently emits no warning classes; -W* is accepted so normal
       * build systems do not break, but it is recorded in asflags. */
      add_asflag(d, a);
      continue;
    }
    if (!strncmp(a, "-O", 2)) {
      const char *l = a + 2;
      if (!*l) { if (i + 1 < argc) l = argv[++i]; else { WALL_LOGE("wall", "-O needs a level\n"); return -1; } }
      if (!strcmp(l, "s")) d->opt_level = 4;
      else if (!strcmp(l, "z")) d->opt_level = 5;
      else d->opt_level = atoi(l);
      if (d->opt_level < 0) d->opt_level = 0;
      if (d->opt_level > 5) d->opt_level = 5;
      continue;
    }
    if (!strcmp(a, "-std") || !strncmp(a, "-std=", 5)) {
      const char *std = a[4] == '=' ? a + 5 : (i + 1 < argc ? argv[++i] : NULL);
      if (!std || strcmp(std, WALL_LANGUAGE_STD)) {
        WALL_LOGE("wall", "unsupported language standard '%s' (only %s)\n", std ? std : "", WALL_LANGUAGE_STD);
        return -1;
      }
      continue;
    }
    if (!strcmp(a, "--target")) {
      if (i + 1 >= argc) { WALL_LOGE("wall", "--target needs a triple\n"); return -1; }
      d->target = argv[++i]; continue;
    }
    if (!strncmp(a, "--target=", 9)) { d->target = a + 9; continue; }
    if (!strncmp(a, "-march", 6) || !strncmp(a, "-mcpu", 5) ||
        !strncmp(a, "-mabi", 5) || !strncmp(a, "-mfloat-abi", 11) ||
        !strncmp(a, "-mfpu", 5)) {
      if (strchr(a, '=')) { add_asflag(d, a); continue; }
      if (i + 1 >= argc) { WALL_LOGE("wall", "%s needs a value\n", a); return -1; }
      char *s = (char *)malloc(strlen(a) + strlen(argv[i + 1]) + 2);
      if (!s) return -1;
      sprintf(s, "%s=%s", a, argv[++i]);
      add_asflag(d, s);
      continue;
    }
    if (!strcmp(a, "-mthumb") || !strcmp(a, "-marm") ||
        !strcmp(a, "-m64") || !strcmp(a, "-m32") ||
        !strcmp(a, "--64") || !strcmp(a, "--32")) {
      if (!strcmp(a, "--64")) add_asflag(d, "-m64");
      else if (!strcmp(a, "--32")) add_asflag(d, "-m32");
      else add_asflag(d, a);
      continue;
    }
    if (!strcmp(a, "-I") || !strncmp(a, "-I", 2)) {
      if (a[2]) { d->include_dirs_storage[d->ninclude_dirs++] = a + 2; continue; }
      if (i + 1 >= argc) { WALL_LOGE("wall", "-I needs a directory\n"); return -1; }
      d->include_dirs_storage[d->ninclude_dirs++] = argv[++i];
      continue;
    }
    if (!strcmp(a, "-L") || !strncmp(a, "-L", 2) ||
        !strcmp(a, "-l") || !strncmp(a, "-l", 2) ||
        !strcmp(a, "-e") || !strcmp(a, "-T") || !strcmp(a, "-z") ||
        !strcmp(a, "-u") || !strcmp(a, "-R") || !strcmp(a, "-Map")) {
      add_ldflag(d, a);
      int needs = !strcmp(a, "-L") || !strcmp(a, "-l") ||
                  !strcmp(a, "-e") || !strcmp(a, "-T") || !strcmp(a, "-z") ||
                  !strcmp(a, "-u") || !strcmp(a, "-R") || !strcmp(a, "-Map");
      if (needs) {
        if (i + 1 >= argc) { WALL_LOGE("wall", "%s needs an argument\n", a); return -1; }
        add_ldflag(d, argv[++i]);
      }
      continue;
    }
    if (!strcmp(a, "-Xlinker")) {
      if (i + 1 >= argc) { WALL_LOGE("wall", "-Xlinker needs an argument\n"); return -1; }
      add_ldflag(d, argv[++i]);
      continue;
    }
    if (!strcmp(a, "-static") || !strcmp(a, "-shared") || !strcmp(a, "-pie") ||
        !strcmp(a, "-no-pie") || !strcmp(a, "-nostdlib") || !strcmp(a, "-nodefaultlibs") ||
        !strcmp(a, "-nostartfiles") || !strcmp(a, "-rdynamic") || !strcmp(a, "-s") ||
        !strcmp(a, "-static-libgcc") || !strcmp(a, "-Wl,--gc-sections")) {
      if (!strcmp(a, "-shared")) d->shared = 1;
      if (!strcmp(a, "-static")) d->static_link = 1;
      if (!strcmp(a, "-s")) d->strip = 1;
      add_ldflag(d, a);
      continue;
    }
    if (!strcmp(a, "-B")) {
      if (i + 1 >= argc) { WALL_LOGE("wall", "-B needs a directory\n"); return -1; }
      d->backend = argv[++i];
      continue;
    }
    if (!strncmp(a, "-B", 2)) {
      if (!strcmp(a, "-Bstatic") || !strcmp(a, "-Bdynamic")) { add_ldflag(d, a); continue; }
      d->backend = a + 2;
      continue;
    }
    if (!strncmp(a, "--backend=", 10)) { d->backend = a + 10; continue; }
    if (!strcmp(a, "-D") || !strncmp(a, "-D", 2) ||
        !strcmp(a, "-U") || !strncmp(a, "-U", 2)) {
      add_asflag(d, a);
      if ((!strcmp(a, "-D") || !strcmp(a, "-U")) && i + 1 < argc) {
        char *s = (char *)malloc(strlen(a) + strlen(argv[i + 1]) + 2);
        if (!s) return -1;
        sprintf(s, "%s%s", a, argv[++i]);
        add_asflag(d, s);
      }
      continue;
    }
    if (!strcmp(a, "-pthread")) { add_ldflag(d, a); add_asflag(d, a); continue; }
    if (!strcmp(a, "-fPIC") || !strcmp(a, "-fpic") || !strcmp(a, "-fPIE") || !strcmp(a, "-fpie")) {
      add_asflag(d, a); continue;
    }
    if (a[0] == '-' && a[1]) {
      WALL_LOGE("wall", "unknown option '%s' (use --help=all for the accepted catalogue)\n", a);
      return -1;
    }
    if (d->ninputs >= MAX_ARGS) { WALL_LOGE("wall", "too many input files\n"); return -1; }
    d->inputs[d->ninputs++] = a;
  }
  d->include_dirs = d->include_dirs_storage;
  return 0;
}

static int cmd_build(Drv *d) {
  Inc inc = { d->include_dirs, d->ninclude_dirs, 0 };

  if (d->want_help) { usage(stdout); return 0; }
  if (d->want_version) { version(); return 0; }
  if (d->want_help_all) { if (g_print_options) g_print_options(stdout, "all"); else usage(stdout); return 0; }
  if (d->ninputs == 0) { WALL_LOGE("wall", "no input files\n"); usage(stderr); return 1; }
  if (d->emit_ir && d->compile_only) { WALL_LOGE("wall", "-S and -c are mutually exclusive\n"); return 1; }
  if (d->emit_ir && d->ninputs > 1) { WALL_LOGE("wall", "-S accepts one input file\n"); return 1; }

  /* -S: IR only. */
  if (d->emit_ir) {
    const char *in = d->inputs[0];
    if (!has_suffix(in, ".wa")) { WALL_LOGE("wall", "-S expects a .wa file\n"); return 1; }
    char out[PATH_MAX];
    if (d->output) snprintf(out, sizeof(out), "%s", d->output);
    else replace_ext(in, ".s", out, sizeof(out));
    return compile_to_ir_file(d, &inc, in, out);
  }

  /* -c: object only. */
  if (d->compile_only) {
    if (d->ninputs > 1 && d->output) { WALL_LOGE("wall", "-o cannot be used with multiple inputs\n"); return 1; }
    for (int i = 0; i < d->ninputs; i++) {
      const char *in = d->inputs[i];
      char out[PATH_MAX];
      if (d->output) snprintf(out, sizeof(out), "%s", d->output);
      else replace_ext(in, ".o", out, sizeof(out));
      int rc;
      if (has_suffix(in, ".wa")) rc = compile_source(d, &inc, in, out);
      else rc = invoke_assemble(d, in, out);
      if (rc != 0) return 1;
    }
    return 0;
  }

  /* Link mode.  Compile Wall sources to temporary objects, pass objects and
   * archives to the linker. */
  char *objs[MAX_ARGS];
  int created[MAX_ARGS];
  int nobj = 0;
  int rc = 0;
  for (int i = 0; i < d->ninputs; i++) {
    const char *in = d->inputs[i];
    if (has_suffix(in, ".wa")) {
      char obj[PATH_MAX];
      if (d->keep_temps) replace_ext(in, ".o", obj, sizeof(obj));
      else if (temp_path(obj, sizeof(obj), "obj") != 0) {
        WALL_LOGE("wall", "cannot create a temporary file: %s\n", strerror(errno));
        rc = 1; break;
      }
      if (compile_source(d, &inc, in, obj) != 0) { rc = 1; break; }
      objs[nobj] = xstrdup(obj);
      created[nobj++] = !d->keep_temps;
    } else if (has_suffix(in, ".s") || has_suffix(in, ".ll")) {
      char obj[PATH_MAX];
      if (d->keep_temps) replace_ext(in, ".o", obj, sizeof(obj));
      else if (temp_path(obj, sizeof(obj), "obj") != 0) {
        WALL_LOGE("wall", "cannot create a temporary file: %s\n", strerror(errno));
        rc = 1; break;
      }
      if (invoke_assemble(d, in, obj) != 0) { rc = 1; break; }
      objs[nobj] = xstrdup(obj);
      created[nobj++] = !d->keep_temps;
    } else {
      objs[nobj] = xstrdup(in);
      created[nobj++] = 0;
    }
  }

  if (rc == 0) {
    const char *out = d->output ? d->output : "a.out";
    rc = invoke_link(d, objs, nobj, out);
    if (rc == 0) {
      chmod(out, 0755);
      if (d->verbose) fprintf(stderr, "wall: built %s\n", out);
    }
  }

  if (!d->keep_temps) {
    for (int i = 0; i < nobj; i++) {
      if (created[i]) unlink(objs[i]);
    }
  }
  for (int i = 0; i < nobj; i++) free(objs[i]);
  return rc;
}

static int cmd_run(Drv *d, int argc, char **argv, int start) {
  /* Parse options up to "--"; everything after belongs to the program. */
  int dstart = start;
  int sep = -1;
  for (int i = start; i < argc; i++) if (!strcmp(argv[i], "--")) { sep = i; break; }
  int parse_end = sep >= 0 ? sep : argc;
  char *saved[MAX_ARGS];
  int nsaved = 0;
  for (int i = sep >= 0 ? sep + 1 : argc; i < argc; i++) saved[nsaved++] = argv[i];

  /* Temporarily parse only the driver options. */
  char *tmp[8] = { (char *)"run", NULL };
  (void)tmp;
  int rc = parse_common(d, parse_end, argv, dstart);
  if (rc != 0) return 1;
  if (d->ninputs != 1) { WALL_LOGE("wall", "run needs one .wa input\n"); return 1; }
  if (!has_suffix(d->inputs[0], ".wa")) { WALL_LOGE("wall", "run expects a .wa file\n"); return 1; }

  char exe[PATH_MAX];
  if (temp_path(exe, sizeof(exe), "run") != 0) {
    WALL_LOGE("wall", "cannot create a temporary file: %s\n", strerror(errno));
    return 1;
  }
  const char *save_out = d->output;
  d->output = exe;
  d->compile_only = 0;
  d->emit_ir = 0;
  rc = cmd_build(d);
  d->output = save_out;
  if (rc != 0) { unlink(exe); return 1; }

  char *av[MAX_ARGS];
  av[0] = exe;
  for (int i = 0; i < nsaved && i < MAX_ARGS - 2; i++) av[i + 1] = saved[i];
  av[nsaved + 1] = NULL;
  unlink(exe);
  int st = run_child(av, d->verbose);
  return st;
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *prog = base_name(argv[0]);

  Drv d;
  memset(&d, 0, sizeof(d));
  d.opt_level = 0;

  /* Compatibility symlinks. */
  if (!strcmp(prog, "obasm")) {
    if (load_backend(NULL) != 0) return 1;
    return cmd_assemble(argc - 1, argv + 1);
  }
  if (!strcmp(prog, "linker") || !strcmp(prog, "ld.wall") || !strcmp(prog, "wall-ld")) {
    if (load_backend(NULL) != 0) return 1;
    return cmd_link(argc - 1, argv + 1);
  }

  int start = 1;
  const char *cmd = "build";
  if (argc > 1 && argv[1][0] != '-') {
    if (!strcmp(argv[1], "build")) { cmd = "build"; start = 2; }
    else if (!strcmp(argv[1], "compile") || !strcmp(argv[1], "cc")) { cmd = "build"; start = 2; }
    else if (!strcmp(argv[1], "as") || !strcmp(argv[1], "assemble")) {
      if (load_backend(NULL) != 0) return 1;
      return cmd_assemble(argc - 2, argv + 2);
    } else if (!strcmp(argv[1], "link") || !strcmp(argv[1], "ld")) {
      if (load_backend(NULL) != 0) return 1;
      return cmd_link(argc - 2, argv + 2);
    } else if (!strcmp(argv[1], "run")) { cmd = "run"; start = 2; }
    else if (!strcmp(argv[1], "help")) { usage(stdout); return 0; }
    else if (!strcmp(argv[1], "version")) {
      if (load_backend(NULL) != 0) return 1;
      version();
      return 0;
    }
  }

  /* Pre-scan for --backend/-B so we can load before parsing everything. */
  for (int i = start; i < argc; i++) {
    if (!strncmp(argv[i], "--backend=", 10)) d.backend = argv[i] + 10;
    else if (!strcmp(argv[i], "-B") && i + 1 < argc) d.backend = argv[i + 1];
    else if (!strncmp(argv[i], "-B", 2) && strcmp(argv[i], "-Bstatic") && strcmp(argv[i], "-Bdynamic")) d.backend = argv[i] + 2;
  }

  if (load_backend(d.backend) != 0) return 1;

  if (!strcmp(prog, "wallc")) {
    /* wallc is the compiler-facing name; it uses the same full driver. */
  }

  if (!strcmp(cmd, "run")) return cmd_run(&d, argc, argv, start);

  if (parse_common(&d, argc, argv, start) != 0) return 1;
  return cmd_build(&d);
}
