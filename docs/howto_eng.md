CHKDSK for ZX Sprinter / Estex DSS
==================================

(c) 2026 Dmitry Mikhalchenkov, Sprinter Team. Licensed under GPLv3.

A FAT filesystem checker and repair tool for floppies, IDE / CF / SD
partitions, and similar media accessible through the DSS BIOS.


WHICH BINARY TO RUN
-------------------

Two builds ship together; pick the one that matches your target volume:

  CHKDSK.EXE     FAT16 + FAT32. Use for IDE/CF/SD partitions and any
                 medium larger than ~16 MB.

  CHKDSK12.EXE   FAT12 only. Use for floppies and very small media
                 (typically under ~16 MB).

If you run the wrong binary you will get an "unsupported FAT type"
diagnostic in Phase 1 and the program exits.


COMMAND LINE
------------

  CHKDSK <drive>: [/F] [/C] [/V]
  CHKDSK12 <drive>: [/F] [/V]

Drive letter is required and must be followed by a colon, e.g. C:, D:,
A:. Letters are case-insensitive.

  /F     Apply repairs. Without /F the program is read-only and only
         reports issues.

  /C     With /F: when lost clusters are found, link each orphan chain
         into a new FILE####.CHK entry in the root directory instead of
         freeing the clusters silently. Useful for data recovery. Has
         no effect without /F.

  /V     Verbose progress: emit a dot per chunk during the long-running
         passes (Phase 2 FAT scan, Phase 3 directory walk, Phase 4
         sweep) so you can tell the program is working on a multi-
         second scan. Cosmetic only.

  /?     Help.


WHAT EACH PHASE DOES
--------------------

  Phase 1 -- boot sector and BPB
    Validates the boot signature (0xAA55), sector and cluster sizes,
    FAT count, root-dir entry count. Classifies the volume as FAT12,
    FAT16 or FAT32 by cluster count.

  Phase 2 -- FAT tables
    Walks every FAT entry. Each entry is classified as free, in-use,
    BAD, end-of-chain, or invalid. If the volume has two FAT copies,
    Phase 2 also detects mismatching sectors. The FAT[0] media
    descriptor is cross-checked against the BPB; FAT[1] is checked for
    a valid EOC marker, and on FAT16/32 also for the clean-shutdown
    and hard-error status bits.

  Phase 3 -- directory walk and chain check
    Recursively walks every directory starting at the root. Each entry
    is validated:

      * SFN character set, lowercase, leading-space, attribute bits,
        directory-vs-file consistency, first-cluster bounds, FAT12/16
        high-cluster word, dir-size requirement.
      * "." / ".." cluster pointers (must equal own / parent cluster).
      * LFN slots must be in descending order, all carry the same
        checksum byte, and the SFN that follows must reproduce that
        checksum.
      * Cluster chain: cycles, cross-links, broken / BAD links,
        truncated chain (shorter than file size says), excess chain
        (longer than file size says).

    Output is silent for clean entries; the trailing Totals line gives
    the population counts.

  Phase 4 -- lost cluster sweep
    Every FAT entry that is in-use but not reachable from any
    directory is identified as a lost cluster. Reported by count;
    repaired with /F or /F /C.


REPAIRS
-------

When run with /F, the program will fix the following classes of
issues. Each successful repair bumps the "applied" counter shown in
the trailing summary; the "found" counter bumps once per logical
issue noticed regardless of /F.

  FAT 1/2 mismatch
    FAT 1 is copied over FAT 2 sector-by-sector.

  Directory entry with non-zero size on ATTR_DIR
    If the entry's first cluster looks like a real subdir (its first
    32-byte slot starts with the "." entry), only the size DWORD is
    zeroed. Otherwise the entry is marked deleted (0xE5) -- it was
    almost certainly a corrupted file whose attribute byte got
    flipped to include ATTR_DIR.

  Bad "." / ".." cluster pointers
    The cluster fields of the dot entry are rewritten to the correct
    value (own first cluster for ".", parent's first cluster for
    "..", or 0 if the parent is the root).

  Truncated / broken / cross-linked file chains
    The file's size DWORD is shrunk to match the actual chain
    coverage so the user-visible content stops at safe data.

  Excess-length chains (chain longer than file size)
    EOC is written at the expected last cluster, and the trailing
    chain (now orphaned) is freed.

  Lost clusters
    Default: cleared in the FAT (set to 0 = free). With /C: each
    chain is linked into a FILE####.CHK entry in the root directory
    so its content can be recovered.


WHAT IS NOT REPAIRED
--------------------

  * Bad blocks at the medium level. The program does not perform a
    surface scan and does not mark unreadable clusters as BAD. Use
    your hardware's diagnostics if you suspect physical damage.

  * Cross-linked clusters are reported and the affected file's size
    is shrunk, but the program does not split chains into separate
    copies. The first file the walker encountered keeps the cluster.

  * LFN sequence breakage is reported but not auto-repaired -- you
    can delete the file from a host system and restore from backup,
    or run a more capable tool.


OUTPUT EXAMPLE
--------------

  checkdsk 0.1.20260501 for Sprinter DSS
  Mode: read-only
  Phase 1: boot sector and BPB
    Type: FAT16, clusters: 32761
    No issues
  Volume C: (FAT16)
  Serial: 4414-6469
  Cluster size:  32768 bytes
  Total clusters: 32761
  Total bytes:   1073512448
  Phase 2: FAT tables
    FAT[0] media: 0xF8, FAT[1] OK
    free=26905 used=2994 invalid=0
    No issues
  Phase 3: directory and chain walk
    Totals: entries=2942 dirs=579
  Phase 4: lost clusters
    No issues

When something is wrong, flagged entries appear in their phase's
section, prefixed by their full path:

  /SOMEDIR/FILE.TXT * truncated
  /OTHER/SUB . * dot-clust got=12 expect=42


EXIT CODE
---------

  0 -- everything clean, no issues found.
  1 -- at least one issue was reported (and possibly repaired with /F).
  2 -- bad arguments or the drive could not be opened.


SAFETY NOTES
------------

  * Always run the read-only mode first to see what the tool wants to
    fix. Only re-run with /F once the report makes sense to you.

  * The repairs are irreversible. There is no undo. Make a backup of
    the volume's contents before running with /F if the data is
    valuable.

  * On removable media, do not unplug or reset during a /F run --
    interrupting a Phase 2 FAT 1/2 sync mid-stream can leave the FAT
    in a worse state than before.

  * If Phase 4 reports many lost clusters, prefer /F /C over plain
    /F: the FILE####.CHK entries let you inspect what was orphaned
    before deciding to free the space.
