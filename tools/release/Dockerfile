# The PC (x86_64) release build of alia_engine: Debian 13, the same baseline as the board (glibc 2.41, SDL3), so the
# module runs on Debian 13, Ubuntu 25.04+, Fedora 42+, Arch and the like. tools/release.sh builds with it.
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build clang lld file libsdl3-dev libegl-dev libgles-dev python3-dev python3-setuptools python3-pip \
    && rm -rf /var/lib/apt/lists/*
