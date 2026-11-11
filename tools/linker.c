/* linker - Wall linker driver.
 *
 * Usage:
 *   linker <input.o...> [-o <output>] [-shared|-static|-pie|-r]
 *                      [-L <dir>]... [-l <lib>]... [--target=<triple>]
 *                      [-e <symbol>] [--direct] [-v] [-h] [-V]
 *
 * Links relocatable objects (.o) into a shared object (.so) or an ELF
 * executable.  The default back end is the C compiler driver (clang/cc);
 * --direct uses the system linker (ld) instead.
 *
 * This is the bootstrap implementation of stage1/tools/linker.wa: both accept
 * the same command line and produce the same kinds of output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(const char *cmd) {
  if (getenv("WALL_LINKER_VERBOSE")) fprintf(stderr, "linker: %s\n", cmd);
  return system(cmd) == 0 ? 0 : -1;
}

static void usage(FILE *f) {
  fprintf(f,
    "wallc version 1.0.0\n"
    "Usage: linker <input.o...> [-o <output>] [options]\n"
    "\n"
    "Links relocatable objects into a shared object or an ELF binary.\n"
    "\n"
    "Output modes:\n"
    "  (default)            Produce an ELF executable\n"
    "  -shared              Produce a shared object (.so)\n"
    "  -pie                 Produce a position independent executable\n"
    "  -static              Produce a static executable\n"
    "  -r, --relocatable    Merge objects into one relocatable object\n"
    "\n"
    "Options:\n"
    "  -o <filename>        Set output filename (default: a.out)\n"
    "  -L <dir>             Add a library search directory\n"
    "  -l <lib>             Link against library <lib>\n"
    "  -e, --entry <sym>    Set the entry symbol\n"
    "  --target=<triple>    Set the target triple\n"
    "  --direct             Link with ld directly (no cc driver)\n"
    "  -v, --verbose        Print each pipeline step\n"
    "  -V, --version        Print version information and exit\n"
    "  -h, --help           Print this help message and exit\n");
}

int main(int argc, char **argv) {
  const char *out = "a.out";
  const char *mode = "exe";
  const char *triple = NULL;
  const char *entry = NULL;
  int direct = 0;
  int verbose = 0;
  int ninputs = 0;
  char **inputs = (char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof(char *));
  char libs[4096] = "";
  char libdirs[4096] = "";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("wallc version 1.0.0\n"); return 0; }
    else if (!strcmp(a, "-o")) { if (i + 1 < argc) out = argv[++i]; else { fprintf(stderr, "linker: error: -o requires a filename\n"); return 1; } }
    else if (!strcmp(a, "-shared")) { mode = "shared"; }
    else if (!strcmp(a, "-pie")) { mode = "pie"; }
    else if (!strcmp(a, "-static")) { mode = "static"; }
    else if (!strcmp(a, "-r") || !strcmp(a, "--relocatable")) { mode = "reloc"; }
    else if (!strcmp(a, "-e") || !strcmp(a, "--entry")) { if (i + 1 < argc) entry = argv[++i]; else { fprintf(stderr, "linker: error: -e requires a symbol\n"); return 1; } }
    else if (!strcmp(a, "-L")) { if (i + 1 < argc) { strncat(libdirs, " -L ", sizeof(libdirs) - strlen(libdirs) - 1); strncat(libdirs, argv[++i], sizeof(libdirs) - strlen(libdirs) - 1); } else { fprintf(stderr, "linker: error: -L requires a directory\n"); return 1; } }
    else if (!strcmp(a, "-l")) { if (i + 1 < argc) { strncat(libs, " -l", sizeof(libs) - strlen(libs) - 1); strncat(libs, argv[++i], sizeof(libs) - strlen(libs) - 1); } else { fprintf(stderr, "linker: error: -l requires a library name\n"); return 1; } }
    else if (!strcmp(a, "--direct")) { direct = 1; }
    else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; }
    else if (!strncmp(a, "--target=", 9)) { triple = a + 9; }
    else if (a[0] == '-' && a[1]) { fprintf(stderr, "linker: error: unknown option '%s'\n", a); return 1; }
    else { inputs[ninputs++] = argv[i]; }
  }

  if (ninputs == 0) { fprintf(stderr, "linker: error: no input files\n"); return 1; }
  if (verbose) setenv("WALL_LINKER_VERBOSE", "1", 1);

  if (!strcmp(mode, "shared") && !strcmp(out, "a.out")) out = "libwall_out.so";

  char cmd[16384];
  snprintf(cmd, sizeof(cmd), "%s", direct ? "ld" : "cc");
  if (triple && triple[0]) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " --target=%s", triple);

  if (!strcmp(mode, "reloc")) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -r");
  else if (!strcmp(mode, "shared")) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -shared");
  else if (!strcmp(mode, "pie")) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -pie");
  else if (!strcmp(mode, "static")) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -static");

  if (entry && entry[0]) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -e %s", entry);
  snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), "%s%s", libdirs, libs);
  snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " -o %s", out);
  for (int i = 0; i < ninputs; i++) snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " %s", inputs[i]);

  if (run(cmd) != 0) {
    fprintf(stderr, "linker: error: link failed\n");
    return 1;
  }
  return 0;
}
