/*
 * host_env.c -- host-side environment for the chkdsk core.
 *
 * Replaces the Sprinter-specific modules (diskio_dss.c, diskio_batch.c,
 * sectbuf.c, bitmap.c and the DSS syscalls) with plain-C equivalents
 * backed by a disk-image file loaded into memory. The checked core
 * (mount/bpb/fat/chain/dirwalk/dirent/scan/fix/summary/prt) compiles
 * unchanged against these.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "diskio_dss.h"
#include "diskio_batch.h"
#include "sectbuf.h"
#include "bitmap.h"

/* ---------------- image-backed disk ---------------- */

static u8      *g_img       = NULL;
static u32      g_img_secs  = 0;
static u32      g_part_off  = 0;
static char     g_img_path[1024];
static int      g_dirty     = 0;

int host_disk_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_img = malloc((size_t)sz);
    if (!g_img || fread(g_img, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cannot read %s\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    g_img_secs = (u32)(sz / 512);
    snprintf(g_img_path, sizeof g_img_path, "%s", path);
    g_dirty = 0;
    return 1;
}

u8 *host_image_data(u32 *out_secs)
{
    if (out_secs) *out_secs = g_img_secs;
    return g_img;
}

int host_disk_flush(void)
{
    FILE *f;
    if (!g_dirty) return 1;
    f = fopen(g_img_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", g_img_path); return 0; }
    if (fwrite(g_img, 512, g_img_secs, f) != g_img_secs) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv)     { (void)pdrv; return 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    (void)pdrv;
    if ((u32)sector + g_part_off + count > g_img_secs) return RES_ERROR;
    memcpy(buf, g_img + ((size_t)(sector + g_part_off) * 512), (size_t)count * 512);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    (void)pdrv;
    if ((u32)sector + g_part_off + count > g_img_secs) return RES_ERROR;
    memcpy(g_img + ((size_t)(sector + g_part_off) * 512), buf, (size_t)count * 512);
    g_dirty = 1;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_SIZE:  if (buff) *(WORD *)buff = 512u;              return RES_OK;
    case GET_SECTOR_COUNT: if (buff) *(LBA_t *)buff = g_img_secs;       return RES_OK;
    case GET_BLOCK_SIZE:   if (buff) *(DWORD *)buff = 1u;               return RES_OK;
    default:               return RES_PARERR;
    }
}

void diskio_dss_set_device(u8 disk_num, u16 desc_addr) { (void)disk_num; (void)desc_addr; }
void diskio_dss_set_partition_offset(unsigned long lba) { g_part_off = (u32)lba; }
u8   diskio_dss_last_error(void) { return 0u; }
void diskio_dss_rescan(void) {}

u8 diskio_dss_read_batch(unsigned long lba, u8 count, u8 *dst)
{
    return (disk_read(0, dst, (LBA_t)lba, count) == RES_OK) ? 0u : 1u;
}

/* ---------------- WIN3 arbiter ----------------
 *
 * On Sprinter, sectbuf, the batch reader and the cluster bitmap ALL live
 * in the single 16 KB window WIN3 (0xC000..0xFFFF), one page at a time.
 * With CHKDSK_WIN3_SHARE=1 the host models this faithfully: every mapper
 * returns the same 16 KB window pointer, and switching owners saves the
 * old page's bytes and loads the new page's -- exactly the hardware
 * behaviour. This is what surfaces WIN3 aliasing bugs (e.g. a scratch
 * buffer overlapping the batch page) that separate host buffers hide.
 *
 * Default off, so the fast matrix stays as-is; run_tests.sh flips it on
 * for a dedicated pass. */

#define WIN3_SZ 16384

static int  g_win3_share = -1;   /* -1 = not yet resolved */
static u8   g_win3[WIN3_SZ] __attribute__((aligned(16)));
static u8  *g_win3_owner_store = NULL;   /* backing store currently loaded */
static size_t g_win3_owner_len = 0;

static int win3_shared(void)
{
    if (g_win3_share < 0) g_win3_share = getenv("CHKDSK_WIN3_SHARE") ? 1 : 0;
    return g_win3_share;
}

/* Make `store` (of `len` bytes) the page visible in the window; returns
 * the window base. Saves the previous owner's bytes, loads the new. */
static u8 *win3_select(u8 *store, size_t len)
{
    if (!win3_shared()) return store;
    if (g_win3_owner_store == store) return g_win3;
    if (g_win3_owner_store)
        memcpy(g_win3_owner_store, g_win3, g_win3_owner_len);
    memcpy(g_win3, store, len);
    g_win3_owner_store = store;
    g_win3_owner_len = len;
    return g_win3;
}

/* Called before a backing store is freed: flush it back if it is the
 * live owner, then forget it so we never memcpy into freed memory. */
static void win3_forget(u8 *base, size_t span)
{
    if (!win3_shared() || !g_win3_owner_store) return;
    if (g_win3_owner_store >= base && g_win3_owner_store < base + span) {
        memcpy(g_win3_owner_store, g_win3, g_win3_owner_len);
        g_win3_owner_store = NULL;
        g_win3_owner_len = 0;
    }
}

/* ---------------- sectbuf ---------------- */

static u8 g_sectbuf[512] __attribute__((aligned(16)));

int  sectbuf_init(void)       { return 1; }
void sectbuf_release(void)    {}
u8  *sectbuf_acquire(void)    { return win3_select(g_sectbuf, sizeof g_sectbuf); }
void sectbuf_invalidate(void) {}

/* ---------------- bitmap ---------------- */

static u8 *g_bits   = NULL;
static u32 g_nbits  = 0;

int bitmap_init(u32 num_bits)
{
    /* Round the backing store up to whole WIN3 pages so win3_select can
     * map any page in full without reading past the allocation. */
    size_t bytes = (size_t)(num_bits + 7) / 8 + 1;
    size_t pages = (bytes + WIN3_SZ - 1) / WIN3_SZ;
    bitmap_release();
    g_bits = calloc(pages * WIN3_SZ, 1);
    if (!g_bits) return 0;
    g_nbits = num_bits;
    return 1;
}

void bitmap_release(void)
{
    if (g_bits) win3_forget(g_bits, (size_t)((g_nbits + 7) / 8 + 1));
    free(g_bits);
    g_bits = NULL;
    g_nbits = 0;
}

/* Model the bitmap living in WIN3 too: each access maps the 16 KB page
 * holding `idx`'s byte into the shared window before touching it, so the
 * batch-reader-vs-bitmap WIN3 contention of Phase 4 is reproduced. */
static u8 *bitmap_byte(u32 idx)
{
    size_t byte = idx >> 3;
    size_t page = byte / WIN3_SZ;
    u8 *win = win3_select(g_bits + page * WIN3_SZ, WIN3_SZ);
    return win + (byte % WIN3_SZ);
}

u8 bitmap_get(u32 idx)
{
    if (idx >= g_nbits) return 0;
    return (*bitmap_byte(idx) >> (idx & 7)) & 1u;
}

void bitmap_set(u32 idx)
{
    if (idx >= g_nbits) return;
    *bitmap_byte(idx) |= (u8)(1u << (idx & 7));
}

u8 bitmap_test_and_set(u32 idx)
{
    u8 prev = bitmap_get(idx);
    bitmap_set(idx);
    return prev;
}

void bitmap_clear_all(void)
{
    if (g_bits) memset(g_bits, 0, (g_nbits + 7) / 8 + 1);
}

void bitmap_invalidate(void) {}

/* ---------------- batch reader ---------------- */

/* CHKDSK_STALE_BATCH=1 simulates the hardware fault observed in the
 * field: multi-sector reads return the disk content as of batch_open
 * (writes made through the single-sector path stay invisible to the
 * batched path). The core must stay correct against a permanently
 * lying batch layer -- all repair decisions must be re-verified via
 * single-sector reads. */

static u8 *g_batch_pages = NULL;
static u8  g_batch_npages = 0;
static u8 *g_stale_snap = NULL;   /* snapshot serving stale batch reads */

extern u8 *host_image_data(u32 *out_secs);

int diskio_batch_open(u8 num_pages)
{
    diskio_batch_close();
    g_batch_pages = malloc((size_t)num_pages * BATCH_PAGE_SIZE);
    if (!g_batch_pages) return 0;
    g_batch_npages = num_pages;
    if (getenv("CHKDSK_STALE_BATCH")) {
        u32 secs;
        u8 *img = host_image_data(&secs);
        g_stale_snap = malloc((size_t)secs * 512);
        if (g_stale_snap) memcpy(g_stale_snap, img, (size_t)secs * 512);
    }
    return 1;
}

void diskio_batch_close(void)
{
    if (g_batch_pages) win3_forget(g_batch_pages, (size_t)g_batch_npages * BATCH_PAGE_SIZE);
    free(g_batch_pages);
    g_batch_pages = NULL;
    g_batch_npages = 0;
    free(g_stale_snap);
    g_stale_snap = NULL;
}

int diskio_batch_read(unsigned long lba, u8 count, u8 page_idx)
{
    u8 *win;
    if (page_idx >= g_batch_npages || count > BATCH_SECTORS_PER_PAGE) return 0;
    win = diskio_batch_map(page_idx);
    if (g_stale_snap) {
        u32 secs;
        (void)host_image_data(&secs);
        if ((u32)lba + g_part_off + count > secs) return 0;
        memcpy(win, g_stale_snap + (size_t)(lba + g_part_off) * 512,
               (size_t)count * 512);
        return 1;
    }
    return (disk_read(0, win, (LBA_t)lba, count) == RES_OK) ? 1 : 0;
}

u8 *diskio_batch_map(u8 page_idx)
{
    return win3_select(g_batch_pages + (size_t)page_idx * BATCH_PAGE_SIZE,
                       BATCH_PAGE_SIZE);
}

void diskio_batch_invalidate(void) {}

/* ---------------- DSS syscall stubs ---------------- */

void dss_puts(const char *s) { fputs(s, stdout); }
void dss_putchar(u8 c)       { fputc((int)c, stdout); }

/* Fixed timestamp so runs are deterministic and diffable. */
void dss_getdate(dss_date_t *d) { d->year = 2026; d->month = 7; d->day = 17; d->dow = 5; }
void dss_gettime(dss_time_t *t) { t->hour = 12; t->minute = 0; t->second = 0; }

int  dss_kbhit(void)              { return 0; }
void dss_waitkey_ex(dss_key_t *k) { k->ascii = 0; k->scan = 0; k->mode = 0; }
void dss_exit(u8 code)            { exit((int)code); }
