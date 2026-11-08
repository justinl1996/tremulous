#!/bin/bash
#
# Builds the web release:
#   1. compiles the code, which packs the QVMs into vms-gpp-*.pk3
#   2. writes the CRC32 of every pk3 in the build into the paks list in
#      src/sys/sys_common.js, which the web client checks its downloads against
#   3. compiles again, so the web client (and node server) carry the new
#      checksums, and builds the native dedicated server for the web
#
# usage: scripts/build-web.sh [make jobs]

set -euo pipefail

cd "$(dirname "$0")/.."

JOBS=${1:-$(nproc)}
RELEASE_JS=build/release-js-js
WEBSERVER_DIR=build/webserver

echo "=== Removing $RELEASE_JS and $WEBSERVER_DIR for a clean build"
rm -rf "$RELEASE_JS" "$WEBSERVER_DIR"

echo "=== Building the pk3 assets"
make PLATFORM=js release -j"$JOBS"

echo "=== Updating pk3 checksums in src/sys/sys_common.js"
for dest in $(sed -n "s/.*dest: '\([^']*\.pk3\)', checksum: [0-9]*.*/\1/p" src/sys/sys_common.js); do
    [ -f "$RELEASE_JS/$dest" ] || continue

    crc=$(cksum -a crc32b "$RELEASE_JS/$dest" | cut -d' ' -f1)
    printf '  %-28s %s\n' "$dest" "$crc"
    sed -i "s|\(dest: '$dest', checksum: \)[0-9]*|\1$crc|" src/sys/sys_common.js
done

echo "=== Building the web client"
make PLATFORM=js release -j"$JOBS"

echo "=== Building the web dedicated server"
make release BUILD_CLIENT=0 BUILD_GAME_SO=0 BUILD_GAME_QVM=0 WEB_SERVER=1 BUILD_DIR="$WEBSERVER_DIR" -j"$JOBS"

echo
echo "Client: $RELEASE_JS/tremulous.js, $RELEASE_JS/tremulous.wasm"
echo "Server: $WEBSERVER_DIR/release-linux-x86_64/tremded"
echo "Assets: $RELEASE_JS/gpp/*.pk3"
