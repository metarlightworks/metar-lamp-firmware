#!/usr/bin/env bash
# Create or update the Production bulb release asset from an Arduino export.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(git -C "$SCRIPT_DIR/../.." rev-parse --show-toplevel)"
BULB_DIR="$REPO_ROOT/firmware/METARLightworks_Bulb/METARLightworks_Bulb"
VERSION_HEADER="$BULB_DIR/version.h"
INPUT_BINARY="$BULB_DIR/build/esp32.esp32.esp32c3/METARLightworks_Bulb.ino.bin"
ASSET_NAME="METARLightworks_Bulb_ESP32C3.bin"
RELEASE_BINARY="$BULB_DIR/$ASSET_NAME"
GITHUB_REPO="METARlightworks/metar-bulb-firmware"

usage() {
  echo "Usage: $(basename "$0") [--check]" >&2
  echo "  --check  Validate the exported binary and release metadata without GitHub changes." >&2
}

case "${1:-}" in
  "") check_only=false ;;
  --check) check_only=true ;;
  -h|--help) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac

if [[ ! -f "$VERSION_HEADER" ]]; then
  echo "ERROR: Version header not found: $VERSION_HEADER" >&2
  exit 1
fi

FW_VERSION="$(sed -nE 's/^[[:space:]]*#define[[:space:]]+FW_VERSION[[:space:]]+"([^"]+)".*/\1/p' "$VERSION_HEADER")"
if [[ -z "$FW_VERSION" ]]; then
  echo "ERROR: Could not read FW_VERSION from $VERSION_HEADER" >&2
  exit 1
fi

if [[ ! -f "$INPUT_BINARY" ]]; then
  echo "ERROR: Arduino application binary is missing:" >&2
  echo "  $INPUT_BINARY" >&2
  echo "Compile/export the sketch for ESP32-C3 first; do not use merged, bootloader, or partitions binaries." >&2
  exit 1
fi

if [[ ! -s "$INPUT_BINARY" ]]; then
  echo "ERROR: Arduino application binary is empty: $INPUT_BINARY" >&2
  exit 1
fi

byte_size="$(wc -c < "$INPUT_BINARY" | tr -d '[:space:]')"
md5_hash="$(md5 -q "$INPUT_BINARY")"
sha256_hash="$(shasum -a 256 "$INPUT_BINARY" | awk '{print $1}')"

echo "Release version: v$FW_VERSION"
echo "Application binary: $INPUT_BINARY"
echo "Asset filename: $ASSET_NAME"
echo "Byte size: $byte_size"
echo "MD5: $md5_hash"
echo "SHA-256: $sha256_hash"

if "$check_only"; then
  echo "Validation only: no files copied and no GitHub release action performed."
  exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "ERROR: GitHub CLI (gh) is required to publish releases." >&2
  exit 1
fi

cp -f "$INPUT_BINARY" "$RELEASE_BINARY"
echo "Copied release asset: $RELEASE_BINARY"

release_tag="v$FW_VERSION"
if gh release view "$release_tag" --repo "$GITHUB_REPO" >/dev/null 2>&1; then
  gh release upload "$release_tag" "$RELEASE_BINARY" --clobber --repo "$GITHUB_REPO"
else
  gh release create "$release_tag" "$RELEASE_BINARY" \
    --repo "$GITHUB_REPO" \
    --title "$release_tag" \
    --generate-notes
fi

if ! gh release view "$release_tag" --repo "$GITHUB_REPO" --json assets --jq '.assets[].name' | grep -Fxq "$ASSET_NAME"; then
  echo "ERROR: Release $release_tag does not contain $ASSET_NAME after upload." >&2
  exit 1
fi

echo "Verified $ASSET_NAME on GitHub release $release_tag."
