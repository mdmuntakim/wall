# Toolchain

## Multicall dispatch

`wall` inspects `argv[0]` and optionally its first subcommand:

| Invocation | Behaviour |
| --- | --- |
| `wall file.wa` | build and link |
| `wall run file.wa` | build and execute |
| `wall -S file.wa` | emit LLVM IR |
| `wall -c file.wa` | emit an object |
| `wall as file.ll` | assemble |
| `wall link file.o` | link |
| `wallc ...` | same as `wall ...` |
| `obasm ...` | same as `wall as ...` |
| `linker ...` | same as `wall link ...` |
| `ld.wall ...` | same as `wall link ...` |

All compatibility names are symlinks to a single executable and a single
`libwall.so`.

## Driver options

```text
-o <file>              output file
-S                     emit textual LLVM IR
-c                     emit a relocatable object
-O0..3, -Os, -Oz       optimisation level
-g                     request debug information (assembler/backend dependent)
-I <dir>               source include directory
-L <dir>               linker library directory
-l <name>              link lib<name>.a or an explicit archive path
-Wl,<option>           forward one option to the linker
-Wa,<option>           forward one option to the assembler
--target=<triple>      target triple
-march=, -mcpu=        architecture and CPU
-mabi=, -mfloat-abi=   ABI and floating-point ABI
-mfpu=, -mthumb        FPU selection and Thumb mode
-m64, -m32             word-size selection
-static                request a static executable
-shared                request a shared object where supported
--keep-temps           retain intermediate files
-v, --verbose          print each backend command
-h, --help             concise help
--help=all             complete assembler/linker option catalogue
-V, --version          version
```

## Assembler

`obasm` accepts LLVM textual IR (`.ll`) and LLVM textual assembly (`.s`). It
invokes the LLVM integrated assembler/backend through `clang`, so the target
triple and ABI flags are real:

```text
--target=<triple>
-march=<arch>
-mcpu=<cpu>
-mabi=<abi>
-mfloat-abi=<abi>
-mfpu=<fpu>
-mthumb / -marm
-m64 / -m32
```

Options that only make sense for GNU `as` syntax are rejected with an explicit
diagnostic instead of being accepted as no-ops. The assembler also supports
`--defsym SYM=VALUE`, `--comment TEXT`, `--listing FILE`, `-I DIR`,
`--keep-temps`, `-v`, and `--version`.

## Linker

The Wall linker is a self-contained static ELF linker. It does not invoke a
system linker and does not emit dynamic relocations, PLT/GOT stubs, linker
scripts, or PE/Mach-O output.

Supported families:

```text
-o FILE, -e SYMBOL, -r, -pie, --shared
-L DIR, -l NAME, --whole-archive, --no-whole-archive
-u SYMBOL, --undefined=SYMBOL, --defsym=SYM=VALUE
--require-defined=SYMBOL, -U SYMBOL, --allow-undefined
--entry=SYMBOL, --target=TRIPLE, -A ARCH, -b FORMAT, --oformat=FORMAT
--build-id[=sha1|none], --no-build-id
--comment=TEXT, --stamp-comment=TEXT, --get-comment
-M, --print-map, -Map FILE, --map=FILE
--print-memory-usage, --print-output-format, --stats, --trace
--fatal-warnings, --no-fatal-warnings, --no-warnings
--warn-unresolved-symbols, --no-warn-unresolved-symbols
--warn-common, --no-warn-common, --sort-common, --no-sort-common
--define-common, -d, --no-define-common
--allow-multiple-definition, -z defs, -z muldefs, -z noexecstack
--execstack, --noexecstack, --error-execstack, --warn-execstack
--retain-symbols-file FILE
--push-state, --pop-state
--strip, --strip-all, --symbols, --debug, --strip-debug
--no-runtime
```

Unsupported GNU `ld` options return a non-zero status and name the option. The
`--help=all` catalogue marks whether each entry is implemented, rejected, or
accepted as a compatibility hint. `--list-relocs[=all]` prints the relocation
reference table.

The output format is `elf64-aarch64` (also accepted as
`elf64-littleaarch64`). Requesting another format is an error.

## Diagnostics

Diagnostics use LLVM-style severity labels and colours:

```text
wall: error: <message>
linker: warning: <message>
obasm: note: <message>
```

Colour is enabled only when stderr is a terminal. Set `NO_COLOR=1` to force
plain output, or `CLICOLOR_FORCE=1` to force colour in captured logs.

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | success |
| 1 | compilation, assembly, or link failure |
| 2 | usage error (reserved for driver argument handling) |
