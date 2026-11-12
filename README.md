# Wall

Wall is a small systems programming language and a self-contained toolchain
that targets AArch64 Linux and Android. The compiler emits LLVM IR, and the
project ships its own assembler wrapper and static ELF linker, so a Wall
program can be built without a system linker.

The repository contains two implementations:

- **stage 0** — the bootstrap toolchain written in C. It is the implementation
  used by the current `wall` command.
- **stage 1** — the self-hosted Wall implementation. It is under active
  development and is not yet the default build path.

## Status

The stage 0 toolchain builds and runs end-to-end on AArch64:

```text
Wall source  ->  LLVM IR  ->  relocatable object  ->  static ELF executable
             wall -S          wall -c                  wall
```

The language and ABI are versioned as `wall21` / ABI revision 1.

## Quick start

Requirements: `clang`, `llvm-as`, `dpkg-deb` (for packaging), and a POSIX
build environment.

```sh
make
./wall tests/language/hello.wa -o hello
./hello
```

Expected output:

```text
Hello, wall!
30 200 -10
```

Compile and run in one step:

```sh
./wall run tests/language/hello.wa
```

## Driver

`wall` is a multicall program. The compatibility names `wallc`, `obasm`,
`linker`, and `ld.wall` are symlinks to the same executable and dispatch on
`argv[0]`.

```text
wall build (default)   compile, assemble, and link
wall compile | cc      compile to LLVM IR or an object
wall as | assemble     assemble LLVM IR to an object
wall link | ld         link objects into a static executable
wall run               build and execute a program
wall help              show help
wall version           show the version
```

Frequently used options:

```text
-o <file>              output path
-S                     emit LLVM IR only
-c                     emit a relocatable object
-I <dir>               source include search path
-L <dir>               linker library search path
-l <name>              link a static archive
-Wl,<option>           pass an option to the Wall linker
-Wa,<option>           pass an option to the assembler
--target=<triple>      target triple
-march=, -mcpu=        target architecture / CPU
-mabi=, -mfloat-abi=   ABI / floating-point ABI
-mfpu=, -mthumb        FPU / Thumb mode
-m64, -m32             word size selection
--keep-temps           keep intermediate files
-v, --verbose          print backend commands
```

Run `wall --help=all` for the complete assembler and linker option catalogue.
Diagnostics use LLVM-style colour on terminals; set `NO_COLOR=1` to disable it.

## Language

A minimal program:

```wall
fn main() -> i32 {
  printf("Hello, wall!\n");
  return 0;
}
```

The bootstrap language supports functions, scalar locals and globals, integer
and floating-point arithmetic, comparisons, boolean operators, `if`/`else`,
`while`, `for`, calls, string literals, and the LLVM-backed C ABI.

Source files can be composed with textual includes:

```wall
include "config.wa";
include <shared/types.wa>;
```

`include "..."` is resolved relative to the including file and then through
`-I` directories. `include <...>` searches the `-I` directories first.

External declarations bind Wall names to symbols supplied by another object:

```wall
extern fn puts(s: string) -> i32;
extern fn printf(fmt: string, ...) -> i32;
extern global errno: i32;
extern let argc: i32;
extern type opaque_handle;
```

See [docs/language.md](docs/language.md) for the grammar and current
limitations.

## Repository layout

```text
include/wall/        Public C ABI and language reference header
src/compiler/        Bootstrap compiler: lexer, parser, LLVM IR emitter
src/driver/          Multicall driver and textual include processor
src/assembler/       LLVM IR assembler wrapper
src/linker/          Static AArch64 ELF linker and embedded runtime blobs
src/common/          Option catalogue, diagnostics, ELF helpers
runtime/             Freestanding Wall runtime and process entry point
stage1/              Self-hosted Wall compiler sources
tests/               Language and integration tests
scripts/             Build, packaging, and source-generation scripts
packaging/debian/    Debian package metadata
docs/                Architecture, language, toolchain, and packaging notes
```

## Build

```sh
make                  # build wall, libwall.so, and symlinks
make test             # build and run the integration suite
make install          # install into $PREFIX
make package          # build wall_<version>_<arch>.deb
make clean
```

The lower-level interface is `scripts/build.sh`:

```sh
./build.sh             # same as make
./build.sh install
./build.sh package
./build.sh clean
```

Cross-compilation is controlled with environment variables:

```sh
TARGET=aarch64-unknown-linux-gnu \
SYSROOT=/usr/aarch64-linux-gnu \
./build.sh
```

The build writes a content-derived identity into every artefact and stamps the
ELF `.comment` section with `wall version <version> (<hash>)`.

## Installation layout

```text
$PREFIX/bin/wall
$PREFIX/bin/wallc      -> wall
$PREFIX/bin/obasm      -> wall
$PREFIX/bin/linker     -> wall
$PREFIX/bin/ld.wall    -> wall
$PREFIX/lib/libwall.so
```

The `.deb` uses the Termux prefix
`/data/data/com.termux/files/usr`.

## Tests

```sh
make test
```

The suite exercises `--version`, `--help`, IR emission, object emission,
linking, execution (when the host can run the target), and malformed input.
See [docs/testing.md](docs/testing.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Language](docs/language.md)
- [Toolchain](docs/toolchain.md)
- [Testing](docs/testing.md)
- [Packaging](docs/packaging.md)
- [Self-hosting](docs/self-hosting.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security reports should follow
[SECURITY.md](SECURITY.md).

## Licence

MIT. See [LICENSE](LICENSE).
