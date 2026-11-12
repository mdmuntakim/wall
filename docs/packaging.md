# Packaging

Wall ships a Termux-compatible Debian package built with `dpkg-deb`. The
package metadata is templated in `packaging/debian/control.in`.

## Build a package

```sh
make package
```

The script:

1. builds `wall`, `libwall.so`, and the compatibility symlinks;
2. stages them under
   `.wallbuild/debroot/data/data/com.termux/files/usr/{bin,lib}`;
3. expands `packaging/debian/control.in`;
4. writes `DEBIAN/md5sums`;
5. creates `wall_<version>_<arch>.deb` in the repository root.

## Verify

```sh
dpkg-deb -I wall_1.0.0_aarch64.deb
dpkg-deb -c wall_1.0.0_aarch64.deb
```

Expected layout:

```text
/data/data/com.termux/files/usr/bin/wall
/data/data/com.termux/files/usr/bin/wallc      -> wall
/data/data/com.termux/files/usr/bin/obasm      -> wall
/data/data/com.termux/files/usr/bin/linker     -> wall
/data/data/com.termux/files/usr/bin/ld.wall    -> wall
/data/data/com.termux/files/usr/lib/libwall.so
```

## Install without a package

```sh
make install
```

`PREFIX` defaults to the Termux prefix and can be overridden:

```sh
PREFIX=/usr/local make install
```

When installing outside Termux, keep `wall` and `libwall.so` in the matching
`bin`/`lib` directories, or point `WALL_BACKEND` at the library.

## Versioning

The package version comes from the `VERSION` file. The compiled identity is a
content hash of the sources, so rebuilding after a source change produces a new
`wall version ... (<hash>)` banner without editing a header.
