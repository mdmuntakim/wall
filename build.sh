#!/bin/bash
# Wall Compiler build script
set -e

cd "$(dirname "$0")"

TARGET=aarch64-unknown-linux-android24

# ---------------------------------------------------------------------------
# Real build identity: a content hash of the compiler sources.  This value is
# compiled into every artifact and written into the ELF .comment section by
# the linker's own in-place section editor, so no host compiler banner remains.
# ---------------------------------------------------------------------------
HASH=$( { cat stage0/lib/libwall.c stage0/src/wall.c stage0/include/wall.h \
              tools/obasm.c tools/linker.c tools/elfcomment.h; } \
        | sha256sum | cut -c1-16 )
if [ -z "$HASH" ]; then HASH=285fd1c; fi
COMMENT="wallc version 1.0.0 ($HASH)"
DEFINE="-DWALL_DEFAULT_COMMENT=\"$COMMENT\""

echo "==> Build identity: $COMMENT"

echo "==> Building Wall Compiler backend (libwall.so)..."
clang --target=$TARGET -shared -fPIC "$DEFINE" \
    stage0/lib/libwall.c -I stage0/include \
    -Wl,-soname,libwall.so -o libwall.so -ldl

echo "==> Building Wall Compiler driver (wall)..."
clang --target=$TARGET "$DEFINE" -I stage0/include \
    stage0/src/wall.c -ldl -o wall

echo "==> Building obasm (assembler: .s -> .o)..."
clang --target=$TARGET "$DEFINE" tools/obasm.c -o obasm

echo "==> Building linker (linker: .o -> .so / ELF)..."
clang --target=$TARGET "$DEFINE" tools/linker.c -o linker

# ---------------------------------------------------------------------------
# Own the .comment sections.  The linker performs this edit itself, so the
# toolchain no longer needs a separate elfcomment binary.  A running executable
# cannot be opened for writing (ETXTBSY), so stamp through a temporary copy.
# ---------------------------------------------------------------------------
echo "==> Writing Wall .comment sections..."
cp linker .linker.run
./.linker.run --stamp-comment="$COMMENT" wall libwall.so obasm linker
rm -f .linker.run

echo "Build OK"
echo "   wall       - Wall compiler driver (.wa -> .s)"
echo "   obasm      - object assembler (.s -> .o)"
echo "   linker     - linker (.o -> .so / ELF) and ELF .comment editor"
