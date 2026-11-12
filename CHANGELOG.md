# Changelog

All notable changes to Wall are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Textual `include "..."` and `include <...>` with `-I` search paths.
- `extern fn` (including variadic declarations), `extern global`, `extern let`,
  and opaque `extern type`.
- Static archive support through `-L`, `-l`, and `--whole-archive`.
- GNU `ld` option handling for common static-link workflows, including
  `--defsym`, `--require-defined`, `--retain-symbols-file`,
  `--allow-multiple-definition`, `--warn-unresolved-symbols`, option-state
  push/pop, `.comment` stamping, build IDs, maps, and link statistics.
- LLVM-style coloured diagnostics with `NO_COLOR` and `CLICOLOR_FORCE`.
- Professional repository layout, Make targets, CI, documentation, and Debian
  packaging metadata.

### Changed

- Assembler and linker are merged into `libwall.so`; `wall` remains a thin
  multicall driver.
- Version output consistently reports `wall version ...`.
- Unknown and unimplemented options fail explicitly.

### Fixed

- Correct temporary-bitcode suffix so LLVM treats assembler input as IR.
- Released hidden `.comment` ownership to the Wall build identity.

## [1.0.0] - 2026-11-12

### Added

- Initial public ABI (`include/wall/wall.h`).
- Stage 0 compiler, LLVM IR backend, assembler wrapper, and static AArch64 ELF
  linker.
- Freestanding runtime and `_start` implementation.
- Termux `.deb` packaging.
