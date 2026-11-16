# syntax=docker/dockerfile:1

# ---- Stage 1: build (musl, fully static, libcurl for the portal upload) ----
# The registry image is the curl build (WITH_CURL=1): it uploads by
# default, exactly the documented difference with the zero-dependency
# one-liner of the README, which stays offline-capable without libcurl.
FROM alpine:3.20 AS build
RUN apk add --no-cache \
        build-base \
        curl-dev curl-static \
        nghttp2-static brotli-static zstd-static libssh2-static \
        zlib-static openssl-libs-static \
        libidn2-static libunistring-static libpsl-static \
        c-ares-static gettext-static \
        ca-certificates
WORKDIR /src
COPY Makefile .
COPY src/ src/
# -static via CFLAGS (the Makefile link rule uses $(CFLAGS) at link
# time). CURL_STATIC_LIBS carries libcurl.a's own dependency closure —
# TLS, HTTP/2, brotli/zstd compression, c-ares DNS, IDN and the public
# suffix list — which only exists when the whole binary is static
# (dynamic builds rely on libcurl.so carrying its own deps).
RUN make CC=gcc WITH_CURL=1 \
        CFLAGS="-O2 -std=c11 -Wall -Wextra -pedantic \
                -ffp-contract=off -static" \
        CURL_STATIC_LIBS="-lssl -lcrypto -lnghttp2 \
                          -lbrotlidec -lbrotlienc -lbrotlicommon \
                          -lzstd -lssh2 -lz \
                          -lidn2 -lunistring -lpsl -lcares"

# ---- Stage 2: runtime — the lotofbench command and its TLS trust ----
FROM scratch
# The static binary resolves the CA bundle at the standard path; without
# it the HTTPS default endpoint cannot be verified.
COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/
COPY --from=build /src/lotofbench /usr/local/bin/lotofbench
ENTRYPOINT ["/usr/local/bin/lotofbench"]
