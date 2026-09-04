/*
 * UFTA-VMM — tier.c — Memory tier and bandwidth allocator implementation
 *
 * T_i = (B_max, L, C, E, Q)
 * B_alloc = min(B_max, β, B_budget)
 */

#include "ufta/tier.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ── Tier initialization ──────────────────────────────────────── */

void tier_init(tier_t *t, const char *name, real_t bw_max, real_t latency,
               uint64_t capacity, real_t energy, real_t transfer)
{
    t->id           = 0;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->bandwidth_max  = bw_max;
    t->latency        = latency;
    t->capacity       = capacity;
    t->energy_cost    = energy;
    t->transfer_cost  = transfer;
    t->used           = 0;
    t->page_count     = 0;
    t->utilization    = 0.0;

    /* Measured fields default to 0 (uncalibrated) */
    t->bw_measured      = 0.0;
    t->lat_measured     = 0.0;
    t->calibrated       = false;
    t->bytes_read       = 0;
    t->bytes_written    = 0;
    t->bytes_migrated   = 0;
    t->transfer_count   = 0;
}

/* ── Default tier registry ────────────────────────────────────── */

void tier_registry_init_defaults(tier_registry_t *reg)
{
    reg->num_tiers = 0;

    /* T0: RAM — 20 GB/s, 60 ns, 16 GB */
    tier_t *ram = &reg->tiers[reg->num_tiers++];
    tier_init(ram, "RAM", 20.0e9, 60e-9, 16ULL * 1024 * 1024 * 1024, 1e-9, 0.01);
    ram->id = 0;

    /* T1: VRAM — 500 GB/s, 200 ns, 8 GB */
    tier_t *vram = &reg->tiers[reg->num_tiers++];
    tier_init(vram, "VRAM", 500.0e9, 200e-9, 8ULL * 1024 * 1024 * 1024, 2e-9, 0.02);
    vram->id = 1;

    /* T2: NVMe — 3 GB/s, 20 µs, 1 TB */
    tier_t *nvme = &reg->tiers[reg->num_tiers++];
    tier_init(nvme, "NVMe", 3.0e9, 20e-6, 1ULL * 1024 * 1024 * 1024 * 1024, 5e-9, 0.05);
    nvme->id = 2;

    /* T3: USB — 120 MB/s, 1 ms, 64 GB */
    tier_t *usb = &reg->tiers[reg->num_tiers++];
    tier_init(usb, "USB", 120.0e6, 1e-3, 64ULL * 1024 * 1024 * 1024, 1e-8, 0.10);
    usb->id = 3;

    /* T4: File (.vmem) — 200 MB/s, 5 ms, unlimited (disk) */
    tier_t *file = &reg->tiers[reg->num_tiers++];
    tier_init(file, "FILE", 200.0e6, 5e-3, UINT64_MAX, 1e-8, 0.15);
    file->id = 4;
}

/* ── Find tier by name ────────────────────────────────────────── */

tier_t *tier_registry_find(tier_registry_t *reg, const char *name)
{
    for (int i = 0; i < reg->num_tiers; i++) {
        if (strcasecmp(reg->tiers[i].name, name) == 0)
            return &reg->tiers[i];
    }
    return NULL;
}

/* ── Effective bandwidth ──────────────────────────────────────── */

real_t bw_effective(const channel_t *ch)
{
    real_t eff = ch->bandwidth_max;
    if (ch->bandwidth_desired < eff)  eff = ch->bandwidth_desired;
    if (ch->bandwidth_budget  < eff)  eff = ch->bandwidth_budget;
    return eff;
}

/* ── Bandwidth allocation (weighted optimization) ─────────────── */

void bw_allocate(bw_allocator_t *alloc)
{
    if (alloc->num_channels <= 0) return;

    /* Compute total weight */
    real_t total_weight = 0.0;
    for (int i = 0; i < alloc->num_channels; i++) {
        total_weight += alloc->channels[i].weight;
    }

    if (total_weight < 1e-12) {
        /* Equal distribution */
        real_t per_channel = alloc->total_budget / alloc->num_channels;
        for (int i = 0; i < alloc->num_channels; i++) {
            alloc->channels[i].bandwidth_budget = per_channel;
        }
    } else {
        /* Weighted distribution */
        for (int i = 0; i < alloc->num_channels; i++) {
            real_t share = (alloc->channels[i].weight / total_weight)
                           * alloc->total_budget;
            alloc->channels[i].bandwidth_budget = share;
        }
    }

    /* Clamp to physical max and compute effective */
    for (int i = 0; i < alloc->num_channels; i++) {
        channel_t *ch = &alloc->channels[i];
        if (ch->bandwidth_budget > ch->bandwidth_max)
            ch->bandwidth_budget = ch->bandwidth_max;
        ch->bandwidth_alloc = bw_effective(ch);
    }
}

/* ── Print tier info ──────────────────────────────────────────── */

void tier_print(const tier_t *t, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "Tier %-6s | BW: %8.1f MB/s | Lat: %8.1f µs | "
               "Cap: %8llu MB | Used: %8llu MB | Pages: %u | Util: %.1f%%",
            t->name,
            t->bandwidth_max / 1e6,
            t->latency * 1e6,
            (unsigned long long)(t->capacity / (1024*1024)),
            (unsigned long long)(t->used / (1024*1024)),
            t->page_count,
            t->utilization * 100.0);
    if (t->calibrated) {
        fprintf(f, " [CALIBRATED: %.1f GB/s, %.1f ns]",
                t->bw_measured / 1e9,
                t->lat_measured * 1e9);
    }
    fprintf(f, "\n");
    fclose(f);
}
