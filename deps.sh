#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch rhkd's third-party sources into src/ at pinned commits and apply our patches.
# Our own code (src/rhkd, src/pal-thingino) is tracked in this repo; everything fetched here is not.
#
#   ./deps.sh            build dependencies (HomeKitADK, mbedtls, raptor-ipc)
#   ./deps.sh --ref      also the reference-only sources (raptor, raptor-hal, raptor-common)
set -eu
cd "$(dirname "$0")/src"

fetch() { # name url commit
	if [ ! -d "$1/.git" ]; then
		git clone -q "$2" "$1"
	fi
	git -C "$1" fetch -q origin "$3" 2>/dev/null || git -C "$1" fetch -q origin
	git -C "$1" checkout -q "$3"
	echo "$1 @ $(git -C "$1" rev-parse --short HEAD)"
}

patch_dir() { # name
	for p in ../patches/"$1"/*.patch; do
		[ -f "$p" ] || continue
		git -C "$1" -c user.name=deps -c user.email=deps@localhost am -q "$PWD/$p"
		echo "  applied $(basename "$p")"
	done
}

# Build dependencies
fetch HomeKitADK https://github.com/apple/HomeKitADK.git fb201f98f5fdc7fef6a455054f08b59cca5d1ec8
patch_dir HomeKitADK
fetch mbedtls https://github.com/Mbed-TLS/mbedtls.git 2fc8413bfcb51354c8e679141b17b3f1a5942561 # v2.28.10
fetch raptor-ipc https://github.com/gtxaspec/raptor-ipc.git 00810bb7b91c3e21210b5a76d43748a23758f36a

# Reference only (protocol/ring behaviour, upstream fixes). Fixes go on branches in the fork clones
# under workspace/projects/ — see ../upstream/upstream.md (Repos and workflow).
if [ "${1:-}" = "--ref" ]; then
	fetch raptor https://github.com/gtxaspec/raptor.git 101fb359f941d4f8724c74af8617477f00bd4520
	fetch raptor-hal https://github.com/gtxaspec/raptor-hal.git 9f096647a17e45abb638d89dbd502022c782be0f
	fetch raptor-common https://github.com/gtxaspec/raptor-common.git ab1f9fd40dc917388449c201a0945c051587f4fb
fi
