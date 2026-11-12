/* wall.h - Wall Compiler public ABI.
 *
 * ---------------------------------------------------------------------------
 * Overview
 * ---------------------------------------------------------------------------
 * The Wall compiler is split into two independent pieces:
 *
 *   1. The *driver* (`wall`) - a thin command line program that parses
 *      arguments, loads a backend shared library, reads the source file and
 *      writes the generated textual IR to the output file.
 *
 *   2. The *backend* (`libwall.so`) - a plugin that exposes the two C ABI
 *      entry points declared below.  The driver locates the backend at run
 *      time (via WALL_BACKEND, --backend=, -B, the executable directory, or
 *      the default library search path) and resolves the symbols with
 *      dlopen()/dlsym().
 *
 * Because the interface is a small, stable, versioned C ABI, alternate
 * backends can be dropped in without recompiling the driver.  For example a
 * backend could target a different IR dialect, emit a byte-code format, or be
 * an entirely different implementation written in another language, as long as
 * it exports the entry points declared here.
 *
 * ---------------------------------------------------------------------------
 * Version history
 * ---------------------------------------------------------------------------
 *   1.0.0  Initial public ABI.  world: wall_compile, wall_version.
 *
 * ---------------------------------------------------------------------------
 * Thread safety
 * ---------------------------------------------------------------------------
 * wall_compile() is re-entrant: each call keeps all of its state on the stack
 * and never touches globals.  Two threads may therefore call wall_compile()
 * concurrently with different source buffers.  wall_version() is also safe to
 * call from any thread.
 *
 * ---------------------------------------------------------------------------
 * Ownership
 * ---------------------------------------------------------------------------
 * The driver owns the input `source` buffer and keeps it alive for the
 * duration of the wall_compile() call.  The backend owns the buffers it hands
 * back through *out_ir and *err; the caller is responsible for freeing them
 * with free().  A backend is free to allocate these with malloc()/realloc()
 * as long as the result can be released with free().
 */

#ifndef WALL_H
#define WALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Compiler identification
 * ===========================================================================
 */

/* The canonical compiler name. */
#define WALL_COMPILER_NAME "wall"

/* The compiler version, as integers and as a string. */
#define WALL_VERSION_MAJOR 1
#define WALL_VERSION_MINOR 0
#define WALL_VERSION_PATCH 0
#define WALL_VERSION_STRING "wall version 1.0.0"

/* The source language revision understood by this ABI. */
#define WALL_LANGUAGE_STD "wall21"

/* The default target triple used when none is supplied. */
#define WALL_DEFAULT_TRIPLE "aarch64-unknown-linux-android"

/* The name of the default backend shared library. */
#define WALL_BACKEND_NAME "libwall.so"

/* The ABI revision.  Backends may reject drivers with a different revision. */
#define WALL_ABI_REVISION 1

/* ===========================================================================
 * Optimization levels
 * ===========================================================================
 * The driver maps its -O flag onto one of these values:
 *
 *   -O0            -> WALL_OPT_NONE
 *   -O1            -> WALL_OPT_LESS
 *   -O2            -> WALL_OPT_DEFAULT
 *   -O3            -> WALL_OPT_AGGRESSIVE
 *   -Os            -> WALL_OPT_SIZE
 *   -Oz            -> WALL_OPT_SIZE_AGGRESSIVE
 */
enum wall_opt_level {
  WALL_OPT_NONE = 0,
  WALL_OPT_LESS = 1,
  WALL_OPT_DEFAULT = 2,
  WALL_OPT_AGGRESSIVE = 3,
  WALL_OPT_SIZE = 4,
  WALL_OPT_SIZE_AGGRESSIVE = 5
};

/* ===========================================================================
 * Error codes
 * ===========================================================================
 * wall_compile() historically returns 0 on success and non-zero on failure.
 * Backends are encouraged to return one of the symbolic codes below so the
 * driver can print a friendly "category" prefix where appropriate.
 */
enum wall_status {
  WALL_OK = 0,
  WALL_ERR_IO = 1,
  WALL_ERR_LEX = 2,
  WALL_ERR_PARSE = 3,
  WALL_ERR_SEMANTIC = 4,
  WALL_ERR_CODEGEN = 5,
  WALL_ERR_OOM = 6,
  WALL_ERR_INTERNAL = 7,
  WALL_ERR_UNSUPPORTED = 8
};

/* ===========================================================================
 * Public entry points
 * ===========================================================================
 */

/* wall_version_string:
 *   Returns a pointer to a static, NUL-terminated version string.  The string
 *   is owned by the backend and must not be freed.  This symbol is optional;
 *   the driver falls back to wall_version() when it is absent.
 */
const char *wall_version_string(void);

/* wall_version:
 *   Copy a human readable version line ("wall version 1.0.0") into `buf`.
 *   At most `buflen` bytes (including the trailing NUL) are written.  Returns
 *   0 on success, -1 when `buf` is NULL or `buflen` is too small.
 */
int wall_version(char *buf, int buflen);

/* wall_compile:
 *   Compile `source` (a NUL-terminated Wall translation unit) and return the
 *   generated LLVM IR text.
 *
 *   Parameters:
 *     source          NUL-terminated Wall source buffer.
 *     optimize_level  One of enum wall_opt_level.
 *     out_ir          Out-parameter receiving a malloc'ed IR string.
 *     err             Out-parameter receiving a malloc'ed error message.
 *
 *   Returns:
 *     0    success, *out_ir is set and *err is NULL.
 *     <0   failure, *out_ir is NULL and *err holds a diagnostic of the form
 *          "<line>:<column>: error: <message>".
 *
 *   The caller takes ownership of both output buffers and must free() them.
 */
int wall_compile(const char *source, int optimize_level,
                 char **out_ir, char **err);

/* ===========================================================================
 * Token kinds - shared between the lexer and the parser.
 * ===========================================================================
 * The values are part of the backend's internal contract and are documented
 * here so that alternate backends can reuse the numbering.
 */
enum wall_token_kind {
  TOK_EOF = 0,
  TOK_IDENT = 1,
  TOK_INT = 2,
  TOK_FLOAT = 3,
  TOK_STRING = 4,
  TOK_CHAR = 5,

  /* keywords */
  TOK_FN = 10,
  TOK_LET = 11,
  TOK_MUT = 12,
  TOK_CONST = 13,
  TOK_GLOBAL = 14,
  TOK_IF = 15,
  TOK_ELSE = 16,
  TOK_WHILE = 17,
  TOK_FOR = 18,
  TOK_RETURN = 19,
  TOK_BREAK = 20,
  TOK_CONTINUE = 21,
  TOK_STRUCT = 22,
  TOK_ENUM = 23,
  TOK_TYPE = 24,
  TOK_IMPORT = 25,
  TOK_EXPORT = 26,
  TOK_EXTERN = 27,
  TOK_TRUE = 28,
  TOK_FALSE = 29,
  TOK_NULL = 30,

  /* primitive type keywords */
  TOK_I32 = 40,
  TOK_I64 = 41,
  TOK_U32 = 42,
  TOK_U64 = 43,
  TOK_F32 = 44,
  TOK_F64 = 45,
  TOK_FLOAT_KW = 46,
  TOK_DOUBLE_KW = 47,
  TOK_BOOL = 48,
  TOK_STRING_KW = 49,
  TOK_VOID = 50,

  /* punctuation */
  TOK_LPAREN = 60,
  TOK_RPAREN = 61,
  TOK_LBRACE = 62,
  TOK_RBRACE = 63,
  TOK_LBRACKET = 64,
  TOK_RBRACKET = 65,
  TOK_COMMA = 66,
  TOK_SEMI = 67,
  TOK_COLON = 68,
  TOK_ARROW = 69,
  TOK_DOT = 70,
  TOK_QUESTION = 71,

  /* operators */
  TOK_PLUS = 80,
  TOK_MINUS = 81,
  TOK_STAR = 82,
  TOK_SLASH = 83,
  TOK_PERCENT = 84,
  TOK_AMP = 85,
  TOK_PIPE = 86,
  TOK_CARET = 87,
  TOK_TILDE = 88,
  TOK_BANG = 89,
  TOK_ASSIGN = 90,
  TOK_EQ = 91,
  TOK_NE = 92,
  TOK_LT = 93,
  TOK_LE = 94,
  TOK_GT = 95,
  TOK_GE = 96,
  TOK_ANDAND = 97,
  TOK_OROR = 98,
  TOK_SHL = 99,
  TOK_SHR = 100,
  TOK_PLUSEQ = 101,
  TOK_MINUSEQ = 102,
  TOK_STAREQ = 103,
  TOK_SLASHEQ = 104,

  TOK_ERROR = 255
};

/* ===========================================================================
 * Builtin types understood by the code generator.
 * =========================================================================== */
enum wall_type {
  TY_VOID = 0,
  TY_I32 = 1,
  TY_I64 = 2,
  TY_FLOAT = 3,
  TY_DOUBLE = 4,
  TY_BOOL = 5,
  TY_STRING = 6,
  TY_PTR = 7,
  TY_UNKNOWN = 255
};

/* ===========================================================================
 * AST node kinds.
 * ===========================================================================
 * A Wall program is represented as a small tree whose nodes are one of the
 * kinds below.  The tree is built by the parser and consumed by the code
 * generator.  A node carries an operator/kind tag, up to two children and an
 * optional literal payload (identifier name, integer value or string).
 */
enum wall_node_kind {
  WALL_ND_NONE = 0,
  WALL_ND_PROGRAM = 1,
  WALL_ND_FN = 2,
  WALL_ND_GLOBAL = 3,
  WALL_ND_LET = 4,
  WALL_ND_BLOCK = 5,
  WALL_ND_IF = 6,
  WALL_ND_WHILE = 7,
  WALL_ND_FOR = 8,
  WALL_ND_RETURN = 9,
  WALL_ND_BREAK = 10,
  WALL_ND_CONTINUE = 11,
  WALL_ND_NUM = 12,
  WALL_ND_FLOAT = 13,
  WALL_ND_STRING = 14,
  WALL_ND_IDENT = 15,
  WALL_ND_BIN = 16,
  WALL_ND_UNARY = 17,
  WALL_ND_CALL = 18,
  WALL_ND_ASSIGN = 19,
  WALL_ND_TYPE = 20,
  WALL_ND_PARAM = 21,
  WALL_ND_EMPTY = 22
};

/* ===========================================================================
 * Diagnostics
 * ===========================================================================
 * A diagnostic carries a severity, source position and a formatted message.
 * The driver prints diagnostics as:
 *
 *   wall: error: <line>:<column>: <message>
 */
typedef struct wall_diag {
  int severity;     /* 0 = note, 1 = warning, 2 = error */
  int line;         /* 1-based source line */
  int column;       /* 1-based source column */
  const char *file; /* source file name, may be NULL */
  char message[512];
} wall_diag;

/* ===========================================================================
 * Compile options
 * ===========================================================================
 * A richer option record for backends that want more than the ABI's
 * (source, optimize_level) pair.  Not required by wall_compile(); provided for
 * future use and for embedders that link the backend statically.
 */
typedef struct wall_options {
  int opt_level;          /* enum wall_opt_level */
  int warnings;           /* bitmask of enabled warning classes */
  int werror;             /* treat warnings as errors */
  const char *target;     /* target triple, may be NULL */
  const char *std;        /* language standard, may be NULL */
  const char *output;     /* requested output path, may be NULL */
} wall_options;

/* ===========================================================================
 * Backend metadata (optional extensions)
 * ===========================================================================
 * A backend may additionally export the following symbols.  The driver ignores
 * them when they are absent.
 */
typedef struct wall_backend_info {
  const char *name;       /* "wall" */
  const char *version;    /* "1.0.0" */
  int abi_revision;       /* WALL_ABI_REVISION */
  const char *target;     /* default triple */
} wall_backend_info;

/* int wall_backend_get_info(wall_backend_info *out); */
/* int wall_backend_set_options(const wall_options *opts); */

#ifdef __cplusplus
}
#endif

#endif /* WALL_H */

/* ===========================================================================
 * Appendix A - Example embedding
 * ===========================================================================
 *
 *   #include <dlfcn.h>
 *   #include "wall.h"
 *
 *   int compile(const char *src, char **ir) {
 *     void *h = dlopen("libwall.so", RTLD_NOW);
 *     if (!h) return -1;
 *     int (*fn)(const char *, int, char **, char **) =
 *         (int (*)(const char *, int, char **, char **))dlsym(h, "wall_compile");
 *     char *err = NULL;
 *     int rc = fn(src, WALL_OPT_NONE, ir, &err);
 *     if (rc != 0) { fprintf(stderr, "%s\n", err ? err : "error"); free(err); }
 *     dlclose(h);
 *     return rc;
 *   }
 */

/* ===========================================================================
 * Appendix B - ABI stability policy
 * ===========================================================================
 *
 * The ABI is append-only within a major revision:
 *
 *   - New enum members are appended, never renumbered.
 *   - New struct fields are appended at the end; callers size-initialise the
 *     struct and backends read only fields that fit.
 *   - New entry points are optional and detected with dlsym().
 *
 * A major revision may remove or renumber symbols, in which case the driver
 * refuses to load a backend whose WALL_ABI_REVISION differs.
 */

/* ===========================================================================
 * Appendix C - Tokenisation reference
 * ===========================================================================
 *
 * Whitespace: space, tab, CR, LF.
 * Line comments:   // ... LF
 * Block comments:  slash-star ... star-slash (nesting is not supported)
 *
 * Identifiers: [A-Za-z_][A-Za-z0-9_]*
 * Integers:    decimal, 0x hex, 0o octal, 0b binary, '_' separators allowed.
 * Floats:      digits '.' digits [e [+-] digits]
 * Strings:     "..." with escapes \\, \", \n, \r, \t, \0, \xHH
 * Chars:       '...' with the same escape set (single byte).
 *
 * Operators are maximal-munch: "==" beats "=", "&&" beats "&", and so on.
 */

/* ===========================================================================
 * Appendix D - Grammar reference (informative)
 * ===========================================================================
 *
 *   program     := item*
 *   item        := fnDecl | globalDecl
 *
 *   fnDecl      := 'fn' IDENT '(' paramList? ')' ('->' type)? block
 *   paramList   := param (',' param)*
 *   param       := IDENT ':' type
 *   globalDecl  := 'global' IDENT ':' type '=' expr ';'
 *
 *   block       := '{' stmt* '}'
 *   stmt        := letStmt | ifStmt | whileStmt | forStmt | returnStmt
 *                | breakStmt | continueStmt | exprStmt
 *   letStmt     := 'let' IDENT ':' type '=' expr ';'
 *   ifStmt      := 'if' '(' expr ')' block ('else' (block | ifStmt))?
 *   whileStmt   := 'while' '(' expr ')' block
 *   forStmt     := 'for' '(' stmt? ';' expr? ';' stmt? ')' block
 *   returnStmt  := 'return' expr? ';'
 *
 *   expr        := assignment
 *   assignment  := logicalOr ('=' assignment)?
 *   logicalOr   := logicalAnd ('||' logicalAnd)*
 *   logicalAnd  := equality ('&&' equality)*
 *   equality    := comparison (('==' | '!=') comparison)*
 *   comparison  := additive (('<' | '<=' | '>' | '>=') additive)*
 *   additive    := multiplicative (('+' | '-') multiplicative)*
 *   multiplicative := unary (('*' | '/' | '%') unary)*
 *   unary       := ('!' | '-') unary | primary
 *   primary     := INT | FLOAT | STRING | 'true' | 'false' | IDENT
 *                | IDENT '(' argList? ')' | '(' expr ')'
 */

/* ===========================================================================
 * Appendix E - Code generation notes
 * ===========================================================================
 *
 * The backend emits textual LLVM IR with opaque pointers:
 *
 *   ; ModuleID = 'wall'
 *   target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
 *   target triple = "aarch64-unknown-linux-android"
 *
 *   declare i32 @printf(ptr, ...)
 *
 *   define i32 @add(i32 %a, i32 %b) {
 *   entry:
 *     %0 = add i32 %a, %b
 *     ret i32 %0
 *   }
 *
 * Integer arithmetic uses `add`/`sub`/`mul`/`sdiv`/`srem`; comparisons produce
 * `i1` which is zero-extended to `i32` when used as a value.  Boolean
 * operators short-circuit with `br`/`phi` chains.  Each function has exactly
 * one `entry:` block in this revision; flow-sensitive constructs create extra
 * numbered blocks as needed.
 *
 * String literals are emitted as private global constants of type
 * `[N x i8]` (N includes the trailing NUL) and passed to runtime functions as
 * `ptr`.  Calls to `printf` use the C variadic ABI and rely on the target's
 * default calling convention.
 */

/* ===========================================================================
 * Appendix F - Driver behaviour
 * ===========================================================================
 *
 * Backend search order (first hit wins):
 *
 *   1. $WALL_BACKEND
 *   2. --backend=<path>
 *   3. <dir>/libwall.so for each -B <dir>
 *   4. <directory of the running wall executable>/libwall.so
 *   5. libwall.so on the platform loader search path
 *
 * Output naming: when -o is supplied it is used verbatim; otherwise the driver
 * writes a temporary file named tmp_<pid>_<n>.s where <pid> is the process id
 * and <n> is a per-process counter.  This keeps parallel builds from colliding
 * and avoids clobbering a permanent file the user did not ask for.
 *
 * Exit codes: 0 success, 1 compile error, 2 usage error (reserved).
 */

/* ===========================================================================
 * Appendix G - Porting guide
 * ===========================================================================
 *
 * To add a target:
 *
 *   1. Add the triple to scripts/build.sh cross-target table.
 *   2. Ensure the toolchain can link the compiler-rt builtins and libunwind
 *      for that triple (the driver and backend are freestanding enough to run
 *      without libc++).
 *   3. Add a CI matrix entry that builds and smoke-tests the tarball.
 *   4. Record the triple in README.md's support matrix.
 *
 * To add a language feature:
 *
 *   1. Extend the lexer keyword table in src/compiler/libwall.c.
 *   2. Add token kinds here and in the parser's precedence table.
 *   3. Add an AST node kind and a parser production.
 *   4. Emit IR from the code generator and add a test under tests/.
 *   5. Mirror the change in the stage1 sources so the self-hosted compiler keeps
 *      up with the bootstrap.
 */
