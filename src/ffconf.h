/*---------------------------------------------------------------------------/
/  FatFs Configuration for CHKDSK / Sprinter DSS
/---------------------------------------------------------------------------/
/
/  Based on FatFs R0.16 (elm-chan.org) ffconf.h template.
/  Values fixed per checkdsk specs.md, sections "Configuration of FatFs"
/  and "Volume label resolution".
/
/  CHKDISK_USE_LFN is propagated by the top-level Makefile via SDCC_FLAGS
/  (-DCHKDISK_USE_LFN=0|1). Default is 0 (no LFN).
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* Revision ID -- must match ff.h */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#ifndef CHKDSK_FF_READONLY
#define CHKDSK_FF_READONLY 1
#endif
#define FF_FS_READONLY	CHKDSK_FF_READONLY
/* Stage 0..4 build read-only to keep ff.c code under the WIN1 budget;
 * stage 5 will pass -DCHKDSK_FF_READONLY=0 to enable f_write / f_sync /
 * f_unlink / f_truncate. NOTE: read-only mode also removes f_getfree,
 * so the summary prints total bytes only (no free-bytes line). */

#define FF_FS_MINIMIZE	0
/* All API functions enabled. */

#define FF_USE_FIND	0
/* CHKDSK walks directories at sector level itself; no need for f_findfirst. */

#define FF_USE_MKFS	0
/* No formatting from CHKDSK. */

#define FF_USE_FASTSEEK	0
/* CHKDSK does not use seek-heavy patterns; saves code size. */

#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	1
/* Need f_getlabel for the volume summary printed in stage 0. */

#define FF_USE_FORWARD	0

#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0
/* All output is done by checkdsk's own printer. */


/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	866
/* Sprinter DSS console and FAT OEM names use CP866 (IBM PC Cyrillic).
/  See sprinter_ai_doc/manual/05_graphics/08_text_mode.md. */


#ifndef CHKDISK_USE_LFN
#define CHKDISK_USE_LFN 0
#endif

#if CHKDISK_USE_LFN
#define FF_USE_LFN	2	/* Dynamic working buffer on the stack. */
#else
#define FF_USE_LFN	0	/* No LFN; stage 0 default. */
#endif

#define FF_MAX_LFN	64
/* Reduced from default 255 to save memory; fits typical Sprinter use. */

#define FF_LFN_UNICODE	0
/* OEM (CP866) on the API; raw FAT bytes pass through unchanged. */

#define FF_LFN_BUF	64
#define FF_SFN_BUF	12

#define FF_FS_RPATH	0
/* No relative paths; checkdsk takes a single absolute drive argument. */

#define FF_PATH_DEPTH	10
/* exFAT-only; harmless when FF_FS_EXFAT == 0. */


/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES	1
/* One volume per checkdsk run. */

#define FF_STR_VOLUME_ID	0

#define FF_MULTI_PARTITION	1
/* volume.c populates VolToPart[0] = { pdrv=0, partition_num } at runtime
/  so f_mount selects the right MBR partition for the requested drive
/  letter. See specs.md / Stage 0 / "Drive letter resolution". */

#define FF_MIN_SS	512
#define FF_MAX_SS	512
/* Sprinter sector size is fixed at 512 bytes (Estex-DSS/ATA_DRV.asm:134). */

#define FF_LBA64	0
/* No 64-bit LBA on Sprinter targets. */

#define FF_MIN_GPT	0x10000000
/* Has no effect when FF_LBA64 == 0. */

#define FF_USE_TRIM	0


/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY	0
/* Per-FIL sector buffer kept; checkdsk opens at most one FIL at a time. */

#define FF_FS_EXFAT	0
/* exFAT explicitly out of scope per specs.md "Explicitly out of scope". */

#define FF_FS_NORTC	0
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2025
/* Sprinter has CMOS RTC. get_fattime() in diskio_dss.c reads it via
/  dss_gettime / dss_getdate (sdcc-sprinter-sdk/include/sprinter/dss.h:282). */

#define FF_FS_CRTIME	0
/* Creation timestamp not surfaced; modification time is enough for the
/  summary. Stage 3 may revisit when validating dir-entry timestamps. */

#define FF_FS_NOFSINFO	0
/* Trust FSINFO on FAT32 by default. Stage 1 will revalidate FSINFO
/  fields explicitly; here we only need the mount summary. */

#define FF_FS_LOCK	0
/* Single-process utility; no concurrent file lock control needed. */

#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
