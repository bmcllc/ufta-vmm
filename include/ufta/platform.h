/*
 * UFTA-VMM — platform.h — Cross-platform compatibility layer
 *
 * Provides POSIX/Linux → Windows (MinGW-w64) shims for:
 *   - mmap / munmap / mprotect  → VirtualAlloc / VirtualFree / VirtualProtect
 *   - sigaction(SIGSEGV)        → VEH (Vectored Exception Handler)
 *   - futex                     → ConditionVariable + CRITICAL_SECTION
 *   - pthread                   → MinGW winpthreads (included)
 *   - clock_gettime             → QueryPerformanceCounter
 *   - memfd_create              → temp file
 *   - pread / pwrite            → lseek + read / write
 *   - posix_memalign            → _aligned_malloc
 *   - nanosleep                 → Sleep
 *   - dprintf                   → fprintf wrapper
 *   - rmdir / unlink            → _rmdir / _unlink
 *   - mkdir                     → _mkdir
 *
 * Linux: this header is a no-op (includes the real POSIX headers).
 * Windows: provides shims and Win32 API mappings.
 */

#ifndef UFTA_PLATFORM_H
#define UFTA_PLATFORM_H

#ifdef _WIN32
/* ═══════════════════════════════════════════════════════════════
 *  WINDOWS (MinGW-w64)
 * ═══════════════════════════════════════════════════════════════ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/* MinGW-w64 provides these */
#include <getopt.h>
#include <pthread.h>

/* ── mmap / munmap / mprotect ─────────────────────────────────── */

#ifndef PROT_READ
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_NONE     0x0
#endif

#ifndef MAP_SHARED
#define MAP_SHARED    0x01
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define MAP_FAILED    ((void *)(intptr_t)-1)
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static inline void *ufta_mmap(void *addr, size_t length, int prot,
                              int flags, int fd, long offset)
{
    DWORD flProtect = PAGE_READWRITE;
    (void)addr; (void)flags; (void)fd; (void)offset;

    if (prot == PROT_NONE)       flProtect = PAGE_NOACCESS;
    else if (prot == PROT_READ)  flProtect = PAGE_READONLY;

    void *ptr = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, flProtect);
    return ptr ? ptr : MAP_FAILED;
}

static inline int ufta_munmap(void *addr, size_t length)
{
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int ufta_mprotect(void *addr, size_t length, int prot)
{
    DWORD flProtect = PAGE_READWRITE;
    (void)length;

    if (prot == PROT_NONE)       flProtect = PAGE_NOACCESS;
    else if (prot == PROT_READ)  flProtect = PAGE_READONLY;

    DWORD oldProtect;
    return VirtualProtect(addr, length, flProtect, &oldProtect) ? 0 : -1;
}

#define mmap     ufta_mmap
#define munmap   ufta_munmap
#define mprotect ufta_mprotect

/* ── mkdir ─────────────────────────────────────────────────────── */

#define mkdir(path, mode) _mkdir(path)

/* ── Signal handling ─────────────────────────────────────────────
 * On Windows we use VEH (Vectored Exception Handler) to catch
 * EXCEPTION_ACCESS_VIOLATION (equivalent of SIGSEGV).
 * The VEH handler calls ufta_win32_pagefault_handler() which must
 * be defined in pagefault.c.
 */

#ifndef SIGSEGV
#define SIGSEGV 11
#endif

#ifndef SA_SIGINFO
#define SA_SIGINFO 0x00000004
#endif

/* Minimal siginfo_t for compat with existing code */
typedef struct {
    int si_signo;
    void *si_addr;
} siginfo_t;

/* sigset_t is not in Win32 — define as DWORD (unused) */
#ifndef _SIGSET_T_DEFINED
#define _SIGSET_T_DEFINED
typedef DWORD sigset_t;
#endif

/* Wrapper struct matching 'struct sigaction' usage */
typedef struct {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    int sa_flags;
    sigset_t sa_mask;
} struct_sigaction;

/* sigemptyset stub */
static inline int sigemptyset(sigset_t *set) { *set = 0; return 0; }

/* Declared in pagefault.c — called by the VEH handler */
extern void ufta_win32_pagefault_handler(void *fault_addr);

/* Global VEH bookkeeping (set by pf_init/pf_destroy) */
static void *g_veh_handle = NULL;
static struct_sigaction g_veh_prev = {0};

/* Windows VEH callback */
static LONG CALLBACK ufta_veh_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        if (ep->ExceptionRecord->NumberParameters >= 2) {
            void *fault_addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            ufta_win32_pagefault_handler(fault_addr);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static inline int ufta_sigaction(int signum, const struct_sigaction *act,
                                  struct_sigaction *oldact)
{
    (void)signum;
    if (oldact) {
        *oldact = g_veh_prev;
    }
    if (act && act->sa_sigaction == NULL) {
        /* Uninstall VEH handler */
        if (g_veh_handle) {
            RemoveVectoredExceptionHandler(g_veh_handle);
            g_veh_handle = NULL;
        }
        return 0;
    }
    /* Install VEH handler */
    if (act) g_veh_prev = *act;
    g_veh_handle = AddVectoredExceptionHandler(1, ufta_veh_handler);
    return g_veh_handle ? 0 : -1;
}

#define sigaction     ufta_sigaction

/* ── pread / pwrite ───────────────────────────────────────────── */

static inline ssize_t ufta_pread(int fd, void *buf, size_t count, long offset)
{
    long saved = _lseek(fd, 0, SEEK_CUR);
    _lseek(fd, offset, SEEK_SET);
    ssize_t rd = (ssize_t)_read(fd, buf, (unsigned int)count);
    _lseek(fd, saved, SEEK_SET);
    return rd;
}

static inline ssize_t ufta_pwrite(int fd, const void *buf, size_t count, long offset)
{
    long saved = _lseek(fd, 0, SEEK_CUR);
    _lseek(fd, offset, SEEK_SET);
    ssize_t wr = (ssize_t)_write(fd, buf, (unsigned int)count);
    _lseek(fd, saved, SEEK_SET);
    return wr;
}

#define pread  ufta_pread
#define pwrite ufta_pwrite

/* ── memfd_create → temp file ──────────────────────────────────── */

static inline int ufta_memfd_create(const char *name, unsigned int flags)
{
    (void)flags;
    char tmppath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmppath);
    char filename[MAX_PATH];
    snprintf(filename, MAX_PATH, "%sufta_%s.tmp", tmppath, name);
    int fd = _open(filename, _O_BINARY | _O_RDWR | _O_CREAT | _O_TEMPORARY,
                   _S_IREAD | _S_IWRITE);
    return fd;
}

#define memfd_create ufta_memfd_create

/* ── posix_memalign → _aligned_malloc ──────────────────────────── */

static inline int ufta_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    *memptr = _aligned_malloc(size, alignment);
    return *memptr ? 0 : ENOMEM;
}

#define posix_memalign ufta_posix_memalign

/*
 * NOTE: Do NOT redefine free() — it would break all normal malloc() frees.
 * For allocations made via posix_memalign/_aligned_malloc on Windows, use
 * ufta_aligned_free() instead of free(). On Linux, free() works for both.
 */
static inline void ufta_aligned_free(void *p) { _aligned_free(p); }

/* ── clock_gettime ─────────────────────────────────────────────── */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Do NOT redefine struct timespec — MinGW defines it already */

static inline int ufta_clock_gettime(int clock_id, struct timespec *ts)
{
    (void)clock_id;
    static LARGE_INTEGER freq = {0};
    static BOOL freq_init = FALSE;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = TRUE;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ts->tv_sec  = (time_t)(now.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}

#define clock_gettime ufta_clock_gettime

/* ── nanosleep ─────────────────────────────────────────────────── */

static inline int ufta_nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    if (ms == 0) ms = 1;
    Sleep(ms);
    return 0;
}

#define nanosleep ufta_nanosleep

/* ── dprintf (POSIX dprintf not in MinGW) ─────────────────────── */

static inline int ufta_dprintf(int fd, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    FILE *f = _fdopen(_dup(fd), "w");
    if (!f) { va_end(args); return -1; }
    int ret = vfprintf(f, fmt, args);
    fclose(f);
    va_end(args);
    return ret;
}

#define dprintf ufta_dprintf

/* ── fsync → _commit ───────────────────────────────────────────── */

#define fsync _commit

/* ── rmdir / unlink ────────────────────────────────────────────── */

#define rmdir  _rmdir
#define unlink _unlink

/* ── File mode constants ───────────────────────────────────────── */

#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE
#endif

/* ── ftruncate ─────────────────────────────────────────────────── */

static inline int ufta_ftruncate(int fd, long length)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    long cur = _lseek(fd, 0, SEEK_CUR);
    SetFilePointer(h, length, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    _lseek(fd, cur, SEEK_SET);
    return 0;
}

#define ftruncate ufta_ftruncate

/* ── read/write/close/open/lseek (MinGW uses underscore prefix) ── */

#define read   _read
#define write  _write
#define close  _close
#define open   _open
#define lseek  _lseek

/* ── POSIX constants not in MinGW's default headers ───────────── */

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

/* ── Off_t ─────────────────────────────────────────────────────── */

#ifndef _OFF_T_DEFINED
typedef long off_t;
#define _OFF_T_DEFINED
#endif

#else /* ── LINUX ──────────────────────────────────────────────── */

/* Include all the real POSIX headers */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>

/* On Linux, free() handles posix_memalign results — provide a uniform API */
#include <stdlib.h>
static inline void ufta_aligned_free(void *p) { free(p); }
#include <pthread.h>
#include <getopt.h>

#endif /* _WIN32 */

#endif /* UFTA_PLATFORM_H */
