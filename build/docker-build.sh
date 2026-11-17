#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build on the (remote) Docker host: stream sources in, binaries out.
# Usage: build/docker-build.sh [make args]   (run from the homekit/ folder)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/out
COPYFILE_DISABLE=1 tar -c --exclude=.git --exclude=build/out --exclude='*/tests' --exclude='*/Documentation' \
  src build/Makefile \
| docker run --rm -i thingino-hap-build sh -c "
    mkdir -p /work && tar -x -C /work 2>/dev/null && cd /work/build &&
    make -j\$(nproc) $* >&2 && tar -c -C /work/build out/hap-setupgen out/rhkd" \
| tar -x -C build
ls -la build/out
