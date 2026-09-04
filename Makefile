# UFTA-VMM — Universal Field Theory Architecture
# Virtual Memory Manager
#
# Build:          make            (CPU-only, no CUDA)
# Build CUDA:     make cuda       (with CUDA GPU support)
# Clean:          make clean
# Validate CPU:   ./uvm validate
# Validate GPU:   ./uvm validate-cuda

CC       = gcc
CFLAGS   = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
CFLAGS  += -Iinclude
CFLAGS  += -O2 -g
LDFLAGS  = -lm

# GUI (SDL2 + OpenGL)
GUI_CFLAGS  = $(shell pkg-config --cflags sdl2)
GUI_LDFLAGS = $(shell pkg-config --libs sdl2) -lGL -lpthread

# CUDA
NVCC     = nvcc
NVCCFLAGS = -std=c++14 -Iinclude -O2 -g -D_GNU_SOURCE
CUDALIBS = -lcudart

SRCDIR   = src
INCDIR   = include
BUILDDIR = build

# CPU sources (exclude .cu files, exclude gui.c — use stub instead)
SOURCES  = $(filter-out %.cu, $(wildcard $(SRCDIR)/*.c))
OBJECTS  = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET   = uvm

# Core sources: everything except gui.c (gui_stub.c provides the stub)
CORE_SOURCES = $(filter-out $(SRCDIR)/gui.c, $(SOURCES))
CORE_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SOURCES))
GUI_TARGET   = uvm-gui

.PHONY: all clean demo run cuda validate-cuda gui package package-portable package-deb installer

# ── Default: CPU-only build ─────────────────────────────────────

all: $(TARGET)

$(TARGET): $(CORE_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── GUI build (SDL2 + OpenGL) ───────────────────────────────────

gui: $(CORE_OBJECTS) $(BUILDDIR)/gui.o
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -o $(GUI_TARGET) \
	    $(filter-out $(BUILDDIR)/gui_stub.o,$(CORE_OBJECTS)) \
	    $(BUILDDIR)/gui.o $(LDFLAGS) $(GUI_LDFLAGS)

$(BUILDDIR)/gui.o: $(SRCDIR)/gui.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(GUI_CFLAGS) -c -o $@ $<

# ── CUDA build ──────────────────────────────────────────────────

CUDA_OBJ  = $(BUILDDIR)/cuda_backend.o
CUDA_SRCS = $(wildcard $(SRCDIR)/*.cu)

# Recompile main.c with nvcc so __CUDACC__ is defined
CUDA_MAIN = $(BUILDDIR)/main_cuda.o

# Recompile pagefault.c with nvcc so PF_HAS_CUDA is defined
CUDA_PF = $(BUILDDIR)/pagefault_cuda.o

cuda: $(CORE_OBJECTS) $(CUDA_OBJ) $(CUDA_MAIN) $(CUDA_PF)
	$(NVCC) $(NVCCFLAGS) -o $(TARGET)-cuda \
	    $(filter-out $(BUILDDIR)/main.o $(BUILDDIR)/pagefault.o,$(CORE_OBJECTS)) \
	    $(CUDA_MAIN) $(CUDA_PF) $(CUDA_OBJ) \
	    $(CUDALIBS) -lm

$(BUILDDIR)/cuda_backend.o: $(SRCDIR)/cuda_backend.cu | $(BUILDDIR)
	$(NVCC) $(NVCCFLAGS) -c -o $@ $<

$(BUILDDIR)/main_cuda.o: $(SRCDIR)/main.c | $(BUILDDIR)
	$(NVCC) $(NVCCFLAGS) -DHAVE_CUDA_BACKEND -c -o $@ $<

$(BUILDDIR)/pagefault_cuda.o: $(SRCDIR)/pagefault.c | $(BUILDDIR)
	$(NVCC) $(NVCCFLAGS) -DHAVE_CUDA_BACKEND -c -o $@ $<

# ── Clean ───────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR) $(TARGET) $(TARGET)-cuda $(GUI_TARGET)

# ── Packaging / Release ─────────────────────────────────────────

VERSION ?= 1.0.0

# Build all distributable packages
package: package-portable package-deb

# Self-contained portable tar.gz with embedded SDL2/OpenGL libs
package-portable:
	@echo "═══ Building portable Linux package ═══"
	@bash packaging/linux/build-portable.sh --version $(VERSION)

# Native Debian/Ubuntu .deb installer
package-deb:
	@echo "═══ Building Debian package ═══"
	@bash packaging/linux/build-deb.sh --version $(VERSION)

# Self-extracting .run installer (no compile needed by end user)
installer:
	@echo "═══ Building self-extracting installer ═══"
	@bash packaging/linux/build-installer.sh --version $(VERSION)

# ── Convenience targets ─────────────────────────────────────────

demo: $(TARGET)
	./$(TARGET) demo

run: $(TARGET)
	./$(TARGET) run

validate: $(TARGET)
	./$(TARGET) validate

validate-cuda: cuda
	./$(TARGET)-cuda validate-cuda
