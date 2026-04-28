#!/usr/bin/env bash
#
# Build CHKDSK.EXE and pack it into a FAT12 floppy image plus a
# distribution zip. Adapted from utils/run/create_floppy_image.sh.
#
# Usage: create_floppy_image.sh [image_path] [dist_zip_path]
#
# Defaults (computed from script location):
#   image_path     = $repo_root/build/chkdsk.img
#   dist_zip_path  = $repo_root/dist/checkdsk.zip
#
# Local-machine overrides (e.g. SDCC290_BIN_DIR) belong in a separate
# *.local.sh wrapper that exec's this script with appropriate
# environment, not in this committed file. See CLAUDE.md "Scripts and
# build packaging".
set -euo pipefail

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Error: required command not found: $1" >&2
    exit 1
  fi
}

need_cmd mformat
need_cmd mcopy
need_cmd mmd
need_cmd make
need_cmd zip

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

image_path="${1:-$repo_root/build/chkdsk.img}"
dist_zip_path="${2:-$repo_root/dist/checkdsk.zip}"
dist_root="$repo_root/build/zip_root"

CHKDSK_BUILD_LOG="${CHKDSK_BUILD_LOG:-0}"
CHKDSK_BUILD_VERSION="${CHKDSK_BUILD_VERSION:-}"
CHKDSK_USE_LFN="${CHKDSK_USE_LFN:-0}"

mkdir -p "$repo_root/build"
mkdir -p "$repo_root/dist"
rm -f "$image_path"
rm -f "$dist_zip_path"
rm -rf "$dist_root"

echo "Building chkdsk (CHKDISK_USE_LFN=$CHKDSK_USE_LFN)..."
make -C "$repo_root" clean
if [ -n "$CHKDSK_BUILD_VERSION" ]; then
  make -C "$repo_root" \
    LOG="$CHKDSK_BUILD_LOG" \
    VERSION="$CHKDSK_BUILD_VERSION" \
    CHKDISK_USE_LFN="$CHKDSK_USE_LFN"
else
  make -C "$repo_root" \
    LOG="$CHKDSK_BUILD_LOG" \
    CHKDISK_USE_LFN="$CHKDSK_USE_LFN"
fi

if [ ! -f "$repo_root/chkdsk.exe" ]; then
  echo "Error: chkdsk.exe was not produced by the build" >&2
  exit 1
fi

echo "Creating FAT12 floppy image..."
mformat -C -i "$image_path" -f 1440 ::

mkdir_img_dir() {
  mmd -i "$image_path" "$1" 2>/dev/null || true
}

mkdir_img_dir "::/CHKDSK"
mcopy -i "$image_path" -o "$repo_root/chkdsk.exe" "::/CHKDSK/CHKDSK.EXE"

if [ -f "$repo_root/chkdsk.txt" ]; then
  mcopy -i "$image_path" -o "$repo_root/chkdsk.txt" "::/CHKDSK/CHKDSK.TXT"
fi

mkdir -p "$dist_root/CHKDSK"
cp "$repo_root/chkdsk.exe" "$dist_root/CHKDSK/CHKDSK.EXE"
if [ -f "$repo_root/chkdsk.txt" ]; then
  cp "$repo_root/chkdsk.txt" "$dist_root/CHKDSK/CHKDSK.TXT"
fi

if [ -f "$repo_root/README.md" ]; then
  mcopy -i "$image_path" -o "$repo_root/README.md" "::/README.MD"
  cp "$repo_root/README.md" "$dist_root/README.MD"
fi

echo "Creating ZIP package (store mode)..."
(
  cd "$dist_root"
  zip -r -0 "$dist_zip_path" .
)

echo "Created FAT12 floppy image: $image_path"
echo "Created ZIP package: $dist_zip_path"
