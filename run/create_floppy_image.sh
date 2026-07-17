#!/usr/bin/env bash
#
# Build chkdsk.exe, then pack it together with the user guides into a
# FAT12 floppy image and a distribution zip.
#
# Usage: create_floppy_image.sh [image_path] [dist_zip_path]
#
# Defaults (computed from script location):
#   image_path     = $repo_root/build/chkdsk.img
#   dist_zip_path  = $repo_root/dist/checkdsk.zip
#
# Local-machine overrides (alternative SDCC, custom toolchain paths,
# etc.) belong in an untracked `create_floppy_image.local.sh` wrapper
# next to this file that exec's this script with the desired env set.
# See CLAUDE.md "Scripts and build packaging".
#
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
need_cmd iconv

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

image_path="${1:-$repo_root/build/chkdsk.img}"
dist_zip_path="${2:-$repo_root/dist/checkdsk.zip}"
dist_root="$repo_root/build/zip_root"

CHKDSK_BUILD_LOG="${CHKDSK_BUILD_LOG:-0}"
CHKDSK_BUILD_VERSION="${CHKDSK_BUILD_VERSION:-}"

mkdir -p "$repo_root/build"
mkdir -p "$repo_root/dist"
rm -f "$image_path"
rm -f "$dist_zip_path"
rm -rf "$dist_root"

# ---- 1. Build the binary ----
echo "Building chkdsk.exe..."
make -C "$repo_root" clean
if [ -n "$CHKDSK_BUILD_VERSION" ]; then
  make -C "$repo_root" LOG="$CHKDSK_BUILD_LOG" VERSION="$CHKDSK_BUILD_VERSION"
else
  make -C "$repo_root" LOG="$CHKDSK_BUILD_LOG"
fi

if [ ! -f "$repo_root/chkdsk.exe" ]; then
  echo "Error: chkdsk.exe was not produced by the build" >&2
  exit 1
fi

# ---- 2. Convert markdown user guides to packagable plain text ----
#
# The .md sources stay markdown for the repo; the floppy image and zip
# get plain-text twins:
#   README.ENG -- ASCII (suitable for any DOS / Unix tooling).
#   README.TXT -- CP866 (the canonical 8-bit Russian encoding on
#                 Sprinter and DOS; mojibake-free in DSS shell).
# We do not strip markdown syntax: the howto files are written in a
# minimal style (== / -- underlines, indented bullets) that already
# reads as plain text.
readme_eng="$repo_root/build/README.ENG"
readme_txt="$repo_root/build/README.TXT"
cp "$repo_root/docs/howto_eng.md" "$readme_eng"
iconv -f UTF-8 -t CP866 "$repo_root/docs/howto_ru.md" > "$readme_txt"

# ---- 3. Build the floppy image ----
echo "Creating FAT12 floppy image..."
mformat -C -i "$image_path" -f 1440 ::

mkdir_img_dir() { mmd -i "$image_path" "$1" 2>/dev/null || true; }

mkdir_img_dir "::/CHKDSK"
mcopy -i "$image_path" -o "$repo_root/chkdsk.exe"   "::/CHKDSK/CHKDSK.EXE"
mcopy -i "$image_path" -o "$readme_eng"             "::/CHKDSK/README.ENG"
mcopy -i "$image_path" -o "$readme_txt"             "::/CHKDSK/README.TXT"

# ---- 4. Build the dist zip (same payload, flat) ----
mkdir -p "$dist_root/CHKDSK"
cp "$repo_root/chkdsk.exe"   "$dist_root/CHKDSK/CHKDSK.EXE"
cp "$readme_eng"             "$dist_root/CHKDSK/README.ENG"
cp "$readme_txt"             "$dist_root/CHKDSK/README.TXT"

echo "Creating ZIP package (store mode)..."
(
  cd "$dist_root"
  zip -r -0 "$dist_zip_path" .
)

echo "Created FAT12 floppy image: $image_path"
echo "Created ZIP package: $dist_zip_path"
