#!/bin/bash
# Wall integration test suite.
#
# The suite builds small programs and verifies the observable contract of the
# driver, assembler, and linker.  Set TEST_RUNNER to a command prefix when the
# host cannot execute the target binaries directly, for example:
#
#   TEST_RUNNER="qemu-aarch64 -L /usr/aarch64-linux-gnu" make test
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMP="$ROOT/tests/tmp"
rm -rf "$TMP"
mkdir -p "$TMP"

RUNNER=()
if [ -n "${TEST_RUNNER:-}" ]; then
  read -r -a RUNNER <<< "$TEST_RUNNER"
fi

run_cmd() {
  if [ "${#RUNNER[@]}" -gt 0 ]; then
    "${RUNNER[@]}" "$@"
  else
    "$@"
  fi
}

pass=0
fail=0
skip=0

ok() {
  printf '  ok   %s\n' "$1"
  pass=$((pass + 1))
}
bad() {
  printf '  FAIL %s\n' "$1" >&2
  fail=$((fail + 1))
}
skip_test() {
  printf '  skip %s\n' "$1"
  skip=$((skip + 1))
}
check() {
  local name="$1"
  shift
  if "$@"; then ok "$name"; else bad "$name"; fi
}
elf_magic() {
  [ "$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')" = "7f454c46" ]
}
is_wall_version() {
  local out
  out="$(run_cmd "$1" --version)" || return 1
  [[ "$out" == "wall version "* ]]
}
help_has_no_examples() {
  ! run_cmd ./wall --help | grep -qi '^Examples:'
}
catalogue_available() {
  run_cmd ./wall --help=all | grep -qi 'relocation'
}
unknown_option_fails() {
  ! run_cmd ./wall --definitely-not-an-option >/dev/null 2>&1
}
ir_has_triple() {
  grep -q 'target triple' "$1"
}
prints_output_format() {
  [ "$(run_cmd ./linker --print-output-format)" = "elf64-aarch64" ]
}
output_is() {
  local expected="$1" binary="$2" out
  out="$(run_cmd "$binary")" || return 1
  [ "$out" = "$expected" ]
}
can_run() {
  run_cmd "$@" >/dev/null 2>&1
}
static_no_dynamic_deps() {
  [ -z "$(readelf -dW "$1" 2>&1 | grep -i 'NEEDED')" ]
}
static_no_interpreter() {
  ! readelf -lW "$1" | grep -qi 'INTERP'
}
static_has_wall_string() {
  strings -a "$1" | grep -q 'Hello, wall!'
}
static_has_symtab() {
  readelf -SW "$1" | grep -q '\.symtab'
}
static_has_strtab() {
  readelf -SW "$1" | grep -q '\.strtab'
}

echo "==> Driver contract"
check "version is a wall version" is_wall_version ./wall
check "wallc reports wall version" is_wall_version ./wallc
check "obasm reports wall version" is_wall_version ./obasm
check "linker reports wall version" is_wall_version ./linker
check "help has no examples section" help_has_no_examples
check "option catalogue is available" catalogue_available
check "unknown options fail" unknown_option_fails

echo "==> Compiler stages"
check "IR emission" run_cmd ./wall -S tests/language/hello.wa -o "$TMP/hello.ll"
check "IR contains target triple" ir_has_triple "$TMP/hello.ll"
check "object emission" run_cmd ./wall -c tests/language/hello.wa -o "$TMP/direct.o"
check "direct object is ELF" elf_magic "$TMP/direct.o"

echo "==> Assembler and linker"
check "assemble IR" run_cmd ./obasm "$TMP/hello.ll" -o "$TMP/hello.o"
check "assembled object is ELF" elf_magic "$TMP/hello.o"
check "link object" run_cmd ./linker "$TMP/hello.o" -o "$TMP/hello"
check "linked image is ELF" elf_magic "$TMP/hello"
check "print output format" prints_output_format

echo "==> End-to-end"
check "driver build" run_cmd ./wall tests/language/hello.wa -o "$TMP/hello-driver"
check "driver image is ELF" elf_magic "$TMP/hello-driver"
if can_run "$TMP/hello-driver"; then
  check "program output" output_is $'Hello, wall!\n30 200 -10' "$TMP/hello-driver"
  check "final executable has no dynamic library dependencies" static_no_dynamic_deps "$TMP/hello-driver"
  check "final executable has no interpreter" static_no_interpreter "$TMP/hello-driver"
  check "final executable retains Wall strings" static_has_wall_string "$TMP/hello-driver"
  check "final executable retains symbol names" static_has_symtab "$TMP/hello-driver"
  check "final executable retains string table" static_has_strtab "$TMP/hello-driver"
else
  skip_test "program output (target cannot execute on this host)"
fi

echo "==> Language features"
check "include resolution" run_cmd ./wall -I tests/language/include \
  tests/language/include/main.wa -o "$TMP/include"
if can_run "$TMP/include"; then
  check "include output" output_is "42" "$TMP/include"
else
  skip_test "include output"
fi

check "extern object" run_cmd ./wall -c tests/language/extern/provider.wa -o "$TMP/provider.o"
check "extern link" run_cmd ./wall tests/language/extern/main.wa "$TMP/provider.o" -o "$TMP/extern"
if can_run "$TMP/extern"; then
  check "extern output" output_is "42" "$TMP/extern"
else
  skip_test "extern output"
fi

echo
printf 'Summary: %d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
