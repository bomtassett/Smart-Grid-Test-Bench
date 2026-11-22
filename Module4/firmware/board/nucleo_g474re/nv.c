/*
 * THE CITADEL - DEPARTMENT OF ELECTRICAL & COMPUTER ENGINEERING
 * Engineer:  Thomas Bassett
 * Created:   2026-11-20
 * Modified:  2026-11-22
 *
 * Project:   Smart Grid Test Bench
 * Module:    M4 - Supervisory Control and Data Acquisition (SCADA)
 * Level:     1C - Local Controllers
 * File:      Node Non-volatile Store (CAP-M4-13).
 *
 * Purpose:   Keeps calibration constants and energy totals of node through power cycle,
 *            by saving them in processor's flash memory and restoring them @ start-up.
 *
 * Notes:     Store: area of flash reserved for this data; it has two sectors, one of them active.
 *            Record: one saved copy of node state; newest valid record is restored @ start-up.
 *            Slot: place of one record in sector.
 *            Dual-bank flash is needed; w/o it store is disabled.
 */
#include <string.h>
#include "board.h"

#define NV_BASE        0x0807C000u                /* start address of store */
#define NV_SECTOR      0x2000u                    /* sector size (bytes) */
#define NV_SECTORS     2u                         /* number of sectors */
#define BANK2_BASE     0x08040000u                /* start address of flash bank 2 */
#define PAGE_BYTES     0x800u                     /* flash page size (bytes) */
#define PAGES_PER_SEC  (NV_SECTOR / PAGE_BYTES)   /* pages per sector */

#define HDR_MAGIC      0x564E4753u   /* sector header mark */
#define REC_MAGIC      0x31434552u   /* record mark */
#define CAL_SETTLE_MS  2000u         /* delay from calibration write to its save (ms) */

/* sector header. */
typedef struct {
    uint32_t magic;   /* header mark */
    uint32_t seq;     /* sequence number of sector */
} hdr_t;

/* one record. */
typedef struct {
    uint32_t magic;                      /* record mark */
    uint32_t crc;                        /* CRC of fields below */
    uint32_t flags;                      /* NV_* flags */
    uint32_t address;                    /* slave address of node */
    float    gain[REG_CAL_CHANNELS];     /* calibration gain per channel */
    float    offset[REG_CAL_CHANNELS];   /* calibration offset per channel */
    uint32_t energy[REG_MAX_POINTS];     /* energy total per point (mWh) */
} rec_t;

_Static_assert(sizeof(rec_t) == 96, "record layout");
_Static_assert(sizeof(hdr_t) == 8, "header layout");
#define SLOTS ((NV_SECTOR - sizeof(hdr_t)) / sizeof(rec_t))   /* slots per sector */

static bool     s_on;                  /* true = store is usable */
static unsigned s_active;              /* active sector */
static uint32_t s_seq;                 /* sequence number of active sector */
static unsigned s_free;                /* next free slot */
static rec_t    s_last;                /* state last saved or restored */
static bool     s_have_last;           /* true = s_last is valid */
static int      s_erase_sector = -1;   /* sector being erased; -1 = none */
static unsigned s_erase_page;          /* next page to erase */
static bool     s_erase_started;       /* true = page erase has started */

static float    s_cal_seen[2][REG_CAL_CHANNELS];   /* calibration last seen (gain, offset) */
static uint32_t s_cal_ms;                          /* time of last calibration change (ms) */
static bool     s_cal_pending;                     /* true = calibration save is due */
static uint32_t s_energy_ms;                       /* time of last energy save (ms) */

/* return header of sector s. */
static inline const hdr_t *hdr(unsigned s) { return (const hdr_t *)(NV_BASE + s * NV_SECTOR); }

/* return slot i of sector s. */
static inline const rec_t *slot(unsigned s, unsigned i)
{
    return (const rec_t *)(NV_BASE + s * NV_SECTOR + sizeof(hdr_t) + i * sizeof(rec_t));
}

/* return CRC-32 of block of bytes. */
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* return CRC of record. */
static uint32_t rec_crc(const rec_t *r)
{
    return crc32(&r->flags, sizeof *r - offsetof(rec_t, flags));
}

/* return true when block of flash is erased. */
static bool blank(const void *p, size_t len)
{
    const uint32_t *w = p;
    for (size_t k = 0; k < len / 4u; k++) {
        if (w[k] != 0xFFFFFFFFu) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- flash access */
/* flash error flags */
#define SR_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                   FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR)

/* unlock flash for writing. */
static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

/* return true while flash operation is in progress. */
static inline bool busy(void) { return (FLASH->SR & FLASH_SR_BSY) != 0u; }

/* wait for flash operation to end; return true when it ended w/o error. */
static bool wait_idle(void)
{
    for (uint32_t t = 0; busy(); t++) {
        if (t > 20000000u) {
            return false;
        }
    }
    return true;
}

/* reset flash data cache. */
static void flush_data_cache(void)
{
    if (FLASH->ACR & FLASH_ACR_DCEN) {
        FLASH->ACR &= ~FLASH_ACR_DCEN;
        FLASH->ACR |= FLASH_ACR_DCRST;
        FLASH->ACR &= ~FLASH_ACR_DCRST;
        FLASH->ACR |= FLASH_ACR_DCEN;
    }
}

/* start erase of one page of sector. */
static void erase_start(unsigned sector, unsigned page)
{
    uint32_t pnb = (NV_BASE + sector * NV_SECTOR - BANK2_BASE) / PAGE_BYTES + page;
    unlock();
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_PG | FLASH_CR_MER1 | FLASH_CR_MER2)) |
                FLASH_CR_PER | FLASH_CR_BKER | (pnb << FLASH_CR_PNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
}

/* end page erase. */
static void erase_finish(void)
{
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER);
    FLASH->CR |= FLASH_CR_LOCK;
    flush_data_cache();
}

/* erase whole sector and wait for it; return true on success. */
static bool erase_sector_blocking(unsigned sector)
{
    for (unsigned p = 0; p < PAGES_PER_SEC; p++) {
        erase_start(sector, p);
        bool ok = wait_idle();
        erase_finish();
        if (!ok) {
            return false;
        }
    }
    return blank(hdr(sector), NV_SECTOR);
}

/* program block of bytes; return true on success. */
static bool program(uint32_t addr, const void *data, size_t len)
{
    const uint32_t *w = data;
    bool ok = true;
    unlock();
    if (!wait_idle()) {
        return false;
    }
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR |= FLASH_CR_PG;
    for (size_t k = 0; k < len / 8u && ok; k++) {
        volatile uint32_t *dst = (volatile uint32_t *)(addr + 8u * k);
        dst[0] = w[2u * k];
        dst[1] = w[2u * k + 1u];
        ok = wait_idle() && !(FLASH->SR & SR_ERRORS);
        FLASH->SR = FLASH_SR_EOP;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_LOCK;
    flush_data_cache();
    return ok && memcmp((const void *)addr, data, len) == 0;
}

/* ----------------------------------------------------------------- records */
/* return true when record is valid. */
static bool rec_good(const rec_t *r)
{
    return r->magic == REC_MAGIC && r->crc == rec_crc(r);
}

/* return newest valid record of sector, and its first free slot. */
static const rec_t *scan(unsigned s, unsigned *free_slot)
{
    const rec_t *newest = NULL;
    unsigned next = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        const rec_t *r = slot(s, i);
        if (!blank(r, sizeof *r)) {
            next = i + 1u;
            if (rec_good(r)) {
                newest = r;
            }
        }
    }
    if (free_slot) {
        *free_slot = next;
    }
    return newest;
}

/* make sector s active sector; return true on success. */
static bool open_sector(unsigned s, uint32_t seq)
{
    hdr_t h = {HDR_MAGIC, seq};
    if (!program((uint32_t)hdr(s), &h, sizeof h)) {
        return false;
    }
    s_active = s;
    s_seq = seq;
    s_free = 0;
    return true;
}

/* fill record from present node state. */
static void snapshot(node_app_t *a, rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->magic = REC_MAGIC;
    r->flags = (a->regs.live.status & HLTH_STATUS_CAL_VALID) ? NV_CAL_LOADED : 0u;
    r->address = NODE_ADDRESS;
    memcpy(r->gain, a->gain, sizeof r->gain);
    memcpy(r->offset, a->offset, sizeof r->offset);
    board_lock();
    for (unsigned p = 0; p < a->regs.npoints; p++) {
        r->energy[p] = meter_energy_mwh(&a->meter[p]);
    }
    board_unlock();
    r->crc = rec_crc(r);
}

/* save present node state as new record. */
static void store(node_app_t *a)
{
    rec_t r;
    snapshot(a, &r);
    if (s_free >= SLOTS) {
        unsigned other = s_active ^ 1u;
        if (s_erase_sector >= 0 || !blank(hdr(other), NV_SECTOR)) {
            return;   /* other sector not ready yet */
        }
        unsigned old = s_active;
        if (!open_sector(other, s_seq + 1u)) {
            s_on = false;
            return;
        }
        s_erase_sector = (int)old;
        s_erase_page = 0;
        s_erase_started = false;
        dbg_puts("store: sector change\n");
    }
    if (program((uint32_t)slot(s_active, s_free), &r, sizeof r)) {
        s_last = r;
        s_have_last = true;
        dbg_puts("store: record written\n");
    } else {
        dbg_puts("store: RECORD FAILED\n");
    }
    s_free++;   /* failed slot skipped */
}

/* restore stored state; return NV_* flags. */
unsigned nv_init(node_app_t *a)
{
    unsigned flags = 0;
    s_on = (FLASH->OPTR & FLASH_OPTR_DBANK) != 0u;
    if (!s_on) {
        return 0;
    }

    bool v0 = hdr(0)->magic == HDR_MAGIC, v1 = hdr(1)->magic == HDR_MAGIC;
    if (!v0 && !v1) {
        /* no valid header: store is formatted */
        if ((!blank(hdr(0), NV_SECTOR) && !erase_sector_blocking(0)) ||
            (!blank(hdr(1), NV_SECTOR) && !erase_sector_blocking(1)) || !open_sector(0, 1u)) {
            s_on = false;
            return 0;
        }
    } else {
        s_active = (v0 && v1) ? ((hdr(1)->seq > hdr(0)->seq) ? 1u : 0u) : (v1 ? 1u : 0u);
        unsigned other = s_active ^ 1u;
        s_seq = hdr(s_active)->seq;
        const rec_t *r = scan(s_active, &s_free);
        if (!r && (other ? v1 : v0)) {
            r = scan(other, NULL);   /* sector opened but never written */
        }
        if (r && r->address == NODE_ADDRESS) {
            s_last = *r;
            s_have_last = true;
        }
        /* unused sector erased before sampling starts */
        if (!blank(hdr(other), NV_SECTOR) && !erase_sector_blocking(other)) {
            s_on = false;
            return 0;
        }
    }
    flags |= NV_AVAILABLE;

    if (s_have_last) {
        if (s_last.flags & NV_CAL_LOADED) {
            for (uint8_t ch = 0; ch < REG_CAL_CHANNELS; ch++) {
                node_regs_set_cal(&a->regs, ch, s_last.gain[ch], s_last.offset[ch]);
                a->gain[ch] = s_last.gain[ch];
                a->offset[ch] = s_last.offset[ch];
            }
            a->regs.cal_dirty = false;
            flags |= NV_CAL_LOADED;
        }
        for (uint8_t p = 0; p < a->regs.npoints; p++) {
            meter_set_energy_mwh(&a->meter[p], s_last.energy[p]);
            node_regs_set_energy_mwh(&a->regs, p, s_last.energy[p]);
        }
        flags |= NV_ENERGY_LOADED;
    }
    memcpy(s_cal_seen[0], a->gain, sizeof s_cal_seen[0]);
    memcpy(s_cal_seen[1], a->offset, sizeof s_cal_seen[1]);
    return flags;
}

/* save state when due; called from main loop. */
void nv_task(node_app_t *a, uint32_t now_ms)
{
    /* calibration write from host makes constants valid */
    if (memcmp(s_cal_seen[0], a->gain, sizeof s_cal_seen[0]) != 0 ||
        memcmp(s_cal_seen[1], a->offset, sizeof s_cal_seen[1]) != 0) {
        memcpy(s_cal_seen[0], a->gain, sizeof s_cal_seen[0]);
        memcpy(s_cal_seen[1], a->offset, sizeof s_cal_seen[1]);
        s_cal_ms = now_ms;
        s_cal_pending = true;
        board_lock();
        a->regs.live.status |= HLTH_STATUS_CAL_VALID;
        board_unlock();
    }
    if (!s_on) {
        return;
    }

    /* background erase of old sector */
    if (s_erase_sector >= 0) {
        if (!s_erase_started) {
            erase_start((unsigned)s_erase_sector, s_erase_page);
            s_erase_started = true;
        } else if (!busy()) {
            erase_finish();
            s_erase_started = false;
            if (++s_erase_page >= PAGES_PER_SEC) {
                s_erase_sector = -1;
            }
        }
        return;
    }

    bool due = false;
    if (s_cal_pending && (uint32_t)(now_ms - s_cal_ms) >= CAL_SETTLE_MS) {
        s_cal_pending = false;
        due = true;
    }
    /* energy saved when it has changed */
    bool moved = false, reset = false;
    board_lock();
    for (unsigned p = 0; p < a->regs.npoints; p++) {
        uint32_t e = meter_energy_mwh(&a->meter[p]);
        uint32_t was = s_have_last ? s_last.energy[p] : 0u;
        moved = moved || (e != was);
        reset = reset || (e < was);
    }
    board_unlock();
    if (reset) {
        due = true;
    }
    if ((uint32_t)(now_ms - s_energy_ms) >= NV_ENERGY_PERIOD_MS) {
        s_energy_ms = now_ms;
        due = due || moved;
    }
    if (due) {
        s_energy_ms = now_ms;
        store(a);
    }
}
