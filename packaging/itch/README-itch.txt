# UFTA-VMM — Universal Field Transformation Architecture

**Virtual Memory Manager** — prediction-driven, field-aware memory migration system.

---

## Quick Start

### GUI Dashboard (recommended)
```bash
./ufta-vmm.sh gui
```

### Command Line
```bash
./ufta-vmm validate          # Run validation suite
./ufta-vmm migrate           # Run migration demo
./ufta-vmm                   # Show all options
```

---

## System Requirements

- **OS:** Linux (x86_64)
- **GPU:** OpenGL-capable (for GUI dashboard)
- **RAM:** 512 MB minimum

All dependencies (SDL2) are included in this package. No installation needed.

---

## What is UFTA-VMM?

UFTA-VMM is a research-grade Virtual Memory Manager that uses a 7-step
prediction pipeline to intelligently manage memory across multiple tiers
(RAM → VRAM → NVMe → USB → FILE).

**Pipeline:** Observe → Represent → Predict → Prioritize → Throttle → Migrate → Commit

**Key Features:**
- LMS predictor with rotation matrices for access pattern prediction
- Field-driven priority scoring
- Multi-tier memory hierarchy management
- Real-time GUI dashboard with OpenGL rendering
- Worker thread architecture for async migration

---

## Controls (GUI)

| Key | Action |
|-----|--------|
| 1-5 | Switch tier view |
| P   | Pause/Resume simulation |
| R   | Reset statistics |
| ESC | Quit |

---

## License

Proprietary — All Rights Reserved.

© 2026 UFTA-VMM Project
