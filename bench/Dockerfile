# The PDF/A and Grayscale test bench: every tool at a fixed version, so anyone can re-run it.
#
# The base and the Ghostscript and LibreOffice packages are what FynePDF's servers ran before
# fynepdf-pdfa (Debian bookworm, 2026-10-24), so the Ghostscript baseline is a real production
# setup. Untrusted PDFs are only ever opened inside this container.

# veraPDF ships on Alpine with its own Java; only its jars are taken, and run on Debian's Java.
FROM ghcr.io/verapdf/cli:v1.31.206@sha256:5477db97a53d8054194e5bdbc7568cadafb82e5d568c08bce95b1a81d7aff6a7 AS verapdf

# The latest Ghostscript, built from source with its own bundled libraries, as a second
# baseline next to production's Debian 10.00.0 (OCRmyPDF refuses 10.0.0 to 10.02.0).
FROM node:22.23.4-slim AS ghostscript-latest
RUN apt-get update && apt-get install -y --no-install-recommends build-essential xz-utils pkg-config ca-certificates
ADD --checksum=sha256:c20492bc8ebb96c87fa2e52a0926e1cda8cde95d66145e018ac713fed5da38cf \
    https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/download/gs10080/ghostscript-10.08.0.tar.xz /src/gs.tar.xz
RUN cd /src && tar xf gs.tar.xz && cd ghostscript-10.08.0 \
    && ./configure --prefix=/opt/gs-10.08.0 --without-x --disable-cups --disable-gtk --disable-dbus --without-tesseract \
    && make -j"$(nproc)" && make install

FROM node:22.23.4-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    # As production: the converters and the tools around them
    ghostscript \
    qpdf \
    poppler-utils \
    time \
    python3 \
    python3-pip \
    tesseract-ocr \
    tesseract-ocr-eng \
    # veraPDF's Java
    default-jre-headless \
    # Making test files: LaTeX (pdfTeX, and dvips for Type 3 bitmap fonts), CCITT and
    # JPEG 2000 pictures, fonts for Devanagari and Arabic
    texlive-latex-base \
    texlive-binaries \
    libtiff-tools \
    libopenjp2-tools \
    fonts-noto-core \
    && pip3 install --no-cache-dir --break-system-packages ocrmypdf==17.13.0 img2pdf==0.6.3 \
    && apt-get purge -y --auto-remove python3-pip \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /root/.cache

# As production: LibreOffice 25.2 from bookworm-backports, the -nogui builds, and the
# metric-compatible fonts.
RUN echo "deb http://deb.debian.org/debian bookworm-backports main" > /etc/apt/sources.list.d/bookworm-backports.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends -t bookworm-backports \
        libreoffice-writer-nogui libreoffice-impress-nogui libreoffice-draw-nogui \
    && apt-get install -y --no-install-recommends \
        fonts-liberation2 fonts-crosextra-carlito fonts-crosextra-caladea fonts-dejavu-core \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /root/.cache

# Making test files: pyHanko signs PDFs with a throwaway certificate. poppler-data has the
# CMaps Poppler needs to draw CJK text that names Adobe's CMaps.
RUN apt-get update && apt-get install -y --no-install-recommends python3-pip poppler-data \
    && pip3 install --no-cache-dir --break-system-packages pyHanko==0.38.0 pyhanko-cli==0.6.0 \
    && apt-get purge -y --auto-remove python3-pip \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /root/.cache

COPY --from=verapdf /opt/verapdf /opt/verapdf
COPY --from=ghostscript-latest /opt/gs-10.08.0 /opt/gs-10.08.0
ENV PATH="/opt/verapdf:${PATH}" \
    JAVA_OPTS="-Xmx6g"

# pdf.js reads structure and text (a neutral reader, not one of the converters); pdf-lib
# makes test files.
WORKDIR /opt/deps
COPY package.json ./
RUN npm install --no-audit --no-fund --omit=optional && npm cache clean --force

WORKDIR /bench
