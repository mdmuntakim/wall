/* wall_cli.h - shared command-line option knowledge for the Wall toolchain.
 *
 * The merged `wall` binary is a multicall compiler driver, assembler and
 * linker.  This table documents the GNU as / GNU ld option surface it accepts,
 * and records whether an option is implemented, accepted for compatibility, or
 * deliberately rejected.  Keeping the table in one place means `wall --help=all`
 * and the individual tool front ends cannot drift apart.
 */
#ifndef WALL_CLI_H
#define WALL_CLI_H

#include <stdio.h>

enum {
  WALL_OPT_SUPPORTED = 1,
  WALL_OPT_COMPAT = 0,
  WALL_OPT_REJECTED = -1
};

typedef struct {
  const char *long_name;  /* without leading "--" */
  char short_name;        /* 0 when there is no short form */
  int has_arg;            /* 0 none, 1 required, 2 optional */
  const char *arg_name;
  const char *tool;       /* "ld", "as" or "both" */
  const char *desc;
  int support;
} WallOption;

extern const WallOption wall_options[];
extern const int wall_option_count;

/* Print options for one tool ("ld", "as") or every tool ("all"). */
void wall_print_options(FILE *f, const char *tool);

/* Find an option by its command-line spelling, e.g. "--build-id" or "-O2".
 * Returns the table index, or -1.  Parses "--name=value" and "-O2" forms. */
int wall_option_lookup(const char *tool, const char *arg);

/* True when the option is known for the tool; support is returned through
 * support_out (optional). */
int wall_option_known(const char *tool, const char *arg, int *support_out);

#endif
