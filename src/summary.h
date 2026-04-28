/*
 * summary.h -- printer for the volume summary on stage 0.
 *
 * Writes the canonical chkdsk-style header to stdout via printf:
 *   - volume label and serial number
 *   - filesystem type (FAT12/16/32)
 *   - cluster size, total clusters, free clusters
 *   - total bytes, free bytes
 *
 * Stage 0 stops after this line; integrity checks live in stages 1+.
 */

#ifndef CHKDSK_SUMMARY_H
#define CHKDSK_SUMMARY_H

#include "ff.h"

/* Print the summary for a mounted volume. Returns 0 on success,
 * non-zero on FatFs error (the FRESULT is reported in the message). */
int summary_print(FATFS *fs, char drive_letter);

#endif /* CHKDSK_SUMMARY_H */
