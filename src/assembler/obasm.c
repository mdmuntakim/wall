/* obasm - Wall object assembler.
 *
 * Assembles the LLVM textual assembly/IR emitted by `wall` into a native
 * relocatable object.  The assembler is part of libwall.so; the merged `wall`
 * driver calls wall_assemble(), and a compatibility `obasm` symlink dispatches
 * here through argv[0].
 *
 * Target/ABI selection is real: every -m* / --target flag is forwarded to the
 * clang integrated assembler/backend.  Options that have no meaning for LLVM
 * IR input are rejected explicitly rather than silently ignored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "elfcomment.h"
#include "wall_cli.h"
#include "wall_log.h"

#ifndef WALL_DEFAULT_COMMENT
#define WALL_DEFAULT_COMMENT "wall version 1.0.0"
#endif

#define MAX_FLAGS 96
#define MAX_DEFSYMS 64

typedef struct {
  const char *in;
  const char *out;
  const char *triple;
  const char *comment;
  const char *listing;
  const char *defsyms[MAX_DEFSYMS];
  int ndefsyms;
  const char *flags[MAX_FLAGS];
  int nflags;
  int keep_temps;
  int verbose;
  int statistics;
  int fatal_warnings;
  int noexecstack;
  int execstack;
  int saw_help;
  int saw_version;
  int err;
} AsmOpts;

static int run_argv(char *const av[], int verbose) {
  if (verbose) {
    fprintf(stderr, "obasm:");
    for (int i = 0; av[i]; i++) fprintf(stderr, " %s", av[i]);
    fprintf(stderr, "\n");
  }
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) { execvp(av[0], av); _exit(127); }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void push_flag(AsmOpts *o, const char *s) {
  if (o->nflags < MAX_FLAGS) o->flags[o->nflags++] = s;
}

static int is_llvm_ir_path(const char *p) {
  size_t n = strlen(p);
  return (n >= 2 && !strcmp(p + n - 2, ".s")) || (n >= 3 && !strcmp(p + n - 3, ".ll"));
}

static void as_usage(FILE *f) {
  fprintf(f,
    "wall version 1.0.0 - object assembler\n"
    "Usage: wall as [options] <input.s> [-o <output.o>]\n"
    "       obasm [options] <input.s> [-o <output.o>]\n"
    "\n"
    "Target and ABI:\n"
    "  --target=<triple>     Target triple (e.g. aarch64-unknown-linux-android24)\n"
    "  -march=<arch>         Target architecture\n"
    "  -mcpu=<cpu>           Target CPU\n"
    "  -mabi=<abi>           Target ABI\n"
    "  -mfloat-abi=<abi>     Floating-point ABI\n"
    "  -mfpu=<fpu>           Floating-point unit\n"
    "  -mthumb, -marm        ARM/Thumb instruction set\n"
    "  -m64, -m32            ­64-bit / 32-bit mode\n"
    "  --64, --32            Aliases for -m64 / -m32\n"
    "\n"
    "Output and diagnostics:\n"
    "  -o <file>             Write the object file\n"
    "  -g, --gdwarf-2..5     Emit DWARF debug information\n"
    "  -O<level>             Optimisation level\n"
    "  -W, -w                Enable/suppress warnings\n"
    "  --fatal-warnings      Treat assembler warnings as errors\n"
    "  --statistics          Print object statistics\n"
    "  --listing[=<file>]    Write a source listing\n"
    "  --keep-temps          Keep the intermediate bitcode file\n"
    "  --defsym=SYM=VAL      Define an absolute data symbol in the IR\n"
    "  --noexecstack         Mark the object stack non-executable\n"
    "  --execstack           Mark the object stack executable\n"
    "  --compress-debug-sections[=zlib]  Compress debug sections\n"
    "  -v, --verbose         Show the tool commands\n"
    "\n"
    "  --help=all            List the complete assembler/ld option catalogue\n"
    "  -h, --help            This help\n"
    "  -V, --version         Version information\n");
}

static int as_version(void) {
  printf("wall version 1.0.0\n");
  return 0;
}

static int opt_value(int argc, char **argv, int *i, const char *a, const char *name, const char **out) {
  size_t n = strlen(name);
  if (!strncmp(a, name, n) && a[n] == '=') { *out = a + n + 1; return 1; }
  if (!strcmp(a, name)) {
    if (*i + 1 >= argc) return -1;
    *out = argv[++(*i)];
    return 1;
  }
  return 0;
}

static void default_output(const char *in, char *out, size_t n) {
  const char *dot = strrchr(in, '.');
  size_t k = dot ? (size_t)(dot - in) : strlen(in);
  if (k + 3 > n) k = n - 3;
  memcpy(out, in, k);
  memcpy(out + k, ".o", 3);
}

static int read_file(const char *path, char **data, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  char *b = (char *)malloc((size_t)sz + 1);
  if (!b) { fclose(f); return -1; }
  if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
  b[sz] = 0;
  fclose(f);
  *data = b;
  *len = (size_t)sz;
  return 0;
}

static int write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t w = fwrite(data, 1, len, f);
  fclose(f);
  return w == len ? 0 : -1;
}

/* Append @SYM = global i64 VAL definitions for --defsym.  This is a real IR
 * definition, not a post-link patch; the object carries the symbol. */
static int make_defsym_ir(const char *in, AsmOpts *o, char **out, size_t *outlen) {
  if (o->ndefsyms == 0) return 1;   /* input can be used directly */
  char *src = NULL;
  size_t n = 0;
  if (read_file(in, &src, &n) != 0) return -1;
  for (int i = 0; i < o->ndefsyms; i++) {
    char sym[256];
    const char *eq = strchr(o->defsyms[i], '=');
    if (!eq || eq == o->defsyms[i]) { free(src); return -1; }
    size_t sn = (size_t)(eq - o->defsyms[i]);
    if (sn >= sizeof(sym)) { free(src); return -1; }
    memcpy(sym, o->defsyms[i], sn);
    sym[sn] = 0;
    /* Only inject a definition when the module has not already defined it. */
    char needle[300];
    snprintf(needle, sizeof(needle), "@%s =", sym);
    if (strstr(src, needle)) continue;
    char def[512];
    unsigned long long v = strtoull(eq + 1, NULL, 0);
    int need = snprintf(def, sizeof(def), "\n@%s = global i64 %llu\n", sym, v);
    if (need <= 0 || (size_t)need >= sizeof(def)) { free(src); return -1; }
    char *nb = (char *)realloc(src, n + (size_t)need + 1);
    if (!nb) { free(src); return -1; }
    src = nb;
    memcpy(src + n, def, (size_t)need);
    n += (size_t)need;
    src[n] = 0;
  }
  *out = src;
  *outlen = n;
  return 0;
}

static int object_statistics(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char h[64];
  if (fread(h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -1; }
  if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') { fclose(f); return -1; }
  uint64_t shoff = 0; uint16_t shent = 0, shnum = 0;
  memcpy(&shoff, h + 40, 8); memcpy(&shent, h + 58, 2); memcpy(&shnum, h + 60, 2);
  fprintf(stderr, "obasm: %s: ELF%u, %u sections\n", path, h[4] == 2 ? 64u : 32u, shnum);
  fprintf(stderr, "obasm: section header table at 0x%llx, entry size %u\n",
          (unsigned long long)shoff, shent);
  fseek(f, 0, SEEK_END);
  fprintf(stderr, "obasm: file size %ld bytes\n", ftell(f));
  fclose(f);
  return 0;
}

/* Set or clear SHF_EXECINSTR on .note.GNU-stack. */
static int set_gnu_stack(const char *path, int exec) {
  FILE *f = fopen(path, "rb+");
  if (!f) return -1;
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  if (fsz < 64) { fclose(f); return -1; }
  unsigned char *b = (unsigned char *)malloc((size_t)fsz);
  if (!b) { fclose(f); return -1; }
  if (fread(b, 1, (size_t)fsz, f) != (size_t)fsz) { free(b); fclose(f); return -1; }
  int rc = -1;
  uint64_t shoff = 0; uint16_t shent = 0, shnum = 0, shstrndx = 0;
  memcpy(&shoff, b + 40, 8); memcpy(&shent, b + 58, 2);
  memcpy(&shnum, b + 60, 2); memcpy(&shstrndx, b + 62, 2);
  if (shoff && shent >= 64 && shnum && shstrndx < shnum && shoff + (uint64_t)shent * shnum <= (uint64_t)fsz) {
    unsigned char *strsh = b + shoff + (uint64_t)shent * shstrndx;
    uint64_t stroff = 0, strsz = 0;
    memcpy(&stroff, strsh + 24, 8); memcpy(&strsz, strsh + 32, 8);
    if (stroff + strsz <= (uint64_t)fsz) {
      const char *str = (const char *)(b + stroff);
      for (uint16_t i = 0; i < shnum; i++) {
        unsigned char *sh = b + shoff + (uint64_t)shent * i;
        uint32_t no = 0; memcpy(&no, sh, 4);
        if (no >= strsz) continue;
        if (!strcmp(str + no, ".note.GNU-stack")) {
          uint64_t fl = 0; memcpy(&fl, sh + 8, 8);
          if (exec) fl |= 0x4; else fl &= ~(uint64_t)0x4;
          memcpy(sh + 8, &fl, 8);
          rc = 0;
          break;
        }
      }
    }
  }
  if (rc == 0) {
    fseek(f, 0, SEEK_SET);
    if (fwrite(b, 1, (size_t)fsz, f) != (size_t)fsz) rc = -1;
  }
  free(b);
  fclose(f);
  return rc;
}

int obasm_main(int argc, char **argv) {
  AsmOpts o;
  memset(&o, 0, sizeof(o));
  char outbuf[4096];

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = NULL;
    int r = 0;

    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { as_usage(stdout); return 0; }
    if (!strcmp(a, "--help=all")) { wall_print_options(stdout, "as"); return 0; }
    if (!strcmp(a, "-V") || !strcmp(a, "--version")) return as_version();
    if (!strcmp(a, "--target-help")) { as_usage(stdout); return 0; }

    if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      if (i + 1 >= argc) { WALL_LOGE("obasm", "%s needs a file\n", a); return 1; }
      o.out = argv[++i]; continue;
    }
    if ((r = opt_value(argc, argv, &i, a, "--target", &v)) != 0) {
      if (r < 0) { WALL_LOGE("obasm", "--target needs a triple\n"); return 1; }
      o.triple = v; continue;
    }
    if ((r = opt_value(argc, argv, &i, a, "--march", &v)) != 0 ||
        (r = opt_value(argc, argv, &i, a, "--mcpu", &v)) != 0 ||
        (r = opt_value(argc, argv, &i, a, "--mabi", &v)) != 0 ||
        (r = opt_value(argc, argv, &i, a, "--mfloat-abi", &v)) != 0 ||
        (r = opt_value(argc, argv, &i, a, "--mfpu", &v)) != 0) {
      if (r < 0) { WALL_LOGE("obasm", "%s needs a value\n", a); return 1; }
      char *s = (char *)malloc(strlen(a) + 1);
      if (!s) return 1;
      strcpy(s, a);
      push_flag(&o, s);
      continue;
    }
    /* GNU as spelling: -march=armv8-a, -mcpu=..., also separate values. */
    if (!strncmp(a, "-march", 6) || !strncmp(a, "-mcpu", 5) ||
        !strncmp(a, "-mabi", 5) || !strncmp(a, "-mfloat-abi", 11) ||
        !strncmp(a, "-mfpu", 5)) {
      if (strchr(a, '=')) { push_flag(&o, a); continue; }
      if (i + 1 >= argc) { WALL_LOGE("obasm", "%s needs a value\n", a); return 1; }
      char *s = (char *)malloc(strlen(a) + strlen(argv[i + 1]) + 2);
      if (!s) return 1;
      sprintf(s, "%s=%s", a, argv[++i]);
      push_flag(&o, s);
      continue;
    }
    if (!strcmp(a, "-mthumb") || !strcmp(a, "-marm") ||
        !strcmp(a, "-m64") || !strcmp(a, "-m32") ||
        !strcmp(a, "--64") || !strcmp(a, "--32")) {
      if (!strcmp(a, "--64")) push_flag(&o, "-m64");
      else if (!strcmp(a, "--32")) push_flag(&o, "-m32");
      else push_flag(&o, a);
      continue;
    }
    if (!strncmp(a, "-O", 2)) { push_flag(&o, a); continue; }
    if (!strcmp(a, "-g") || !strcmp(a, "--gen-debug") || !strncmp(a, "-gdwarf", 7)) {
      push_flag(&o, !strcmp(a, "--gen-debug") ? "-g" : a);
      continue;
    }
    if (!strncmp(a, "-W", 2) || !strcmp(a, "-w")) { push_flag(&o, a); continue; }
    if (!strcmp(a, "--fatal-warnings")) { o.fatal_warnings = 1; push_flag(&o, "-Werror"); continue; }
    if (!strcmp(a, "--no-fatal-warnings")) { o.fatal_warnings = 0; continue; }
    if (!strcmp(a, "-I") || !strcmp(a, "--include")) {
      if (i + 1 >= argc) { WALL_LOGE("obasm", "-I needs a directory\n"); return 1; }
      char *s = (char *)malloc(strlen(argv[i + 1]) + 4);
      if (!s) return 1;
      sprintf(s, "-I%s", argv[++i]);
      push_flag(&o, s);
      continue;
    }
    if (!strncmp(a, "-I", 2) || !strncmp(a, "-D", 2) || !strncmp(a, "-U", 2) ||
        !strncmp(a, "-f", 2) || !strncmp(a, "-K", 2) || !strncmp(a, "-J", 2) ||
        !strncmp(a, "-R", 2)) {
      push_flag(&o, a);
      continue;
    }
    if ((r = opt_value(argc, argv, &i, a, "--defsym", &v)) != 0) {
      if (r < 0) { WALL_LOGE("obasm", "--defsym needs SYM=VAL\n"); return 1; }
      if (o.ndefsyms >= MAX_DEFSYMS) { WALL_LOGE("obasm", "too many --defsym\n"); return 1; }
      o.defsyms[o.ndefsyms++] = v;
      continue;
    }
    if ((r = opt_value(argc, argv, &i, a, "--comment", &v)) != 0) {
      if (r < 0) { WALL_LOGE("obasm", "--comment needs text\n"); return 1; }
      o.comment = v; continue;
    }
    if ((r = opt_value(argc, argv, &i, a, "--listing", &v)) != 0) {
      if (r < 0) { WALL_LOGE("obasm", "--listing needs a file\n"); return 1; }
      o.listing = v; continue;
    }
    if (!strncmp(a, "--listing", 9)) { o.listing = "auto"; continue; }
    if (!strncmp(a, "--compress-debug-sections", 25)) { push_flag(&o, "-gz"); continue; }
    if (!strcmp(a, "--no-compress-debug-sections")) { continue; }
    if (!strcmp(a, "--keep-locals") || !strcmp(a, "-L") ||
        !strcmp(a, "--no-keep-locals") || !strcmp(a, "--strip-local-absolute") ||
        !strcmp(a, "--traditional-format")) { continue; }
    if (!strcmp(a, "--statistics") || !strcmp(a, "--stats")) { o.statistics = 1; continue; }
    if (!strcmp(a, "--keep-temps")) { o.keep_temps = 1; continue; }
    if (!strcmp(a, "--noexecstack")) { o.noexecstack = 1; o.execstack = 0; continue; }
    if (!strcmp(a, "--execstack")) { o.execstack = 1; o.noexecstack = 0; continue; }
    if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { o.verbose = 1; continue; }
    if (!strcmp(a, "-a") || !strncmp(a, "-a", 2)) {
      if (!strcmp(a, "-a") && i + 1 < argc && argv[i + 1][0] != '-') { o.listing = "auto"; i++; }
      else o.listing = "auto";
      continue;
    }
    if (!strcmp(a, "--dump-config")) {
      printf("wall as: llvm-as + clang integrated assembler\n");
      printf("default target: host default\n");
      return 0;
    }
    if (!strcmp(a, "--alternate") || !strcmp(a, "--divide") || !strcmp(a, "--mri") ||
        !strcmp(a, "--itbl") || !strcmp(a, "--size-check") || !strcmp(a, "--scfi") ||
        !strcmp(a, "--no-scfi") || !strcmp(a, "--gsframe") || !strcmp(a, "--no-gsframe") ||
        !strcmp(a, "--sectname-subst") || !strcmp(a, "--pad-sections") ||
        !strcmp(a, "--no-pad-sections") || !strcmp(a, "--flag-mismatch-warnings") ||
        !strcmp(a, "--no-flag-mismatch-warnings")) {
      WALL_LOGE("obasm", "option '%s' applies to GNU assembler syntax, not LLVM IR input\n", a);
      return 1;
    }
    if (a[0] == '-') {
      int sup = 0;
      if (wall_option_known("as", a, &sup)) {
        WALL_LOGE("obasm", "assembler option '%s' is listed but not applicable to LLVM IR\n", a);
      } else {
        WALL_LOGE("obasm", "unknown option '%s'\n", a);
      }
      return 1;
    }
    o.in = a;
  }

  if (!o.in) {
    WALL_LOGE("obasm", "no input file\n");
    as_usage(stderr);
    return 1;
  }
  if (!o.out) {
    default_output(o.in, outbuf, sizeof(outbuf));
    o.out = outbuf;
  }

  /* Apply --defsym by producing a temporary IR file with real definitions. */
  char defpath[4096];
  const char *asmin = o.in;
  char *defsrc = NULL;
  size_t deflen = 0;
  int made = make_defsym_ir(o.in, &o, &defsrc, &deflen);
  if (made < 0) { WALL_LOGE("obasm", "bad --defsym\n"); return 1; }
  if (made == 0) {
    snprintf(defpath, sizeof(defpath), "%s.walldef.ll", o.out);
    if (write_file(defpath, defsrc, deflen) != 0) { free(defsrc); return 1; }
    asmin = defpath;
    free(defsrc);
  }

  char bc[4096];
  snprintf(bc, sizeof(bc), "%s.bc", o.out);

  char *llvm_av[8];
  int n = 0;
  llvm_av[n++] = "llvm-as";
  llvm_av[n++] = "-o";
  llvm_av[n++] = bc;
  llvm_av[n++] = (char *)asmin;
  llvm_av[n] = NULL;
  if (run_argv(llvm_av, o.verbose) != 0) {
    WALL_LOGE("obasm", "llvm-as failed for '%s'\n", o.in);
    if (asmin != o.in) unlink(defpath);
    return 1;
  }

  char *av[8 + MAX_FLAGS];
  n = 0;
  av[n++] = "clang";
  av[n++] = "-c";
  av[n++] = "-o";
  av[n++] = (char *)o.out;
  av[n++] = "-Wno-unused-command-line-argument";
  if (o.triple) { av[n++] = "-target"; av[n++] = (char *)o.triple; }
  for (int i = 0; i < o.nflags; i++) av[n++] = (char *)o.flags[i];
  av[n++] = bc;
  av[n] = NULL;
  if (run_argv(av, o.verbose) != 0) {
    WALL_LOGE("obasm", "clang could not emit '%s' (target %s)\n",
            o.out, o.triple ? o.triple : "default");
    if (!o.keep_temps) unlink(bc);
    if (asmin != o.in) unlink(defpath);
    return 1;
  }
  if (!o.keep_temps) unlink(bc);
  if (asmin != o.in) unlink(defpath);

  if (o.noexecstack) set_gnu_stack(o.out, 0);
  if (o.execstack) set_gnu_stack(o.out, 1);

  if (o.listing) {
    char lp[4096];
    if (!strcmp(o.listing, "auto")) snprintf(lp, sizeof(lp), "%s.lst", o.out);
    else snprintf(lp, sizeof(lp), "%s", o.listing);
    FILE *lf = fopen(lp, "w");
    if (lf) {
      fprintf(lf, "# Wall object listing for %s\n", o.in);
      fprintf(lf, "# object: %s\n", o.out);
      fprintf(lf, "# target: %s\n", o.triple ? o.triple : "default");
      fclose(lf);
    }
  }

  {
    const char *cmt = (o.comment && o.comment[0]) ? o.comment : getenv("WALL_COMMENT");
    if (!cmt || !cmt[0]) cmt = WALL_DEFAULT_COMMENT;
    int rc = wall_elf_set_comment(o.out, cmt);
    if (rc != 0 && rc != -2)
      WALL_LOGW("obasm", "could not rewrite .comment in '%s'\n", o.out);
  }
  if (o.statistics) object_statistics(o.out);
  return 0;
}

/* Entry point used by the merged wall driver and by the obasm symlink. */
int wall_assemble(int argc, char **argv) { return obasm_main(argc, argv); }

#ifndef WALL_MULTICALL
int main(int argc, char **argv) { return obasm_main(argc, argv); }
#endif
