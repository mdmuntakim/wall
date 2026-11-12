/* wall_log.h - professional, LLVM-style coloured diagnostics.
 *
 * Colour is used only when the destination is a terminal and the user has not
 * asked for plain output (NO_COLOR).  CLICOLOR_FORCE=1 overrides detection,
 * which is useful in CI logs that understand ANSI escape sequences.
 *
 *   tool: error: ...      bold red
 *   tool: warning: ...    bold magenta
 *   tool: note: ...       bold cyan
 */
#ifndef WALL_LOG_H
#define WALL_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

enum wall_log_level {
  WALL_LOG_ERROR = 0,
  WALL_LOG_WARNING = 1,
  WALL_LOG_NOTE = 2,
  WALL_LOG_REMARK = 3
};

static int wall_log_color_enabled(FILE *f) {
  static int cached = -1;
  if (cached >= 0) return cached;
  const char *no = getenv("NO_COLOR");
  const char *force = getenv("CLICOLOR_FORCE");
  if (no && no[0]) cached = 0;
  else if (force && force[0] && strcmp(force, "0")) cached = 1;
  else cached = isatty(fileno(f)) ? 1 : 0;
  return cached;
}

static void wall_log_vimpl(const char *tool, int level, const char *fmt, va_list ap) {
  FILE *f = stderr;
  const char *label = "remark";
  const char *colour = "\033[1;34m";
  if (level == WALL_LOG_ERROR) { label = "error"; colour = "\033[1;31m"; }
  else if (level == WALL_LOG_WARNING) { label = "warning"; colour = "\033[1;35m"; }
  else if (level == WALL_LOG_NOTE) { label = "note"; colour = "\033[1;36m"; }

  if (wall_log_color_enabled(f))
    fprintf(f, "\033[1m%s:\033[0m %s%s:\033[0m ", tool, colour, label);
  else
    fprintf(f, "%s: %s: ", tool, label);
  vfprintf(f, fmt, ap);
}

static void wall_log_impl(const char *tool, int level, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  wall_log_vimpl(tool, level, fmt, ap);
  va_end(ap);
}

#define WALL_LOGE(tool, ...) wall_log_impl(tool, WALL_LOG_ERROR, __VA_ARGS__)
#define WALL_LOGW(tool, ...) wall_log_impl(tool, WALL_LOG_WARNING, __VA_ARGS__)
#define WALL_LOGN(tool, ...) wall_log_impl(tool, WALL_LOG_NOTE, __VA_ARGS__)
#define WALL_LOGR(tool, ...) wall_log_impl(tool, WALL_LOG_REMARK, __VA_ARGS__)

#endif /* WALL_LOG_H */
