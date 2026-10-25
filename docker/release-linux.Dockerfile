# The Linux release build: manylinux_2_28 (AlmaLinux 8, glibc 2.28, GCC 14), so the command line
# runs on any Linux from 2018 on (Debian 10, Ubuntu 18.10, RHEL 8 and newer). Its dependencies
# come from vcpkg and are linked in (vcpkg itself needs zip, and nasm for libjpeg-turbo);
# scripts/release-build.mjs runs inside it with Node (pinned, checked).
ARG BASE
FROM ${BASE}
ARG NODE=22.23.4
RUN dnf install -y --setopt=install_weak_deps=False zip nasm && dnf clean all \
    && pipx install ninja \
    && case "$(uname -m)" in \
         x86_64) arch=x64; sha=1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce ;; \
         aarch64) arch=arm64; sha=6287aa46b6c375d816e63ae66d024fbbfd70404825aabdfdf898a970871d2f4b ;; \
       esac \
    && curl -fsSL "https://nodejs.org/dist/v$NODE/node-v$NODE-linux-$arch.tar.xz" -o /tmp/node.tar.xz \
    && echo "$sha  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz \
    && git config --system --add safe.directory '*'
ENV FYNEPDF_IN_CONTAINER=1
WORKDIR /src
