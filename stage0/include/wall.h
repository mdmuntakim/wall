#ifndef WALL_H
#define WALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Wall Compiler version string. */
const char *wall_version_string(void);

/* Wall version info -> fills buf (e.g. "Wall Compiler version 1.0.0").
 * Returns 0 on success, -1 on failure. */
int wall_version(char *buf, int buflen);

/* Wall compiler backend entry point.
 *
 * wall_compile() takes a wall source string, an optimization level
 * (0=-O0, 1=-O1, 2=-O2, 3=-O3, 4=-Os, 5=-Oz) and produces the LLVM IR
 * text for the translated module.
 *
 * On success returns 0, sets *out_ir to a freshly malloc'd NUL-terminated
 * LLVM IR string (caller frees) and leaves *err NULL.
 *
 * On failure returns -1, leaves *out_ir NULL and sets *err to a human
 * readable error message (e.g. "3:5: error: unexpected token ';'").
 */
int wall_compile(const char *source, int optimize_level,
                 char **out_ir, char **err);

#ifdef __cplusplus
}
#endif

#endif /* WALL_H */
