#!/bin/sh
# Compatibility entry point. The implementation lives in scripts/build.sh.
exec "$(dirname "$0")/scripts/build.sh" "$@"
