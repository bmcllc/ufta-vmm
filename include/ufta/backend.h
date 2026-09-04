/*
 * UFTA-VMM — backend.h — Persistent backends (.vmem file, .txt debug)
 *
 * Binary format:
 *   [HEADER] [PAGE TABLE] [STATE VECTOR TABLE] [FLOW TABLE] [DATA REGION]
 *
 * Text format:
 *   UFTA-VMM v1
 *   PAGE 000001
 *   SIZE 4096
 *   ...
 */

#ifndef UFTA_BACKEND_H
#define UFTA_BACKEND_H

#include "types.h"
#include "page.h"
#include "tier.h"

/* ── File format constants ────────────────────────────────────── */

#define UFTA_MAGIC          "UFTA"
#define UFTA_VERSION        1
#define UFTA_HEADER_SIZE    4096

#define UFTA_FMT_BINARY     0
#define UFTA_FMT_TEXT       1

/* ── Binary file header ───────────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    char      magic[4];        /* "UFTA"             */
    uint32_t  version;         /* format version     */
    uint64_t  page_count;      /* total pages        */
    uint64_t  page_size;       /* bytes per page     */
    uint64_t  state_offset;    /* offset to state vec table */
    uint64_t  flow_offset;     /* offset to flow table      */
    uint64_t  data_offset;     /* offset to data region     */
    uint64_t  total_size;      /* total file size           */
    char      reserved[4096 - 4 - 4 - 6*8]; /* pad to 4KB   */
} backend_header_t;
#pragma pack(pop)

/* ── Binary page table entry ──────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    page_id_t  page_id;
    uint64_t   size;
    tier_id_t  tier_id;
    uint8_t    dirty;
    uint8_t    pinned;
    real_t     heat_value;
    uint64_t   access_count;
    vec3_t     state_raw;
    vec3_t     velocity;
} backend_page_entry_t;
#pragma pack(pop)

/* ── Backend handle ───────────────────────────────────────────── */

typedef struct {
    int        fd;              /* file descriptor            */
    char       path[512];       /* file path                 */
    int        format;          /* UFTA_FMT_BINARY or TEXT   */
    bool       writable;

    backend_header_t header;

    /* Mapping for mmap (if used) */
    void      *map_ptr;
    size_t     map_size;
    bool       is_mapped;
} backend_t;

/* ── API ──────────────────────────────────────────────────────── */

/* Create a new .vmem file with given size */
int backend_create(const char *path, uint64_t total_size,
                   uint32_t page_size, int format);

/* Open an existing .vmem file */
int backend_open(backend_t *be, const char *path, bool writable);

/* Close backend */
void backend_close(backend_t *be);

/* Flush dirty pages to disk */
int backend_flush(backend_t *be, const page_table_t *pt);

/* Read a single page from backend */
int backend_read_page(backend_t *be, page_id_t id, void *buf, size_t buf_size);

/* Write a single page to backend */
int backend_write_page(backend_t *be, page_id_t id, const void *data, size_t size);

/* Sync page table metadata to file header */
int backend_sync_metadata(backend_t *be, const page_table_t *pt);

/* Print backend info */
void backend_print(const backend_t *be, int fd);

/* ── Text format I/O ──────────────────────────────────────────── */

/* Write page in human-readable text format */
int backend_text_write_page(int fd, const page_t *p);

/* Read page from text format */
int backend_text_read_page(int fd, page_t *p);

#endif /* UFTA_BACKEND_H */
