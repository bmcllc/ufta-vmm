# UFTA-VMM

**Universal Field Transformation Architecture — Virtual Memory Manager**

A field-driven virtual memory manager implementing hierarchical memory tier migration using mathematical state vectors, adaptive prediction, and cost/gain analysis.

## Architecture

```
                        APPLICATION
                            │
                    ┌───────▼────────┐
                    │  UFTA RUNTIME  │
                    └───────┬────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
   FIELD ENGINE       OPERATOR ENGINE      PREDICTOR
        │                   │                   │
        └───────────────┬───┴───────────┬───────┘
                        │               │
                   STATE ENGINE    SCHEDULER
                        │               │
                        └───────┬───────┘
                                │
                        VIRTUAL MEMORY
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                 │
             RAM               NVMe              USB
              │                 │                 │
            VRAM              ZRAM             FILE/.vmem
```

## Memory Tiers

| Tier | Backend | Bandwidth | Latency | Capacity |
|------|---------|-----------|---------|----------|
| T0 | RAM | 20 GB/s | 60 ns | 16 GB |
| T1 | VRAM | 500 GB/s | 200 ns | 8 GB |
| T2 | NVMe | 3 GB/s | 20 µs | 1 TB |
| T3 | USB | 120 MB/s | 1 ms | 64 GB |
| T4 | File (.vmem) | 200 MB/s | 5 ms | unlimited |

## State Model

Every entity carries a state vector σ decomposed as:

**σ = m · σ̂**

- `m` = magnitude (intensity, activity level)
- `σ̂` = versor (normalized direction in state space)

Heat level is derived: `H = h / √(h² + r² + w² + 1)`

- HOT: H > 0.8
- WARM: 0.3 < H ≤ 0.8
- COLD: H ≤ 0.3

## Pipeline

The runtime executes a 7-step cycle:

1. **Observe** — collect metrics (access counts, latencies, dirty bits)
2. **Represent** — update σ, v, d̂ for all pages
3. **Predict** — compute σ̂_{t+Δt}^{pred} with adaptive rotation matrix
4. **Prioritize** — compute priority π_p for all pages
5. **Throttle** — allocate bandwidth B_j^{alloc}(t) within budget
6. **Migrate** — evaluate cost/gain, execute beneficial migrations
7. **Commit** — apply state changes, persist dirty pages

## Migration Logic

A page migrates from tier T_i to T_j when:

**G_{i→j}(p) > C_{i→j}(p)**

- Gain: G = Σ (cost_access(i) - cost_access(j)) × predicted_accesses
- Cost: C = size / B_effective + latency + energy

## Build

```bash
make          # build the 'uvm' binary (CPU-only)
make cuda     # build 'uvm-cuda' with real GPU VRAM support
make gui      # build 'uvm-gui' with SDL2 + OpenGL dashboard
make installer # build self-extracting .run installer (no compile needed)
make package  # build all distributable packages
make demo     # run interactive demo
make clean    # clean build artifacts
```

> **GUI dependency**: `make gui` requires SDL2 and OpenGL development
> packages. On Debian/Ubuntu: `sudo apt install libsdl2-dev libgl1-mesa-dev`.

## Instalação (para usuários finais)

Para **usuários finais** que não têm o código-fonte nem querem compilar,
basta distribuir o instalador autossuficiente:

```bash
# 1. Baixe o instalador e torne-o executável
chmod +x ufta-vmm-1.0.0-linux-x86_64.run

# 2. Instale (padrão: /usr/local)
./ufta-vmm-1.0.0-linux-x86_64.run

# 3. Use
ufta-vmm gui        # Dashboard em tempo real (SDL2+OpenGL)
ufta-vmm validate   # Validação de migração
ufta-vmm help       # Lista todos os comandos

# Para remover
./ufta-vmm-1.0.0-linux-x86_64.run --uninstall
```

O instalador embute as bibliotecas SDL2/OpenGL, então **não requer
dependências externas** e funciona em qualquer sistema x86_64 Linux.

Veja [`packaging/README.md`](packaging/README.md) para detalhes sobre
todos os formatos de distribuição (instalador `.run`, `.tar.gz`
portátil, `.deb` e Windows `.zip`).

## Usage

```bash
# Create a virtual memory file
./uvm create memory.vmem --size 8G

# Run demo with sample pages
./uvm demo

# Run the VMM pipeline
./uvm run --file memory.vmem --size 8G

# Validate: real bandwidth benchmark + migration test (CPU)
./uvm validate --pages 65536 --page-size 4096

# Validate: real GPU VRAM (requires CUDA + NVIDIA GPU)
./uvm-cuda validate-cuda --pages 16384 --page-size 4096

# Run full pipeline with real GPU VRAM (kernel processing + migration)
./uvm-cuda run-cuda --pages 16384 --page-size 4096

# Transparent page fault migration (SIGSEGV + mprotect)
./uvm page-fault --pages 1024 --page-size 4096

# Page faults with REAL GPU VRAM backend
./uvm-cuda page-fault-cuda --pages 256 --page-size 4096

# Intelligent Migration with Learning (predictor R(θ) + LMS + momentum)
# Demonstrates the predictor learning a rotation pattern and driving
# migration decisions vs. random. With CUDA, also tests real VRAM faults.
./uvm validate-intelligent --pages 128 --cycles 200 --lr 0.05
./uvm-cuda validate-intelligent --pages 128 --cycles 200 --lr 0.05

# Real-time dashboard (SDL2 + OpenGL)
./uvm-gui gui --pages 256 --page-size 4096

# Show help
./uvm help
```

## Real-Time Dashboard (GUI)

`make gui` produces `uvm-gui`, a real-time SDL2 + OpenGL dashboard that
visualizes the UFTA-VMM pipeline live:

- **Tier occupancy** — RAM / VRAM / NVMe / USB / FILE usage bars
- **Pipeline metrics** — cycle count, FPS estimate, VRAM recovered
- **Worker thread stats** — ring buffer fill, batch sizes, faults handled
- **Page fault telemetry** — faults/sec, latency per fault, bytes loaded
- **Live charts** — bandwidth utilization and migration activity over time

Run it with:

```bash
./uvm-gui gui --pages 256 --page-size 4096
```

Press **ESC** or close the window to exit. The dashboard runs the full
pipeline (Observe→Represent→Predict→Prioritize→Throttle→Migrate→Commit)
in a background thread while rendering at ~60 FPS.

> The core `uvm` binary uses a stub for `ufta_gui_run` so it stays
> dependency-free. Only `uvm-gui` links SDL2/OpenGL.

## Intelligent Migration with Learning

`validate-intelligent` demonstrates the adaptive predictor learning a
rotation pattern and using that knowledge to make migration decisions.

**How it works:**

1. **Pattern** — hot pages follow a *rotated* circular trajectory
   `σ(t) = R(θ_true)·[cos(ωt), sin(ωt), 0.5]`, but the velocity supplied to
   the predictor is the *unrotated* derivative. With θ=0 (initial), the
   prediction is badly wrong.
2. **Learning** — the predictor runs LMS gradient descent with momentum on
   the rotation parameters θ, driving θ → θ_true. The error drops as the
   predictor discovers the rotation.
3. **Migration** — after learning, the predictor's confidence decides which
   pages stay in RAM (T0) vs. migrate to VRAM (T1). Compared against random
   decisions.
4. **VRAM (CUDA)** — with `uvm-cuda`, also exercises real GPU VRAM page
   faults (SIGSEGV + mprotect + cudaMemcpy).

**Typical result** (GTX 1650, 100 cycles, lr=0.05):

```
Erro:       0.480994 → 0.159982 (66.7% redução)
Precisão:   100.0%  (aleatório: 50.8%)
Migrações:  24 (pipeline-driven, not random)
Aprendizado: 2376 gradient updates
VRAM REAL:  64 faults em 0.646 ms (10.1 µs/fault)
```

Options: `--pages N`, `--cycles N`, `--lr F` (learning rate).

## Transparent Page Fault Handling

The `page-fault` command demonstrates **transparent migration** — the key
mechanism that makes the VMM invisible to applications:

1. Pages are allocated in a virtual region (mmap)
2. Pages are "evicted" to VRAM-sim (memfd) or FILE tier via `mprotect(PROT_NONE)`
3. When the app touches an evicted page → **SIGSEGV**
4. The handler transparently loads the page from its backend
5. `sigreturn` → the app retries the instruction → **success**

The application never knows its data was migrated. Measured:
- ~8 µs average fault latency
- 100% data integrity across RAM↔VRAM↔FILE round-trips
- 1536 faults handled in a single run with 0 failures

## Real VRAM Validation (CUDA)

The `run-cuda` and `validate-cuda` commands access **real GPU VRAM** via CUDA:

1. Allocates VRAM with `cudaMalloc`
2. Migrates data RAM→VRAM with `cudaMemcpy` (HostToDevice)
3. Processes data **in-place on the GPU** with a CUDA kernel
4. Verifies data integrity byte-by-byte
5. Runs the full UFTA pipeline with real VRAM as tier T1

Measured on GTX 1650:
- RAM→VRAM: ~5 GB/s (PCIe-limited)
- VRAM→RAM: ~1 GB/s (PCIe-limited)
- VRAM→VRAM: ~62 GB/s (internal GPU bandwidth)
- GPU kernel: ~3 GB/s in-place processing
- 100% data integrity across all tests

> **Note:** host↔VRAM transfers are constrained by the host-device interconnect
> (PCIe) and are therefore not representative of the GPU's internal VRAM bandwidth.
> The ~62 GB/s VRAM→VRAM figure reflects device-local bandwidth, while the
> ~5 GB/s RAM→VRAM figure reflects PCIe x16 Gen3 limits.

## Real VRAM Page Fault Migration (page-fault-cuda)

The `page-fault-cuda` command combines **real GPU VRAM** with the **transparent page fault handler** — the most advanced demo in the project:

1. Pages allocated and filled with verifiable patterns (RAM)
2. Half the pages evicted to **real GPU VRAM** via `cudaMemcpy` (Host→Device)
3. Quarter evicted to FILE tier, rest stay in RAM
4. All pages accessed → faults on GPU-backed pages trigger `cudaMemcpy` (Device→Host)
5. Data integrity verified byte-by-byte across all tiers

This proves **VRAM-backed virtual memory**: the CPU's virtual address space is transparently backed by GPU VRAM, with the page fault handler performing the migration on demand.

> **Architectural note:** the SIGSEGV handler performs CUDA work (cudaMemcpy)
> directly in the signal context. While this works for demonstration, POSIX
> signal handlers should be async-signal-safe ([signal(7)](https://man7.org/linux/man-pages/man7/signal-safety.7.html)).
> A production implementation should register the fault and delegate the
> CUDA transfer to a worker thread to avoid blocking or reentrancy issues
> with the CUDA runtime ([CUDA API behavior](https://docs.nvidia.com/cuda/cuda-runtime-api/api-sync-behavior.html)).

Measured on GTX 1650 (1650 CU):
- 384 page faults handled, 0 integrity failures
- ~9 µs average fault latency (incl. GPU memcpy)
- 128 real VRAM loads in a 256-page test
- Supports repeated eviction↔restore cycles

## Project Structure

```
ufta-vmm/
├── include/ufta/
│   ├── ufta.h          — umbrella include
│   ├── types.h         — core types (vec3, mat3, enums)
│   ├── state.h         — state engine (σ = m · σ̂)
│   ├── tier.h          — memory tier definition
│   ├── page.h          — virtual page & page table
│   ├── field.h         — field engine & operators
│   ├── predictor.h     — adaptive prediction with error
│   ├── migrate.h       — migration engine (cost/gain)
│   ├── pipeline.h      — 7-step runtime pipeline
│   └── backend.h       — persistent backends (.vmem)
├── src/
│   ├── state.c         — state/heat computation
│   ├── tier.c          — tier registry & bandwidth allocator
│   ├── page.c          — page table & address mapping
│   ├── field.c         — operator implementations
│   ├── predictor.c     — LMS-style rotation predictor
│   ├── migrate.c       — migration decision & execution
│   ├── pipeline.c      — pipeline step runner
│   ├── backend.c       — .vmem file I/O (binary & text)
│   └── main.c          — CLI entry point
├── Makefile
└── README.md
```

## Experimental Validation

The implementation has been validated in three progressively stronger
configurations:

1. **CPU-only virtual tier simulation** — predictor learns rotation R(θ)
   via LMS gradient descent with momentum; migration decisions compared
   against random selection.
2. **Transparent userspace page-fault migration** — SIGSEGV + mprotect
   handler transparently migrates pages between RAM, VRAM-sim, and FILE
   tiers with 100% data integrity.
3. **Real GPU VRAM-backed migration** — CUDA backend allocates real VRAM
   via `cudaMalloc`, migrates data with `cudaMemcpy`, and handles page
   faults with GPU-backed pages.

Intelligent migration reduced prediction error by 66.7% in the validated
learning workload and achieved 100% migration classification accuracy,
compared with 50.8% for random selection.

The CUDA backend additionally demonstrated real VRAM residency and
page-fault-driven RAM↔VRAM migration while preserving data integrity.

## Mathematical Foundation

The formalization follows the UFTA-VMM specification:

- **Master equation**: dσ/dt = O(σ, F) + P(σ, d̂) - C(σ)
- **Prediction**: σ̂_{t+Δt} = (σ_t + R(θ)·v_t·Δt) / ‖...‖
- **Bandwidth budget**: B_alloc = min(B_max, β, B_budget)
- **Adaptive error**: E_p(t) = ‖σ̂(t+τ) - σ̂_pred(t+τ)‖

