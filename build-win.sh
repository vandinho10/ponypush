#!/bin/sh
# Build Windows (mingw-w64) dentro de container arm64 debian:bookworm-slim.
set -e
apt-get update -qq > /dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  g++-mingw-w64-x86-64-posix g++-mingw-w64-i686-posix make file > /dev/null

make clean > /dev/null || true

echo "== amd64 =="
make TARGET_OS=Windows_NT CXX=x86_64-w64-mingw32-g++-posix all
cp -v ponypush.exe ponypush-windows-amd64.exe

echo "== x86 =="
make clean > /dev/null
make TARGET_OS=Windows_NT CXX=i686-w64-mingw32-g++-posix all
cp -v ponypush.exe ponypush-windows-x86.exe

file ponypush-windows-amd64.exe ponypush-windows-x86.exe
ls -lh ponypush-windows-*.exe
