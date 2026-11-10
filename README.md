# Wall

**Wall** is a modern, professional systems programming language that compiles to
LLVM IR — built around a pluggable LLVM backend (`libwall.so`) driven by a fast
`wall` compiler driver.

```
wall hello.wa          # compile Wall source -> tmp_<pid>_<n>.s
obasm tmp_1234_0.s     # assemble LLVM .s -> .o
clang tmp_1234_0.o -o hello   # link -> native binary
```

## Quick start

```sh
./wall hello.wa          # emits tmp_<pid>_<n>.s (LLVM IR / textual assembly)
./obasm tmp_1234_0.s -o hello.o
clang tmp_1234_0.o -o hello
./hello                   # run
```

## Installation

Release tarballs (each contains `wall` driver + `libwall.so` backend):

- `wall-aarch64.tar.xz`
- `wall-armeabi-v7a.tar.xz`
- `wall-x86.tar.xz`
- `wall-x86_64.tar.xz`

## Usage

```
wall [options] <input file>
```

- `-h, --help`            print help
- `-v, --version`         print version
- `-o <filename>`          set output filename (default: `tmp_<pid>_<n>.s`)
- `-S, --emit-llvm`        emit LLVM IR (assembly) to the output filename
- `-O <level>`             optimization level (0,1,2,3,s,S,z)
- `--backend=<path>`       backend plugin shared object (default: `libwall.so`)
- `-B <dir>`               backend search directory
- `-Wall`, `-Wextra`, `-Werror`

The backend shared object exports:

```
int wall_version(char *buf, int buflen);
int wall_compile(const char *source, int optimize_level,
                 char **out_ir, char **err);
```

`obasm` assembles LLVM textual assembly into an object file:

```
obasm <input.s> [-o <output.o>] [--target=<triple>]
```

## Building from source

```sh
./build.sh          # produces wall, libwall.so, obasm (aarch64-linux-android)
```

## Architecture

- `stage0/` — bootstrap compiler implementation (driver + LLVM backend)
- `stage1/` — self-hosted Wall compiler sources (front-end / back-end in Wall)
- `src/`, `lib/`, `include/` — compiler source layout

## Tests

See `tests/`.
