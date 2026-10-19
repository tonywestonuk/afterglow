#!/bin/sh
# Test and build afterglowd as a static aarch64 binary, in the afterglow-userland container:
#   docker run --rm -v "$PWD":/work afterglow-userland /work/system/afterglowd/build.sh
# Output: out/afterglowd/afterglowd. Standard library only, so nothing is downloaded.
set -eu
cd /work/system/afterglowd
export CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GOTOOLCHAIN=local GOFLAGS=-trimpath \
    GOCACHE=/work/out/afterglowd/cache GOPATH=/tmp/go
go vet ./...
go test ./...
go build -ldflags "-s -w" -o /work/out/afterglowd/afterglowd .
ls -l /work/out/afterglowd/afterglowd
