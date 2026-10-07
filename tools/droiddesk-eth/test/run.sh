#!/bin/bash
# Builds droiddesk-eth (amd64 for the test, arm64 for the phone) in a golang container and runs
# the network test in a Linux container: a TAP bridged to namespaces with real hosts.
# Windows/Git Bash: MSYS_NO_PATHCONV=1 keeps Docker's Linux paths intact.
set -eu
export MSYS_NO_PATHCONV=1
HERE=$(cd "$(dirname "$0")/.." && pwd)
# Docker Desktop on Windows wants C:/... for bind mounts; pwd -W exists only in Git Bash.
SRC=$( (cd "$HERE" && pwd -W) 2>/dev/null || echo "$HERE")
GO_IMAGE=${GO_IMAGE:-golang:1.27}
TEST_IMAGE=droiddesk-eth-test

echo "=== build"
docker run --rm -v ddeth-gomod:/go/pkg/mod -v ddeth-gocache:/root/.cache/go-build \
    -v "$SRC":/src -w /src "$GO_IMAGE" sh -c '
    set -e
    go vet ./...
    test -z "$(gofmt -l .)" || { echo "gofmt:"; gofmt -l .; exit 1; }
    for arch in amd64 arm64; do
        CGO_ENABLED=0 GOOS=linux GOARCH=$arch go build -trimpath -ldflags="-s -w" -o bin/droiddesk-eth-$arch .
    done
    ls -l bin'

echo "=== test image"
docker build -q -t "$TEST_IMAGE" "$SRC/test" >/dev/null

echo "=== test"
# SYS_ADMIN only for `ip netns` (mounting the namespaces); the daemon itself needs just the TAP,
# and the fake-bridge part runs it as nobody.
docker run --rm --cap-add NET_ADMIN --cap-add SYS_ADMIN --device /dev/net/tun \
    -v "$SRC":/src:ro "$TEST_IMAGE" bash /src/test/inside.sh
