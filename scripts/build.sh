#!/bin/bash
# Wall toolchain build, install and package driver.
#
# Usage:
#   scripts/build.sh [build|install|package|clean]
#
# Environment:
#   TARGET       clang target triple (default: aarch64-unknown-linux-android24)
#   PREFIX       installation prefix (default: Termux $PREFIX or /usr/local)
#   ARCH         Debian architecture (default: dpkg --print-architecture)
#   SYSROOT      optional cross-compilation sysroot
#   CFLAGS_EXTRA additional compiler flags
#
# The script always runs from the repository root, independent of the caller's
# current directory.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="${TARGET:-aarch64-unknown-linux-android24}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
VERSION="$(cat VERSION)"
ARCH="${ARCH:-$(dpkg --print-architecture 2>/dev/null || uname -m)}"
SYSROOT="${SYSROOT:-}"
CFLAGS_EXTRA="${CFLAGS_EXTRA:-}"
BUILD_DIR="$ROOT/.wallbuild"

HOST_TRIPLET="$(gcc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"
CROSS=0
if [ "$TARGET" != "$HOST_TRIPLET" ]; then
  CROSS=1
fi

if [ "$CROSS" = 1 ]; then
  # Cross build: the host GNU linker cannot emit foreign ELF targets, so use
  # LLVM's linker, and point the compiler at the cross GCC runtime
  # (crtbeginS.o, libgcc) that the clang driver does not find by itself.  The
  # cross GCC runtime already provides the target headers and libraries, so a
  # forced sysroot is not needed and LLVM's linker cannot resolve the GNU
  # linker script GROUP entries through one.
  CROSS_CFLAGS="-fuse-ld=lld -B/usr/lib/gcc-cross/${TARGET} -L/usr/lib/gcc-cross/${TARGET}"
  for flag in $CFLAGS_EXTRA; do
    case "$flag" in
      --sysroot=*) ;;
      *) CROSS_CFLAGS="$CROSS_CFLAGS $flag" ;;
    esac
  done
  CFLAGS_EXTRA="$CROSS_CFLAGS"
fi

if [ -n "$SYSROOT" ] && [ "$CROSS" != 1 ]; then
  CFLAGS_EXTRA="--sysroot=$SYSROOT $CFLAGS_EXTRA"
fi

# A real build identity: a content hash of the toolchain sources.  The value is
# compiled into every artifact and written into the ELF .comment section by the
# linker's own section editor, so no host compiler banner remains.
SOURCES=(
  VERSION
  include/wall/wall.h
  src/compiler/libwall.c
  src/driver/wall.c
  src/assembler/obasm.c
  src/linker/linker.c
  src/common/wall_cli.c
  src/common/wall_cli.h
  src/common/wall_log.h
  src/common/elfcomment.h
  src/linker/runtime_blob.h
)
HASH="$(cat "${SOURCES[@]}" | sha256sum | cut -c1-16)"
COMMENT="wall version $VERSION ($HASH)"
DEFINE="-DWALL_DEFAULT_COMMENT=\"$COMMENT\""
CFLAGS="--target=$TARGET -O2 -g0 -fno-ident -fno-asynchronous-unwind-tables $CFLAGS_EXTRA"
INCLUDES="-I include -I src/common -I src/linker"

echo "==> Build identity: $COMMENT"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "==> Building merged runtime library (libwall.so)..."
clang $CFLAGS "$DEFINE" -DWALL_MULTICALL -shared -fPIC $INCLUDES \
    src/compiler/libwall.c src/assembler/obasm.c src/linker/linker.c \
    src/common/wall_cli.c \
    -Wl,-soname,libwall.so -o libwall.so

echo "==> Building Wall driver (wall)..."
clang $CFLAGS "$DEFINE" $INCLUDES src/driver/wall.c -ldl -o wall

echo "==> Creating compatibility links..."
ln -sf wall wallc
ln -sf wall obasm
ln -sf wall linker
ln -sf wall ld.wall

# Own the .comment sections.  A running executable cannot be opened for
# writing (ETXTBSY), so the stamp tool is a separate build-time artifact.
echo "==> Writing Wall .comment sections..."
clang $CFLAGS "$DEFINE" $INCLUDES src/linker/linker.c src/common/wall_cli.c \
    -o "$BUILD_DIR/stamp"
if [ -n "${TEST_RUNNER:-}" ]; then
  read -r -a WALL_RUNNER <<< "$TEST_RUNNER"
else
  WALL_RUNNER=()
fi
if [ "${#WALL_RUNNER[@]}" -gt 0 ]; then
  "${WALL_RUNNER[@]}" "$BUILD_DIR/stamp" --stamp-comment="$COMMENT" wall libwall.so
else
  "$BUILD_DIR/stamp" --stamp-comment="$COMMENT" wall libwall.so
fi
rm -f "$BUILD_DIR/stamp"

install_tree() {
  local dest="$1"
  mkdir -p "$dest/bin" "$dest/lib"
  install -m 0755 wall "$dest/bin/wall"
  install -m 0644 libwall.so "$dest/lib/libwall.so"
  ln -sf wall "$dest/bin/wallc"
  ln -sf wall "$dest/bin/obasm"
  ln -sf wall "$dest/bin/linker"
  ln -sf wall "$dest/bin/ld.wall"
}

case "${1:-build}" in
  build)
    echo "Build OK"
    echo "   wall       - Wall toolchain driver"
    echo "   libwall.so - compiler + assembler + linker"
    echo "   wallc, obasm, linker, ld.wall - compatibility links"
    ;;
  install)
    echo "==> Installing into $PREFIX..."
    install_tree "$PREFIX"
    echo "Installed: $PREFIX/bin/wall"
    ;;
  package)
    echo "==> Building .deb package..."
    DEBROOT="$BUILD_DIR/debroot"
    TERMUX_ROOT="$DEBROOT/data/data/com.termux/files/usr"
    rm -rf "$DEBROOT"
    mkdir -p "$DEBROOT/DEBIAN" "$TERMUX_ROOT/bin" "$TERMUX_ROOT/lib"
    chmod 0755 "$DEBROOT/DEBIAN" "$TERMUX_ROOT/bin" "$TERMUX_ROOT/lib"
    install_tree "$TERMUX_ROOT"

    SIZE="$(du -sk "$DEBROOT" | cut -f1)"
    sed -e "s/@VERSION@/$VERSION/g" \
        -e "s/@ARCH@/$ARCH/g" \
        -e "s/@SIZE@/$SIZE/g" \
        packaging/debian/control.in > "$DEBROOT/DEBIAN/control"
    chmod 0644 "$DEBROOT/DEBIAN/control"

    ( cd "$DEBROOT" && find . -path ./DEBIAN -prune -o -type f -print0 \
        | sort -z | xargs -0 md5sum > DEBIAN/md5sums )
    chmod 0644 "$DEBROOT/DEBIAN/md5sums"

    DEB="wall_${VERSION}_${ARCH}.deb"
    dpkg-deb --build --root-owner-group "$DEBROOT" "$DEB" >/dev/null
    echo "Created $DEB"
    dpkg-deb -I "$DEB"
    ;;
  clean)
    rm -rf "$BUILD_DIR" libwall.so wall wallc obasm linker ld.wall
    echo "Cleaned"
    ;;
  *)
    echo "usage: $0 [build|install|package|clean]" >&2
    exit 2
    ;;
esac
