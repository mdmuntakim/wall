# Contributing

## Before you start

- Read [docs/architecture.md](docs/architecture.md) and
  [docs/language.md](docs/language.md).
- Check existing issues before opening a new one.
- Keep changes focused; avoid unrelated reformatting.

## Development workflow

```sh
make
make test
```

For a clean rebuild:

```sh
make clean
make
```

## Code style

- C sources follow `.clang-format` (LLVM style, two-space indent).
- Shell scripts use two-space indentation and `set -euo pipefail`.
- Public headers stay compatible with the append-only ABI policy in
  `include/wall/wall.h`.
- Do not add silent no-ops. An option either works or fails with a diagnostic.

## Tests

- Add a fixture under `tests/language/` and a check in `tests/run.sh` for new
  language behaviour.
- Add a negative test for every new diagnostic.
- Tests must pass without root privileges and must write only under
  `tests/tmp/`.

## Diagnostics

Use the shared logger in `src/common/wall_log.h`:

```c
WALL_LOGE("linker", "cannot open '%s': %s\n", path, strerror(errno));
WALL_LOGW("obasm", "option '%s' has no effect\n", flag);
```

Do not print ANSI escapes directly; the logger handles terminal detection and
`NO_COLOR`.

## Commit messages

Use a short imperative subject, for example:

```text
linker: reject unimplemented --sort-section
driver: resolve includes relative to the source file
tests: cover variadic extern functions
```

## Reporting bugs

Include:

- the `wall --version` output;
- host OS, architecture, and compiler versions;
- the command line and a minimal reproducing `.wa` file;
- the complete diagnostic output.
