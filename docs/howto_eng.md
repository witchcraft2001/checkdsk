CHKDSK for ZX Sprinter / Estex DSS
==================================

(c) 2026 Dmitry Mikhalchenkov, Sprinter Team. Licensed under GPLv3.

A FAT filesystem checker and repair tool for floppies, IDE / CF / SD
partitions, and similar media accessible through the DSS BIOS.


WHICH BINARY TO RUN
-------------------

A single binary, CHKDSK.EXE, covers FAT12, FAT16 and FAT32 for
floppies and IDE / CF / SD partitions alike. The FAT type is detected
at runtime from the BPB -- there is nothing to choose. (Earlier
releases shipped a separate CHKDSK12.EXE for FAT12; that split is gone.)


COMMAND LINE
------------

  CHKDSK <drive>: [/F] [/C] [/V] [/Y]

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

  /Y     With /F: assume "yes" on the one-time write warning (see
         SAFETY NOTES) and start repairing without waiting for a
         keypress. Meant for unattended or batch use. No effect
         without /F.

  /?     Help.

You can interrupt a long scan at any time by pressing ESC or Ctrl+C;
the program stops at the next check point, leaving already-written
repairs intact.


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
    and hard-error status bits. On FAT32 the free-cluster count cached
    in FSInfo is compared against the count taken from the FAT itself;
    a disagreement is reported and, under /F, FSInfo is marked for
    recalculation. A cached value of "unknown" is legitimate and is not
    reported.

  Phase 3 -- directory walk and chain check
    Recursively walks every directory starting at the root. Each entry
    is validated:

      * SFN character set, lowercase, leading-space, attribute bits,
        directory-vs-file consistency, first-cluster bounds, FAT12/16
        high-cluster word, dir-size requirement.
      * Date and time fields: month, day (leap years included), hour,
        minute and second ranges on the creation, write and last-access
        stamps. An all-zero date means "not set" and is accepted.
      * "." / ".." cluster pointers (must equal own / parent cluster).
      * Current-format LFN (long name) groups: slot order and bounds,
        NUL / 0xFFFF padding, one consistent checksum across the group,
        that checksum matching the short name behind it, forbidden
        UCS-2 characters, required-zero fields, and groups left
        orphaned with no short name. Reserved future LFN dirent types
        are preserved and ignored. The names themselves are never
        decoded, so paths always print the 8.3 name.
      * Cluster chain: cycles, cross-links, broken / BAD links,
        truncated chain (shorter than file size says), excess chain
        (longer than file size says).

    Output is silent for clean entries; the trailing Totals line gives
    the population counts.

  Phase 4 -- lost cluster sweep
    Every FAT entry that is in-use but not reachable from any
    directory is identified as a lost cluster. Reported by count;
    repaired with /F or /F /C.

After Phase 4 the program prints a classic chkdsk-style space report:
total disk space, bytes in user files and in directories, bytes in bad
sectors, bytes free, and the allocation-unit geometry. Only the volume
serial number is shown, not the label -- DSS does not expose the label
through the mount structure.


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

  Out-of-range date / time fields
    Each offending field is reset to the FAT epoch, 1980-01-01
    00:00:00. Only the fields that actually failed validation are
    rewritten, so a sound creation date survives a corrupt
    last-access date.

  FSInfo free-cluster count out of step with the FAT (FAT32)
    The cached count and next-free hint are marked "unknown" so DSS
    recalculates them on the next mount.

  Invalid SFN characters, lowercase letters, leading space
    The 8.3 name is sanitized in place: forbidden bytes become '_',
    lowercase ASCII and lowercase CP866 Cyrillic are folded to upper
    case (the latter matters because DSS itself cannot open a file
    whose stored name holds raw lowercase Cyrillic). If a valid long
    name sits in front of the entry, its checksum is restamped for the
    new short name, so the long name survives the rename. The fix is
    skipped -- and the reason printed -- if the cleaned name would
    collide with another entry in the same directory, or if an I/O
    error prevents the update. Groups crossing sector or directory-
    cluster boundaries are supported.

  Broken long-name (LFN) groups
    A group whose slot order, padding, checksum or characters are
    corrupt is deleted outright, including across sector or directory-
    cluster boundaries. The long name is lost, but the 8.3 name behind
    it stays intact and the file remains fully accessible. An orphaned
    group with no short name behind it is deleted as well.


A REPAIR RUN MAY NEED A SECOND PASS
-----------------------------------

  Some repairs create findings the same run cannot report. /C writes
  FILE####.CHK entries after the directory walk is over; truncating an
  over-long chain orphans its tail; and an entry deleted during the
  directory walk leaves a cluster chain that the lost-cluster sweep of
  that same run still sees as in use. None of this is damage -- it is
  the consequence of the repair that was just applied.

  That is what the closing "Re-run chkdsk" line means. Keep running
  with /F until a pass reports no fixes; each pass strictly reduces
  what is left, so this converges quickly (two or three passes at
  worst). A final read-only run exiting with code 0 is the proof that
  the volume is clean.


WHAT IS NOT REPAIRED
--------------------

  * Bad blocks at the medium level. The program does not perform a
    surface scan and does not mark unreadable clusters as BAD. Use
    your hardware's diagnostics if you suspect physical damage.

  * Cross-linked clusters are reported and the affected file's size
    is shrunk, but the program does not split chains into separate
    copies. The first file the walker encountered keeps the cluster.

  * Long file names are never decoded or rebuilt. A corrupt long name
    is dropped, not reconstructed -- recreate it from a host system if
    you need it. The 8.3 name always stays usable.


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

  Volume Serial Number is 4414-6469

    1073512448 bytes total disk space
     172916736 bytes in 2363 user files
      18972672 bytes in 579 directories
             0 bytes in bad sectors
     881623040 bytes available on disk

         32768 bytes in each allocation unit
         32761 total allocation units on disk
         26905 available allocation units on disk

When something is wrong, flagged entries appear in their phase's
section, prefixed by their full path:

  /SOMEDIR/FILE.TXT * truncated
  /OTHER/SUB . * dot-clust got=12 expect=42


EXIT CODES
----------

  0   -- volume is clean.
  1   -- issues found, not fixed (read-only run, i.e. without /F).
  2   -- issues found, all fixed (/F run).
  3   -- issues found, some left unfixed (/F run) -- see the WARN
         lines for why (name collision or an I/O failure).
  255 -- fatal: the volume could not be checked at all (bad drive,
         unreadable boot sector or BPB, out of page memory, an I/O
         error mid-scan).

DSS's `IF ERRORLEVEL n` is true when the code is >= n (as in MS-DOS),
so a batch script must test from the highest code down:

  CHKDSK C: /F /Y
  IF ERRORLEVEL 255 GOTO Fatal
  IF ERRORLEVEL 3   GOTO Partial
  IF ERRORLEVEL 2   GOTO AllFixed
  IF ERRORLEVEL 1   GOTO FoundUnfixed
  GOTO Clean


SAFETY NOTES
------------

  * Always run the read-only mode first to see what the tool wants to
    fix. Only re-run with /F once the report makes sense to you.

  * Before the first write, a /F run prints
      WARNING: about to write to disk. Press Y to continue, any other
      key to abort.
    Any key other than Y cancels the rest of the run -- nothing more
    is written and the exit code reads as a read-only run. Pass /Y to
    skip this prompt for unattended use.

  * The repairs are irreversible. There is no undo. Make a backup of
    the volume's contents before running with /F if the data is
    valuable.

  * On removable media, do not unplug or reset during a /F run --
    interrupting a Phase 2 FAT 1/2 sync mid-stream can leave the FAT
    in a worse state than before.

  * If Phase 4 reports many lost clusters, prefer /F /C over plain
    /F: the FILE####.CHK entries let you inspect what was orphaned
    before deciding to free the space.
