/*
 * UFTA-VMM — backend.c — Persistent backends (.vmem file, .txt debug)
 */

#include "ufta/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ── Create a new .vmem file ──────────────────────────────────── */

int backend_create(const char *path, uint64_t total_size,
                   uint32_t page_size, int format)
{
    (void)format;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return UFTA_ERR_IO;

    /* Build header */
    backend_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UFTA_MAGIC, 4);
    hdr.version     = UFTA_VERSION;
    hdr.page_size   = page_size;
    hdr.page_count  = 0;
    hdr.data_offset = UFTA_HEADER_SIZE;
    hdr.total_size  = total_size;

    /* Write header */
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd);
        return UFTA_ERR_IO;
    }

    /* Extend file to total_size with zeros */
    if (total_size > UFTA_HEADER_SIZE) {
        if (ftruncate(fd, (off_t)total_size) != 0) {
            close(fd);
            return UFTA_ERR_IO;
        }
    }

    close(fd);
    return UFTA_OK;
}

/* ── Open backend ─────────────────────────────────────────────── */

int backend_open(backend_t *be, const char *path, bool writable)
{
    memset(be, 0, sizeof(*be));
    strncpy(be->path, path, sizeof(be->path) - 1);
    be->writable = writable;

    int flags = writable ? (O_RDWR) : (O_RDONLY);
    be->fd = open(path, flags);
    if (be->fd < 0) return UFTA_ERR_IO;

    /* Read header */
    if (read(be->fd, &be->header, sizeof(backend_header_t))
        != sizeof(backend_header_t)) {
        close(be->fd);
        return UFTA_ERR_IO;
    }

    /* Validate magic */
    if (memcmp(be->header.magic, UFTA_MAGIC, 4) != 0) {
        close(be->fd);
        return UFTA_ERR_INVAL;
    }

    be->format = UFTA_FMT_BINARY;
    return UFTA_OK;
}

/* ── Close ────────────────────────────────────────────────────── */

void backend_close(backend_t *be)
{
    if (be->is_mapped && be->map_ptr) {
        munmap(be->map_ptr, be->map_size);
    }
    if (be->fd >= 0) {
        close(be->fd);
        be->fd = -1;
    }
}

/* ── Read a page ──────────────────────────────────────────────── */

int backend_read_page(backend_t *be, page_id_t id, void *buf, size_t buf_size)
{
    off_t offset = (off_t)(be->header.data_offset +
                            (id - 1) * be->header.page_size);
    if (lseek(be->fd, offset, SEEK_SET) < 0)
        return UFTA_ERR_IO;

    ssize_t rd = read(be->fd, buf, buf_size);
    if (rd < 0) return UFTA_ERR_IO;
    return (size_t)rd == buf_size ? UFTA_OK : UFTA_ERR_IO;
}

/* ── Write a page ─────────────────────────────────────────────── */

int backend_write_page(backend_t *be, page_id_t id, const void *data, size_t size)
{
    if (!be->writable) return UFTA_ERR_INVAL;

    off_t offset = (off_t)(be->header.data_offset +
                            (id - 1) * be->header.page_size);
    if (lseek(be->fd, offset, SEEK_SET) < 0)
        return UFTA_ERR_IO;

    ssize_t wr = write(be->fd, data, size);
    if (wr < 0) return UFTA_ERR_IO;
    return (size_t)wr == size ? UFTA_OK : UFTA_ERR_IO;
}

/* ── Flush dirty pages ────────────────────────────────────────── */

int backend_flush(backend_t *be, const page_table_t *pt)
{
    if (!be->writable) return UFTA_ERR_INVAL;

    /* Sync metadata first */
    backend_sync_metadata(be, pt);

    /* Force to disk */
    if (fsync(be->fd) != 0) return UFTA_ERR_IO;
    return UFTA_OK;
}

/* ── Sync metadata ────────────────────────────────────────────── */

int backend_sync_metadata(backend_t *be, const page_table_t *pt)
{
    be->header.page_count = pt->count;

    /* Write updated header */
    off_t saved = lseek(be->fd, 0, SEEK_CUR);
    lseek(be->fd, 0, SEEK_SET);
    write(be->fd, &be->header, sizeof(backend_header_t));
    lseek(be->fd, saved, SEEK_SET);
    return UFTA_OK;
}

/* ── Print backend info ───────────────────────────────────────── */

void backend_print(const backend_t *be, int fd)
{
    FILE *f = fdopen(dup(fd), "w");
    if (!f) return;
    fprintf(f, "=== Backend ===\n");
    fprintf(f, "  Path:   %s\n", be->path);
    fprintf(f, "  Format: %s\n", be->format == UFTA_FMT_BINARY ? "binary" : "text");
    fprintf(f, "  Size:   %llu MB\n",
            (unsigned long long)(be->header.total_size / (1024*1024)));
    fprintf(f, "  Pages:  %llu\n", (unsigned long long)be->header.page_count);
    fprintf(f, "  PageSz: %llu\n", (unsigned long long)be->header.page_size);
    fprintf(f, "  R/W:    %s\n", be->writable ? "yes" : "no");
    fclose(f);
}

/* ── Text format: write page ──────────────────────────────────── */

int backend_text_write_page(int fd, const page_t *p)
{
    dprintf(fd,
        "PAGE %06llu\n"
        "SIZE %llu\n"
        "TIER %s\n"
        "HEAT %.3f\n"
        "ACCESS %llu\n"
        "DIR %.3f,%.3f,%.3f\n"
        "VEL %.3f,%.3f,%.3f\n"
        "VADDR 0x%08llX\n"
        "DIRTY %d\n"
        "PINNED %d\n"
        "END\n",
        (unsigned long long)p->id,
        (unsigned long long)p->size,
        p->tier_ptr ? p->tier_ptr->name : "???",
        p->heat.value,
        (unsigned long long)p->access_count,
        p->state.raw.x, p->state.raw.y, p->state.raw.z,
        p->motion.velocity.x, p->motion.velocity.y, p->motion.velocity.z,
        (unsigned long long)p->vaddr,
        (int)p->dirty,
        (int)p->pinned);
    return UFTA_OK;
}

/* ── Text format: read page ───────────────────────────────────── */

int backend_text_read_page(int fd, page_t *p)
{
    char line[256];
    memset(p, 0, sizeof(*p));

    FILE *f = fdopen(dup(fd), "r");
    if (!f) return UFTA_ERR_IO;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PAGE ", 5) == 0) {
            sscanf(line + 5, "%llu", (unsigned long long *)&p->id);
        } else if (strncmp(line, "SIZE ", 5) == 0) {
            sscanf(line + 5, "%llu", (unsigned long long *)&p->size);
        } else if (strncmp(line, "HEAT ", 5) == 0) {
            sscanf(line + 5, "%lf", &p->heat.value);
        } else if (strncmp(line, "ACCESS ", 7) == 0) {
            sscanf(line + 7, "%llu", (unsigned long long *)&p->access_count);
        } else if (strncmp(line, "DIR ", 4) == 0) {
            sscanf(line + 4, "%lf,%lf,%lf",
                   &p->state.raw.x, &p->state.raw.y, &p->state.raw.z);
        } else if (strncmp(line, "VEL ", 4) == 0) {
            sscanf(line + 4, "%lf,%lf,%lf",
                   &p->motion.velocity.x, &p->motion.velocity.y,
                   &p->motion.velocity.z);
        } else if (strncmp(line, "VADDR ", 6) == 0) {
            sscanf(line + 6, "%llx", (unsigned long long *)&p->vaddr);
        } else if (strncmp(line, "DIRTY ", 6) == 0) {
            int d; sscanf(line + 6, "%d", &d); p->dirty = d;
        } else if (strncmp(line, "PINNED ", 7) == 0) {
            int pin; sscanf(line + 7, "%d", &pin); p->pinned = pin;
        } else if (strncmp(line, "END", 3) == 0) {
            fclose(f);
            state_update(&p->state);
            return UFTA_OK;
        }
    }
    fclose(f);
    return UFTA_ERR_IO;
}
