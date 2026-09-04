/*
 * UFTA-VMM — page.h — Virtual page definition
 *
 * P_p = (id, size, tier, σ_p, v_p, d̂_p, dirty, pinned)
 */

#ifndef UFTA_PAGE_H
#define UFTA_PAGE_H

#include "types.h"
#include "state.h"
#include "tier.h"

#define UFTA_PAGE_SIZE_DEFAULT  4096
#define UFTA_MAX_PAGES          (1024 * 1024)  /* 1M pages max */
#define UFTA_PAGE_TABLE_INITIAL 4096

/* ── Virtual page ─────────────────────────────────────────────── */

typedef struct page {
    page_id_t  id;
    uint64_t   size;         /* bytes                */
    tier_id_t  tier_id;      /* current tier         */

    state_t    state;        /* σ_p = m · σ̂          */
    motion_t   motion;       /* velocity + predicted */
    heat_t     heat;         /* derived heat level   */

    dirty_t    dirty;        /* modified?            */
    bool       pinned;       /* non-migrable?        */

    /* Address mapping */
    addr_t     vaddr;        /* virtual address      */
    addr_t     offset;       /* offset within tier   */

    /* Statistics */
    uint64_t   access_count;
    uint64_t   last_access_ts;
    uint64_t   migrate_count;

    /* Back pointer to tier (set at runtime) */
    tier_t    *tier_ptr;
} page_t;

/* ── Page Table ───────────────────────────────────────────────── */

typedef struct {
    page_t   *pages;        /* array of pages       */
    uint32_t  capacity;
    uint32_t  count;
    addr_t    next_vaddr;   /* next available virtual address */
} page_table_t;

/* ── Page Table Entry for address translation ────────────────── */

typedef struct {
    page_id_t page_id;
    tier_id_t tier_id;
    addr_t    offset;
    bool      present;      /* is page in memory? */
} pte_t;

/* ── Address Map (virtual → physical translation) ─────────────── */

typedef struct {
    pte_t    *entries;
    uint32_t  capacity;
    uint32_t  count;
    uint32_t  page_shift;   /* log2(page_size) */
    uint32_t  page_mask;    /* page_size - 1   */
} addr_map_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Initialize page table with given capacity */
int page_table_init(page_table_t *pt, uint32_t capacity);

/* Allocate a new page */
page_t *page_alloc(page_table_t *pt, uint64_t size, tier_t *tier);

/* Free a page by id */
int page_free(page_table_t *pt, page_id_t id);

/* Find page by id */
page_t *page_find(page_table_t *pt, page_id_t id);

/* Find page by virtual address */
page_t *page_find_by_vaddr(page_table_t *pt, addr_t vaddr);

/* Update page state and derived quantities */
void page_update_state(page_t *p);

/* Update page heat level */
void page_update_heat(page_t *p);

/* Access a page (update stats, increment heat) */
void page_access(page_t *p, uint64_t timestamp);

/* Initialize address map */
int addr_map_init(addr_map_t *am, uint32_t capacity, uint32_t page_size);

/* Translate virtual address → page */
page_t *addr_translate(addr_map_t *am, addr_t vaddr, page_table_t *pt);

/* Map a page to a virtual address */
int addr_map_insert(addr_map_t *am, page_t *p);

/* Print page info */
void page_print(const page_t *p, int fd);

#endif /* UFTA_PAGE_H */
