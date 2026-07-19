# checkdsk — FAT filesystem checker for ZX Sprinter / Estex DSS

Copyright © 2026 Dmitry Mikhalchenkov, Sprinter Team. Licensed under GPLv3 (see [`LICENSE`](LICENSE)).

A native chkdsk utility for the ZX Sprinter computer (Z80, 7/21 MHz) running the Estex DSS operating system. Validates and repairs FAT12, FAT16 and FAT32 volumes mounted from floppies, IDE / CompactFlash / SD partitions, and similar block devices addressable through the DSS BIOS.

## What it checks

* **Phase 1 — boot sector + BPB**: signature, sector / cluster geometry, FAT type classification by cluster count.
* **Phase 2 — FAT tables**: every entry classified (free / in-use / BAD / EOC / invalid), FAT 1/2 mismatch detection, FAT[0] media descriptor cross-check, FAT[1] EOC + clean-shutdown / hard-error flags. On FAT32 the free-cluster count cached in FSInfo is cross-checked against the count taken from the FAT itself; a cached "unknown" is legitimate and is not reported.
* **Phase 3 — directory tree**: full DFS walk of every directory; entries validated for character set (including CP866 case), attributes, cluster bounds, FAT12/16 high-cluster word, dir-size, date/time field ranges (leap years included; an all-zero date means "not set"), "." / ".." cluster pointers; cluster chains validated for cycles, cross-links, broken / BAD links, truncated and excess length. Current-format LFN groups are fully validated: slot order and bounds, NUL / 0xFFFF padding, a single consistent checksum across the group, that checksum matching the SFN it precedes, forbidden UCS-2 characters, required-zero fields, and groups left orphaned by a missing SFN. Reserved future LFN dirent types are preserved and ignored. Long names are validated but never decoded, so paths always print the 8.3 name.
* **Phase 4 — lost cluster sweep**: every FAT entry compared against the in-use bitmap built during Phase 3.

At the end of a run it prints a classic chkdsk-style space report: total disk space, bytes in user files and directories, bytes in bad sectors, bytes free, and the allocation-unit geometry. (The FAT volume label isn't shown — only the serial number — because DSS doesn't expose it through the mount structure.)

## What it repairs (with `/F`)

* FAT 1/2 mismatch — copy FAT 1 over FAT 2.
* Corrupted directory entries — non-zero size on a directory: zero the size if the cluster looks like a real subdir, otherwise mark the entry deleted.
* Bad "." / ".." cluster pointers — write the correct cluster.
* Truncated / broken / cross-linked file chains — shrink the file size to the actual chain coverage.
* Excess-length chains — write EOC at the expected last cluster, free the trailing chain.
* Lost clusters — free them, or with `/F /C` link each chain into a `FILE####.CHK` entry in the root for manual recovery.
* Invalid SFN characters, lowercase letters (including CP866 lowercase Cyrillic, which Estex-DSS itself cannot address by name -- see below) and a leading space — sanitized in place. If a valid LFN group sits in front of the entry, its checksum is restamped for the new name so the long name survives the rename, including across sector or directory-cluster boundaries. Skipped (left flagged) only if the sanitized name would collide with an existing entry in the same directory or an I/O error prevents the update.
* Broken or orphaned LFN groups (bad slot order, padding, checksum or forbidden characters; missing SFN) — deleted across sector/cluster boundaries, leaving an associated 8.3 name intact and usable.
* Out-of-range date/time fields — reset to the FAT epoch `1980-01-01 00:00:00`. Only the fields that actually failed validation are rewritten, so a sound creation date survives a corrupt last-access date.
* FSInfo free-cluster count out of step with the FAT (FAT32) — the cached count and next-free hint are marked "unknown" so DSS recalculates them on the next mount.

Before the first sector actually gets written, `/F` shows `WARNING: about to write to disk. Press Y to continue, any other key to abort.` Any answer other than `Y` cancels for the rest of the run -- everything already found still gets reported, nothing more gets written, and the exit code reads as if `/F` had not been given at all. Pass `/Y` to assume yes and skip the prompt (for unattended/batch use).

## Exit codes

| Code | Meaning |
|------|---------|
| `0`   | Volume is clean. |
| `1`   | Issues found, not fixed (read-only run, i.e. without `/F`). |
| `2`   | Issues found, all fixed (`/F` run). |
| `3`   | Issues found, some left unfixed (`/F` run) -- see the WARN lines for why (name collision or an I/O failure). |
| `255` | Fatal: couldn't check the volume at all (bad drive, unreadable/unsupported boot sector or BPB, out of page memory, I/O error mid-scan). |

DSS's `IF ERRORLEVEL n` is true when the actual exit code is `>= n` (same convention as MS-DOS), so a script must test from the **highest** code down -- a single `IF ERRORLEVEL 1` cannot tell "all fixed" (2) apart from "still broken" (1/3/255), because all four are `>= 1`:

```
CHKDSK C: /F
IF ERRORLEVEL 255 GOTO Fatal
IF ERRORLEVEL 3   GOTO Partial
IF ERRORLEVEL 2   GOTO AllFixed
IF ERRORLEVEL 1   GOTO FoundUnfixed
GOTO Clean
```

## Build target

The `Makefile` builds a single binary, **`chkdsk.exe`**, covering FAT12, FAT16 and FAT32 for floppies and IDE / CF / SD partitions alike. The FAT type is detected at runtime from the BPB.

The binary uses the SDK's extended WIN0..WIN2 memory layout (code-biased): code and rodata span all three low windows (~44 KB budget), while the RST trampolines, data and stack sit at the top of WIN2. This lifts the old 32 KB code+data ceiling that had previously forced a split into separate FAT12 and FAT16/32 binaries.

## Building from source

### Prerequisites

* **SDCC 4.5.x** (z80 backend). Older 2.9.x is no longer supported.
* **Python 3** (used by the SDK's `ihx2exe.py` packager).
* **GNU make**.
* **mtools** (`mformat`, `mcopy`, `mmd`) — only if you build the floppy image.
* **iconv** and **zip** — only for the dist packager.
* **A built copy of [`sdcc-sprinter-sdk`](https://github.com/witchcraft2001/sdcc-sprinter-sdk), branch `sdcc450`**, sitting next to this repo as `../sdcc45-sprinter-sdk/`. The `sdcc450` branch is the one matched to the SDCC 4.5.0 toolchain. The default `master` branch targets the older SDCC 2.9.0 stack and will not link against this codebase.

### macOS

```sh
brew install sdcc python3 make mtools
git clone -b sdcc450 https://github.com/witchcraft2001/sdcc-sprinter-sdk.git ../sdcc45-sprinter-sdk
make -C ../sdcc45-sprinter-sdk
make
```

### Linux (Debian/Ubuntu)

```sh
sudo apt install sdcc python3 make mtools zip libc-bin
git clone -b sdcc450 https://github.com/witchcraft2001/sdcc-sprinter-sdk.git ../sdcc45-sprinter-sdk
make -C ../sdcc45-sprinter-sdk
make
```

### Windows (MSYS2 / MinGW)

```sh
pacman -S mingw-w64-x86_64-sdcc mingw-w64-x86_64-python make mtools zip
git clone -b sdcc450 https://github.com/witchcraft2001/sdcc-sprinter-sdk.git ../sdcc45-sprinter-sdk
make -C ../sdcc45-sprinter-sdk
make
```

A native Windows build without MSYS2 is possible but requires manually invoking `sdcc` and `ihx2exe.py` for each source file; the Makefile assumes a POSIX shell.

### Output

After `make`:

* `chkdsk.exe` — the single FAT12/16/32 binary, copy to the Sprinter and run as `CHKDSK <drive>:`.

For a packaged release run `run/create_floppy_image.sh`, which produces:

* `build/chkdsk.img` — bootable 1.44 MB FAT12 floppy image with the binary and the user guides.
* `dist/checkdsk.zip` — the same payload as a flat archive.

Local-machine overrides (e.g. an `SDCC_BIN_DIR` pointing at a non-default install) belong in an untracked `run/create_floppy_image.local.sh` wrapper that exec's the main script with environment set; see the file in this repo for an example pattern.

## Documentation

Detailed user guide:

* English — [`docs/howto_eng.md`](docs/howto_eng.md)
* Russian — [`docs/howto_ru.md`](docs/howto_ru.md)

Both ship with the floppy image and zip release as plain-text `README.ENG` (ASCII) and `README.TXT` (CP866 for native DOS / Sprinter rendering).

## License

GPLv3. See [`LICENSE`](LICENSE) for the full text.

The project links against the SDCC z80 runtime, which is GPL-licensed.
