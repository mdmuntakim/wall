#!/bin/bash
# Wall Compiler build script
set -e

cd "$(dirname "$0")"

echo "==> Building Wall Compiler backend (libwall.so)..."
clang --target=aarch64-unknown-linux-android24 -shared -fPIC \
    stage0/lib/libwall.c -I stage0/include \
    -Wl,-soname,libwall.so -o libwall.so -ldl

echo "==> Building Wall Compiler driver (wall)..."
clang --target=aarch64-unknown-linux-android24 -I stage0/include \
    stage0/src/wall.c -ldl -o wall

echo "==> Building obasm (assembler: .s -> .o)..."
clang --target=aarch64-unknown-linux-android24 tools/obasm.c -o obasm

echo "Build OK"
