/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The resource region: read-only data the app does not need to start (Rubik, the factory sounds, the BLE radio
 * program), linked at its flash address (app.ld .mres, MELODEE_RES) and read in place through the plain XIP window.
 * 0xEA000..0xF7FFF: the project pool's second block, which storage.c no longer allocates. The installer writes it
 * after an update (editor command 82: web install, tools/fm1_install.py); build.py patches res_want with the length
 * and CRC-32 this build's payload has, so a code-only update keeps the region. Missing, torn or another build's:
 * res_ok 0 and the readers fall back (Melodee Pixel, INIT sounds, the radio refused).
 *   0xEA000  header: magic, length, CRC-32 of the payload, header CRC (written last: the commit record)
 *   0xEA020  payload: .mres as linked; the sector tails hold data too (build.py: none parses as an update record) */
#define RES_BASE 0xEA000u
#define RES_HEAD 32u
#define RES_MAGIC 0x3153524Du                       /* MRS1 */
typedef struct { uint32_t magic, len, crc, rsv[4], hcrc; } res_hdr_t;
_Static_assert(sizeof(res_hdr_t) == RES_HEAD, "resource commit record");
_Static_assert(RES_BASE == 0xEA000u + (ST_POOL_RES_LO - 40u) * ST_SECTOR, "the region is the pool's reserved sectors");
extern const uint8_t _mres_start[], _mres_end[];
/* build.py finds this by name and writes the payload's length and CRC-32 into the image */
__attribute__((used)) static const volatile uint32_t res_want[3] = {0x544E5752u, 0u, 0u};   /* RWNT */

static uint8_t res_open;                             /* begin erased the region: writes may follow */
static uint32_t res_len(void) { return (uint32_t)(_mres_end - _mres_start); }
static int res_valid(void)
{
    res_hdr_t h;
    uint32_t len = res_want[1];
    if (!flash_ok || res_want[0] != 0x544E5752u || !len || len != res_len()) return 0;
    if (st_read(RES_BASE, &h, sizeof h) || h.magic != RES_MAGIC || h.hcrc != st_crc32(&h, sizeof h - 4u) ||
        h.len != len || h.crc != res_want[2])
        return 0;
    return st_crc32(_mres_start, len) == h.crc;      /* the payload as the app reads it */
}
static void res_check(void) { res_ok = (uint8_t)res_valid(); }

/* Old saves can still sit in the reserved sectors: a slot written by firmware before the pool keeps its extension
 * copies there, a pool record may have been allocated there. Each such current record is copied, byte for byte,
 * into the pool's own sectors and published with the next sequence (power-safe: the old one stays current until
 * then). 0, or the first storage error. */
static int res_evacuate(void)
{
    uint8_t b[256];
    for (uint32_t obj = OBJ_BANK0; obj < OBJ_CZBANK0; obj++) {
        st_hdr_t h; uint32_t used[2] = {0, 0};
        st_read_failed = 0;
        int copy = st_current(obj, &h);
        if (st_read_failed) return -7;
        if (copy < 0) continue;
        st_project_used(used, obj, (uint32_t)copy, &h);
        if (!(used[1] & ST_POOL_RES_MASK1)) continue;
        st_stream_t w;
        int rc = st_stream_begin(&w, obj, h.len);
        for (uint32_t off = 0; !rc && off < h.len; off += sizeof b) {
            uint32_t n = h.len - off < sizeof b ? h.len - off : sizeof b;
            rc = st_read_range(obj, (uint32_t)copy, off, b, n) ? -7 : st_stream_write(&w, b, n);
        }
        if (!rc) rc = st_stream_publish(&w);
        if (rc) return rc;
    }
    return 0;
}
/* begin: refuse while anything could read the region or the flash is in use, invalidate it, move old saves out,
 * erase it */
static int res_begin(void)
{
    if (!flash_ok) return 2;
    if (transport_busy()) return 4;
#if MELODEE_BLE
    if (mb.on || mb.closing) return 4;
#endif
    res_ok = 0;
    int rc = res_evacuate();
    if (rc) return 3;
    for (uint32_t s = 0; s < ST_POOL_RES_N; s++)
        if (st_erase(RES_BASE + s * ST_SECTOR)) return 3;
    res_open = 1;
    return 0;
}
/* write payload bytes at off (RAM, any length), split at the NOR pages */
static int res_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    if (!flash_ok || !res_open || off > res_want[1] || n > res_want[1] - off) return 1;
    uint32_t at = RES_BASE + RES_HEAD + off;
    while (n) {
        uint32_t k = 256u - (at & 255u);
        if (k > n) k = n;
        if (st_prog(at, src, k)) return 3;
        at += k; src += k; n -= k;
    }
    return 0;
}
/* commit: the payload checked as the app will read it, then the header last */
static int res_commit(void)
{
    if (!flash_ok || !res_open) return 1;
    res_open = 0;
    if (st_crc32(_mres_start, res_want[1]) != res_want[2]) return 7;
    res_hdr_t h;
    memset(&h, 0, sizeof h);
    h.magic = RES_MAGIC; h.len = res_want[1]; h.crc = res_want[2];
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if (st_prog(RES_BASE, &h, sizeof h)) return 3;
    res_check();
    return res_ok ? 0 : 7;
}
