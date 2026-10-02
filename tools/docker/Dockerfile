# Build environment for PsiTerm, PsiMail and PsiWeb on any Linux box with
# Docker (no root or apt needed on the host). It holds only the tools; the
# EPOC R5 SDK itself lives on the host (tools/docker/psibuild sets it up),
# so builds are incremental and its headers and docs stay readable there.
#   docker build -t psion-build tools/docker
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-multilib g++-multilib flex bison zlib1g-dev \
        wine wine32:i386 \
        python3 python3-pil git perl gperf pkg-config libpng-dev \
        xz-utils unzip ca-certificates file \
    && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all
