FROM ubuntu:24.04
ARG IMAGE_FINGERPRINT
LABEL dev.fingerprint="${IMAGE_FINGERPRINT}"

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    --no-install-recommends \
    build-essential \
    clang \
    clang-tools \
    cmake \
    ninja-build \
    pkg-config \
    git \
    ca-certificates \
    curl \
    gdb \
    lldb \
 && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++
CMD ["/bin/bash", "-l"]
