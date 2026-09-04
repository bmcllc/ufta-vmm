/*
 * gui_stub.c — Stub for builds without SDL2/OpenGL.
 * Provides a no-op ufta_gui_run() that prints an error message.
 */
#include "ufta/gui.h"
#include <stdio.h>

int ufta_gui_run(const ufta_gui_snapshot_t *snap)
{
    (void)snap;
    fprintf(stderr,
        "╔══════════════════════════════════════════════╗\n"
        "║  GUI não disponível nesta build.            ║\n"
        "║  Use: make gui  (requer SDL2 + OpenGL)      ║\n"
        "╚══════════════════════════════════════════════╝\n");
    return -1;
}
