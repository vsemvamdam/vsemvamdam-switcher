#!/bin/sh
# Builds release archives: dist/vsemvamdam-switcher-<ver>-win64.zip and the source archive.
set -e
cd "$(dirname "$0")/.."
VER=$(grep VS_VER_STRING src/version.h | sed 's/.*"\(.*\)".*/\1/')
make -j2
rm -rf dist && mkdir -p dist/pkg
cp build/vsemvamdam-switcher.exe README.md LICENSE dist/pkg/
(cd dist/pkg && zip -q -9 ../vsemvamdam-switcher-$VER-win64.zip vsemvamdam-switcher.exe README.md LICENSE)
git archive --format=zip -o dist/vsemvamdam-switcher-$VER-src.zip HEAD 2>/dev/null || \
  zip -q -r -9 dist/vsemvamdam-switcher-$VER-src.zip Makefile README.md LICENSE .gitignore src res tests tools third_party
rm -rf dist/pkg
ls -la dist
