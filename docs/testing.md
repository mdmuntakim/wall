# Testing

## Running the suite

```sh
make test
```

`make test` builds the toolchain and runs `tests/run.sh`. The suite creates all
temporary files under `tests/tmp/`, which is ignored by Git and removed by
`make clean`.

## Coverage

The integration suite verifies:

- version identity for `wall`, `wallc`, `obasm`, and `linker`;
- removal of example blocks from the concise help;
- availability of the complete option catalogue;
- explicit failure for unknown options;
- LLVM IR emission and target triple;
- object emission with a valid ELF header;
- assembly through `obasm` and linking through `linker`;
- `--print-output-format`;
- end-to-end execution when the host can run AArch64 binaries;
- textual include resolution;
- external global resolution across objects;
- static output hygiene: no dynamic library dependencies, no ELF interpreter,
  retains the Wall strings, symbol table, and string table; the final executable
  is not stripped or hidden by default.

When the build target cannot execute on the host (for example an AArch64
cross-build on x86_64 CI), execution checks print `skip` and the structural
checks still must pass.

## Adding a test

1. Add a small `.wa` fixture under `tests/language/`.
2. Add a `check`/`skip_test` pair to `tests/run.sh`.
3. Keep fixtures self-contained; use `tests/tmp/` for outputs.
4. Always test both the success path and an explicit failure path for new
   diagnostics.

## Manual checks

```sh
./wall tests/language/hello.wa -o tests/tmp/hello
./tests/tmp/hello

./wall -S tests/language/hello.wa -o tests/tmp/hello.ll
./obasm tests/tmp/hello.ll -o tests/tmp/hello.o
./linker tests/tmp/hello.o -o tests/tmp/hello-linked
./tests/tmp/hello-linked
```

For option-catalogue work:

```sh
./wall --help=all
./linker --list-relocs
./linker --print-output-format
```
