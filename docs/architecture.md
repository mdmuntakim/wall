# Architecture

Wall is organised as a multicall driver plus one shared library. The driver is
deliberately small: it owns process-level concerns, while the compiler,
assembler, and linker are backend entry points in `libwall.so`.

```text
                    +----------------------+
  argv[0] ─────────▶| wall (driver)        |
                    |  argument parsing    |
                    |  include expansion   |
                    |  temp-file handling  |
                    |  dlopen("libwall.so")|
                    +----------+-----------+
                               │
              +----------------+----------------+
              │                │                │
       wall_compile      wall_assemble      wall_link
              │                │                │
       LLVM IR emitter    LLVM assembler    ELF linker
              │                │                │
              ▼                ▼                ▼
          .ll / .s           .o              executable
```

## Components

| Path | Responsibility |
| --- | --- |
| `src/driver/wall.c` | Multicall command dispatch, include expansion, temporary files, backend loading, process execution |
| `src/compiler/libwall.c` | Lexer, parser, semantic checks, LLVM IR generation, version/comment identity |
| `src/assembler/obasm.c` | Validates IR input, invokes the LLVM assembler, owns `.comment` metadata |
| `src/linker/linker.c` | Parses AArch64 ELF relocatable objects, resolves symbols and archives, applies relocations, writes a static ELF executable or shared object |
| `src/common/wall_cli.c` | Machine-readable catalogue of accepted driver, assembler, and linker options |
| `src/common/wall_log.h` | LLVM-style coloured diagnostics |
| `runtime/` | Freestanding runtime (`wallrt.c`) and `_start` (`start.S`) compiled into every executable |
| `stage1/` | Self-hosted implementation written in Wall |

## Compilation pipeline

1. The driver reads the top-level `.wa` file and recursively expands textual
   `include` directives.
2. `wall_compile()` lexes and parses the translation unit and emits textual
   LLVM IR.
3. `wall_assemble()` writes the IR to a temporary file and invokes the LLVM
   integrated assembler through `clang` to produce an AArch64 relocatable
   object.
4. `wall_link()` parses the object, merges its allocatable sections, embeds the
   freestanding runtime, resolves symbols, applies AArch64 relocations, and
   writes the final ELF image.

The linker never calls `exec()`, `system()`, or `popen()`, and the generated
program has no dynamic loader or C library dependency.

## Runtime

`runtime/wallrt.c` implements the small set of primitives used by generated
programs (`printf`, string and memory helpers, process exit) over raw Linux
system calls. `runtime/start.S` provides `_start`. Both are assembled once and
embedded into the linker as byte arrays in `src/linker/runtime_blob.h`, so a
single `wall` binary can link programs in any working directory.

## Backend ABI

The public ABI is declared in `include/wall/wall.h`:

```c
const char *wall_version_string(void);
int wall_version(char *buf, int buflen);
int wall_compile(const char *source, int optimize_level,
                 char **out_ir, char **err);
```

The merged library additionally exports the multicall entry points used by the
driver:

```c
int wall_assemble(int argc, char **argv);
int wall_link(int argc, char **argv);
```

The driver searches for the backend in this order:

1. `$WALL_BACKEND`
2. `--backend=<path>`
3. `<dir>/libwall.so` for each `-B <dir>`
4. the directory containing the running `wall` executable
5. the platform loader search path

## Static ELF output

`wall_link()` emits an ELF64 little-endian AArch64 image. The default layout
places `.text` near the start of the first page, followed by read-only data,
writable data, BSS, and the GOT. Program headers describe the loadable
segments and a non-executable stack. `--build-id` adds a `PT_NOTE` segment with
a SHA-1 build identifier derived from the linked image.

The linker supports the AArch64 relocation subset emitted by LLVM for the Wall
language: absolute, branch, ADR/ADRP, low-12, MOVW, and GOT-based relocations.
Unsupported relocation types are reported as errors rather than emitted
silently.

## Identity

The build computes a SHA-256 hash over the source list in `scripts/build.sh`.
The resulting string, `wall version <version> (<hash>)`, is compiled into the
driver/library and written into the ELF `.comment` section by the linker's own
section editor. No host compiler identification is left in the artefact.
