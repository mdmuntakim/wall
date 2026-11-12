# Wall toolchain - top-level build interface.
#
# `make` builds the driver and the merged libwall.so.  `make test` runs the
# integration suite.  `make package` produces a Termux-layout .deb.

SHELL := /bin/bash
VERSION := $(shell cat VERSION)
ARCH := $(shell dpkg --print-architecture 2>/dev/null || uname -m)

.PHONY: all build install package test check clean distclean format help

all: build

build:
	@./scripts/build.sh build

install:
	@./scripts/build.sh install

package:
	@./scripts/build.sh package

test: build
	@./tests/run.sh

check: test

clean:
	@./scripts/build.sh clean
	@rm -rf tests/tmp

distclean: clean
	@rm -f wall_$(VERSION)_$(ARCH).deb wall-*.tar.xz

format:
	@command -v clang-format >/dev/null || { echo "clang-format not found" >&2; exit 1; }
	@clang-format -i $$(find src include -name '*.c' -o -name '*.h')

help:
	@printf '%s\n' \
	  'Targets:' \
	  '  build     Build wall and libwall.so (default)' \
	  '  install   Install into $$PREFIX' \
	  '  package   Build wall_$(VERSION)_$(ARCH).deb' \
	  '  test      Build and run the integration tests' \
	  '  clean     Remove build products' \
	  '  distclean Remove build products, packages and tarballs' \
	  '  format    Run clang-format over the C sources'
