# SPDX-License-Identifier: GPL-3.0-or-later
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends make curl ca-certificates xz-utils python3 file && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://musl.cc/mipsel-linux-musl-cross.tgz | tar -xz -C /opt
ENV PATH=/opt/mipsel-linux-musl-cross/bin:$PATH
WORKDIR /work
