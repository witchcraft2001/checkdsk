/*
 * volume.c -- drive-letter resolver. See volume.h for the mapping
 * table and out-of-scope notes.
 */

#include "ff.h"
#include "volume.h"
#include "diskio_dss.h"

/* FatFs requires this symbol when FF_MULTI_PARTITION = 1. We update
 * VolToPart[0] at runtime from volume_apply() before calling f_mount. */
PARTITION VolToPart[FF_VOLUMES] = {
    { 0u, 0u }
};

int volume_resolve(char letter, volume_t *out)
{
    /* Normalise to uppercase. */
    if (letter >= 'a' && letter <= 'z') {
        letter = (char)(letter - ('a' - 'A'));
    }
    if (letter < 'A' || letter > 'Z') {
        return VOL_ERR_BAD_LETTER;
    }

    switch (letter) {
    case 'A':
        out->disk = 0x00u;
        out->desc = 0xC1E0u;
        out->part = 0u;     /* floppy: no MBR */
        return VOL_OK;
    case 'B':
        out->disk = 0x01u;
        out->desc = 0xC1E8u;
        out->part = 0u;
        return VOL_OK;
    case 'C': case 'D': case 'E': case 'F':
        out->disk = 0x80u;
        out->desc = 0xC1C0u;
        out->part = (u8)(letter - 'C' + 1);   /* C=1, D=2, E=3, F=4 */
        return VOL_OK;
    case 'G': case 'H': case 'I': case 'J':
        out->disk = 0x81u;
        out->desc = 0xC1C8u;
        out->part = (u8)(letter - 'G' + 1);
        return VOL_OK;
    default:
        /* K..Z: requires CMOS #1E parsing or DSS LOGDRV access -- not
         * supported on stage 0. See specs.md "Drive letter resolution". */
        return VOL_ERR_UNSUPPORTED;
    }
}

void volume_apply(const volume_t *vol)
{
    diskio_dss_set_device(vol->disk, vol->desc);
    VolToPart[0].pd = 0u;          /* FatFs pdrv 0 -- our diskio glue */
    VolToPart[0].pt = vol->part;
}
