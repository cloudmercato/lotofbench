# syntax=docker/dockerfile:1

# ---- Stage 1: build (musl, fully static) ----
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY Makefile .
COPY src/ src/
# -static via CFLAGS: the Makefile link rule uses $(CFLAGS) at link time.
RUN make CC=gcc CFLAGS="-O2 -std=c11 -Wall -Wextra -pedantic -static"

# ---- Stage 2: runtime — nothing but the lotofbench command ----
FROM scratch
COPY --from=build /src/lotofbench /usr/local/bin/lotofbench
ENTRYPOINT ["/usr/local/bin/lotofbench"]
