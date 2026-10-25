# Builds the library and its command line for Linux, against Debian bookworm's qpdf,
# libjpeg-turbo, OpenJPEG and Little CMS: the same libraries as the bench image, so the binary
# runs there.
FROM node:22.23.4-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config libqpdf-dev libjpeg62-turbo-dev libopenjp2-7-dev liblcms2-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/*
WORKDIR /src
