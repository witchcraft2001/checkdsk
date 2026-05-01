# checkdsk — FAT filesystem checker for ZX Sprinter / Estex DSS

Copyright © 2026 Dmitry Mikhalchenkov, Sprinter Team. Licensed under GPLv3 (see [`LICENSE`](LICENSE)).

A native chkdsk utility for the ZX Sprinter computer (Z80, 7/21 MHz) running the Estex DSS operating system. Validates and repairs FAT12, FAT16 and FAT32 volumes mounted from floppies, IDE / CompactFlash / SD partitions, and similar block devices addressable through the DSS BIOS.

## What it checks

* **Phase 1 — boot sector + BPB**: signature, sector / cluster geometry, FAT type classification by cluster count.
* **Phase 2 — FAT tables**: every entry classified (free / in-use / BAD / EOC / invalid), FAT 1/2 mismatch detection, FAT[0] media descriptor cross-check, FAT[1] EOC + clean-shutdown / hard-error flags.
* **Phase 3 — directory tree**: full DFS walk of every directory; entries validated for character set, attributes, cluster bounds, FAT12/16 high-cluster word, dir-size, "." / ".." cluster pointers; cluster chains validated for cycles, cross-links, broken / BAD links, truncated and excess length; LFN sequences checked for descending order, agreed checksum byte, and SFN-checksum match.
* **Phase 4 — lost cluster sweep**: every FAT entry compared against the in-use bitmap built during Phase 3.

## What it repairs (with `/F`)

* FAT 1/2 mismatch — copy FAT 1 over FAT 2.
* Corrupted directory entries — non-zero size on a directory: zero the size if the cluster looks like a real subdir, otherwise mark the entry deleted.
* Bad "." / ".." cluster pointers — write the correct cluster.
* Truncated / broken / cross-linked file chains — shrink the file size to the actual chain coverage.
* Excess-length chains — write EOC at the expected last cluster, free the trailing chain.
* Lost clusters — free them, or with `/F /C` link each chain into a `FILE####.CHK` entry in the root for manual recovery.

## Build targets

The `Makefile` builds two binaries from the same source tree, each tuned to fit the Sprinter's 32 KB code+data window:

* **`chkdsk.exe`**  — FAT16 + FAT32, for IDE / CF / SD partitions.
* **`chkdsk12.exe`** — FAT12 only, for floppies and very small media.

Run the appropriate binary for the target volume; both share the same command-line and behave identically on the FAT types they support.

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

* `chkdsk.exe` — FAT16/32 binary, copy to the Sprinter and run as `CHKDSK <drive>:`.
* `chkdsk12.exe` — FAT12 binary, copy to the Sprinter and run as `CHKDSK12 <drive>:`.

For a packaged release run `run/create_floppy_image.sh`, which produces:

* `build/chkdsk.img` — bootable 1.44 MB FAT12 floppy image with both binaries and the user guides.
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
