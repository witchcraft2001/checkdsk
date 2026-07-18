# checkdsk — FAT filesystem checker for ZX Sprinter / Estex DSS

Copyright © 2026 Dmitry Mikhalchenkov, Sprinter Team. Licensed under GPLv3 (see [`LICENSE`](LICENSE)).

A native chkdsk utility for the ZX Sprinter computer (Z80, 7/21 MHz) running the Estex DSS operating system. Validates and repairs FAT12, FAT16 and FAT32 volumes mounted from floppies, IDE / CompactFlash / SD partitions, and similar block devices addressable through the DSS BIOS.

## What it checks

* **Phase 1 — boot sector + BPB**: signature, sector / cluster geometry, FAT type classification by cluster count.
* **Phase 2 — FAT tables**: every entry classified (free / in-use / BAD / EOC / invalid), FAT 1/2 mismatch detection, FAT[0] media descriptor cross-check, FAT[1] EOC + clean-shutdown / hard-error flags.
* **Phase 3 — directory tree**: full DFS walk of every directory; entries validated for character set (including CP866 case), attributes, cluster bounds, FAT12/16 high-cluster word, dir-size, "." / ".." cluster pointers; cluster chains validated for cycles, cross-links, broken / BAD links, truncated and excess length. LFN slots are checked for internal consistency (reserved fields) only -- sequence order and SFN-checksum cross-validation were dropped to fit the memory budget; long names are always treated as opaque, never decoded or displayed.
* **Phase 4 — lost cluster sweep**: every FAT entry compared against the in-use bitmap built during Phase 3.

At the end of a run it prints a classic chkdsk-style space report: total disk space, bytes in user files and directories, bytes in bad sectors, bytes free, and the allocation-unit geometry. (The FAT volume label isn't shown — only the serial number — because DSS doesn't expose it through the mount structure.)

## What it repairs (with `/F`)

* FAT 1/2 mismatch — copy FAT 1 over FAT 2.
* Corrupted directory entries — non-zero size on a directory: zero the size if the cluster looks like a real subdir, otherwise mark the entry deleted.
* Bad "." / ".." cluster pointers — write the correct cluster.
* Truncated / broken / cross-linked file chains — shrink the file size to the actual chain coverage.
* Excess-length chains — write EOC at the expected last cluster, free the trailing chain.
* Lost clusters — free them, or with `/F /C` link each chain into a `FILE####.CHK` entry in the root for manual recovery.
* Invalid SFN characters, lowercase letters (including CP866 lowercase Cyrillic, which Estex-DSS itself cannot address by name -- see below) and a leading space — sanitized in place. Skipped (left flagged) if the sanitized name would collide with an existing entry in the same directory, or if an LFN run in front of the entry can't be resolved safely; either case prints why.

Before the first sector actually gets written, `/F` shows `WARNING: about to write to disk. Press Y to continue, any other key to abort.` Any answer other than `Y` cancels for the rest of the run -- everything already found still gets reported, nothing more gets written, and the exit code reads as if `/F` had not been given at all. Pass `/Y` to assume yes and skip the prompt (for unattended/batch use).

## Exit codes

| Code | Meaning |
|------|---------|
| `0`   | Volume is clean. |
| `1`   | Issues found, not fixed (read-only run, i.e. without `/F`). |
| `2`   | Issues found, all fixed (`/F` run). |
| `3`   | Issues found, some left unfixed (`/F` run) -- see the WARN lines for why (name collision, LFN spanning a sector, a write that failed). |
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
