/*
 * UFTA-VMM — page.c — Virtual page and page table implementation
 */

#include "ufta/page.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ── Page Table ───────────────────────────────────────────────── */

int page_table_init(page_table_t *pt, uint32_t capacity)
{
    if (capacity == 0) capacity = UFTA_PAGE_TABLE_INITIAL;
    pt->pages = calloc(capacity, sizeof(page_t));
    if (!pt->pages) return UFTA_ERR_NOMEM;
    pt->capacity  = capacity;
    pt->count     = 0;
    pt->next_vaddr = 0x40000000; /* start virtual addresses here */
    return UFTA_OK;
}

/* ── Allocate a page ──────────────────────────────────────────── */

page_t *page_alloc(page_table_t *pt, uint64_t size, tier_t *tier)
{
    if (pt->count >= pt->capacity) return NULL;

    page_t *p = &pt->pages[pt->count];
    memset(p, 0, sizeof(*p));

    p->id       = pt->count + 1; /* 1-based */
    p->size     = size ? size : UFTA_PAGE_SIZE_DEFAULT;
    p->tier_id  = tier ? tier->id : 0;
    p->tier_ptr = tier;
    p->vaddr    = pt->next_vaddr;
    p->dirty    = PAGE_CLEAN;
    p->pinned   = false;

    /* Initialize state with small random perturbation */
    p->state.raw = (vec3_t){
        .x = 0.1 + (real_t)(p->id % 100) / 1000.0,  /* heat */
        .y = 0.01,                                      /* rate */
        .z = 0.01                                        /* width */
    };
    state_update(&p->state);

    /* Initialize motion */
    p->motion.velocity      = vec3_zero();
    p->motion.dir_predicted = p->state.versor;
    p->motion.speed         = 0.0;

    /* Initialize heat */
    p->heat = state_heat(&p->state);

    /* Update tier usage */
    if (tier) {
        tier->used += p->size;
        tier->page_count++;
        tier->utilization = (real_t)tier->used / (real_t)tier->capacity;
    }

    pt->next_vaddr += p->size;
    pt->count++;

    return p;
}

/* ── Free a page ──────────────────────────────────────────────── */

int page_free(page_table_t *pt, page_id_t id)
{
    for (uint32_t i = 0; i < pt->count; i++) {
        if (pt->pages[i].id == id) {
            page_t *p = &pt->pages[i];

            /* Update tier usage */
            if (p->tier_ptr) {
                if (p->tier_ptr->used >= p->size)
                    p->tier_ptr->used -= p->size;
                if (p->tier_ptr->page_count > 0)
                    p->tier_ptr->page_count--;
                p->tier_ptr->utilization =
                    (real_t)p->tier_ptr->used / (real_t)p->tier_ptr->capacity;
            }

            /* Compact array */
            if (i < pt->count - 1) {
                memmove(&pt->pages[i], &pt->pages[i + 1],
                        (pt->count - i - 1) * sizeof(page_t));
            }
            pt->count--;
            return UFTA_OK;
        }
    }
    return UFTA_ERR_NOTFOUND;
}

/* ── Find page ────────────────────────────────────────────────── */

page_t *page_find(page_table_t *pt, page_id_t id)
{
    for (uint32_t i = 0; i < pt->count; i++) {
        if (pt->pages[i].id == id)
            return &pt->pages[i];
    }
    return NULL;
}

page_t *page_find_by_vaddr(page_table_t *pt, addr_t vaddr)
{
    for (uint32_t i = 0; i < pt->count; i++) {
        if (pt->pages[i].vaddr == vaddr)
            return &pt->pages[i];
    }
    return NULL;
}

/* ── Page update ──────────────────────────────────────────────── */

void page_update_state(page_t *p)
{
    state_update(&p->state);
}

void page_update_heat(page_t *p)
{
    p->heat = state_heat(&p->state);
}

void page_access(page_t *p, uint64_t timestamp)
{
    p->access_count++;
    p->last_access_ts = timestamp;

    /* Increase heat component */
    p->state.raw.x += 0.05;
    state_update(&p->state);
    page_update_heat(p);
}

/* ── Address Map ──────────────────────────────────────────────── */

int addr_map_init(addr_map_t *am, uint32_t capacity, uint32_t page_size)
{
    am->entries = calloc(capacity, sizeof(pte_t));
    if (!am->entries) return UFTA_ERR_NOMEM;
    am->capacity  = capacity;
    am->count     = 0;
    am->page_shift = 0;
    am->page_mask  = page_size - 1;

    /* Compute page_shift */
    uint32_t ps = page_size;
    while (ps > 1) { am->page_shift++; ps >>= 1; }

    return UFTA_OK;
}

page_t *addr_translate(addr_map_t *am, addr_t vaddr, page_table_t *pt)
{
    uint32_t idx = (uint32_t)(vaddr >> am->page_shift);
    if (idx >= am->count) return NULL;
    pte_t *pte = &am->entries[idx];
    if (!pte->present) return NULL;
    return page_find(pt, pte->page_id);
}

int addr_map_insert(addr_map_t *am, page_t *p)
{
    uint32_t idx = (uint32_t)(p->vaddr >> am->page_shift);
    if (idx >= am->capacity) return UFTA_ERR_FULL;

    /* Ensure we have enough entries */
    while (am->count <= idx) {
        am->entries[am->count].present = false;
        am->count++;
    }

    am->entries[idx].page_id = p->id;
    am->entries[idx].tier_id = p->tier_id;
    am->entries[idx].offset  = p->offset;
    am->entries[idx].present = true;
    return UFTA_OK;
}

/* ── Print page ───────────────────────────────────────────────── */

void page_print(const page_t *p, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "Page %06llu | VAddr: 0x%08llX | Tier: %-5s | "
               "Size: %llu | Heat: %.3f (%s) | "
               "Accesses: %llu | Dirty: %s | Pinned: %s\n",
            (unsigned long long)p->id,
            (unsigned long long)p->vaddr,
            p->tier_ptr ? p->tier_ptr->name : "???",
            (unsigned long long)p->size,
            p->heat.value,
            p->heat.level == HEAT_HOT  ? "HOT"  :
            p->heat.level == HEAT_WARM ? "WARM" : "COLD",
            (unsigned long long)p->access_count,
            p->dirty ? "yes" : "no",
            p->pinned ? "yes" : "no");
    fclose(f);
}
