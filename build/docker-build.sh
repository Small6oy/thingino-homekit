#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Incremental cross-build on the remote Docker host.
# Usage: [SOC=xburst1|xburst2] build/docker-build.sh [make args]   (run from the homekit/ folder)
#   xburst1 (default): T31 (Wyze v3). musl.cc toolchain, FP32 / legacy NaN.                        -> build/out/
#   xburst2:           T41 (Wyze v4). Thingino's xburst2 musl toolchain, FP64 / NAN2008 (as the firmware). -> build/out-xburst2/
# The two FP ABIs are NOT interchangeable: FP32 double code broke on T41 worker threads (homekit.md).
#
# How it stays fast: a persistent tree on the build host (~/rhkd-build). Each run ships only our own sources
# (src/rhkd, src/pal-thingino, patches, deps.sh, build/Makefile; tar keeps mtimes so make only rebuilds what
# changed). Third-party sources are fetched ON THE HOST by deps.sh, again only when deps.sh or the patches
# change. Object files stay on the host per SoC, so HomeKitADK/mbedTLS/Opus/faac compile once.
set -e
cd "$(dirname "$0")/.."
HOST=${BUILD_HOST:?set BUILD_HOST=user@docker-host (SSH access; the same host as your docker context)}
RDIR=${BUILD_DIR:-rhkd-build}
case "${SOC:-xburst1}" in
	xburst1) IMAGE=thingino-hap-build; OUTDIR=out; CROSSARG= ;;
	xburst2) IMAGE=thingino-hap-build-xburst2; OUTDIR=out-xburst2; CROSSARG="CROSS=mipsel-thingino-linux-musl-" ;;
	*) echo "unknown SOC $SOC" >&2; exit 1 ;;
esac

# 1. Our sources (replace the dirs, so deleted files don't linger).
COPYFILE_DISABLE=1 tar -c src/rhkd src/pal-thingino patches deps.sh build/Makefile \
| ssh "$HOST" "mkdir -p $RDIR && cd $RDIR && rm -rf src/rhkd src/pal-thingino patches && tar -x"

# 2. Third-party sources on the host, when deps.sh or the patches changed.
STAMP=$(cat deps.sh patches/*/*.patch | shasum | cut -c1-16)
ssh "$HOST" "cd $RDIR && if [ \"\$(cat .deps-stamp 2>/dev/null)\" != $STAMP ]; then
	echo 'fetching third-party sources on the build host (deps.sh)' >&2
	sh deps.sh >&2 && echo $STAMP > .deps-stamp
fi"

# 3. Compile in the container on the host tree (bind mount of a host path).
RHOME=$(ssh "$HOST" 'echo $HOME')
docker run --rm -u "$(ssh "$HOST" 'id -u'):$(ssh "$HOST" 'id -g')" -v "$RHOME/$RDIR:/work" -w /work/build "$IMAGE" \
	sh -c "make -j\$(nproc) $CROSSARG OUT=$OUTDIR $* >&2"

# 4. Fetch the binaries.
mkdir -p build/$OUTDIR
ssh "$HOST" "tar -c -C $RDIR/build $OUTDIR/hap-setupgen $OUTDIR/rhkd" | tar -x -C build
ls -la build/$OUTDIR
