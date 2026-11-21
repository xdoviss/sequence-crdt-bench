# Comparative Evaluation of Sequence CRDTs in C++

Implementation, testing and benchmarking framework for five sequence
Conflict-free Replicated Data Types used in collaborative text editing:

| Algorithm | Identifier | Deleted elements | Needs causal delivery |
|---|---|---|---|
| **WOOT** (Oster et al. 2006) | (site, clock) + neighbours cp/cn | tombstones | no (own pending pool) |
| **Logoot** (Weiss et al. 2009) | dense position, base 2^32, boundary+ | removed | yes |
| **RGA** (Roh et al. 2011) | Lamport timestamp + reference element | tombstones | no (own pending pool) |
| **LSEQ** (Nédelec et al. 2013) | dense position, base 2^(4+d), boundary± | removed | yes |
| **Treedoc** (Preguiça et al. 2009) | binary-tree path with disambiguators | tombstones, leaves collected | yes |

All five sit behind one C++17 interface (`SequenceCRDT`) and are driven by the
same replica model, network simulator, workload generator, test suite and
benchmark tools, so they are compared under identical conditions.

* No third-party C++ dependencies; builds with MSVC, GCC and Clang on
  **Windows, Linux and macOS** using CMake.
* Python 3 + `pandas` + `matplotlib` are needed **only** for the graphs.
* No database: results are exported as CSV + JSON (see [Outputs](#outputs)).

Documentation:

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — design and implementation details of every component and algorithm
* [docs/METHODOLOGY.md](docs/METHODOLOGY.md) — experimental methodology, metrics and reproducibility
* [docs/EVALUATION.md](docs/EVALUATION.md) — the comparative evaluation (course-paper ready)
* [docs/references.bib](docs/references.bib) — 18 peer-reviewed references with DOIs and free full-text links
  (BibTeX; import into Zotero via *File → Import*, or into Mendeley / JabRef)
* [PROGRESS.md](PROGRESS.md) — plan and progress tracker

---

## 1. Prerequisites

| | Windows | Linux | macOS |
|---|---|---|---|
| C++17 compiler | Visual Studio 2019, 2022 or 2026 with *Desktop development with C++* (or Build Tools for VS) | GCC ≥ 9 or Clang ≥ 10 | Xcode Command Line Tools (Apple Clang ≥ 12) |
| CMake ≥ 3.16 | included with Visual Studio, or `winget install Kitware.CMake` | `sudo apt install cmake` / `sudo dnf install cmake` | `brew install cmake` |
| Python 3.8+ (graphs only) | `winget install Python.Python.3.12` | usually preinstalled | `brew install python` |

Install the Python packages once (all platforms):

```bash
python -m pip install -r scripts/requirements.txt # Linux/macOS may need: python3 -m pip ...
```

Platform setup in detail:

**Windows** — install Visual Studio Community (2022 or 2026) with the workload
*Desktop development with C++* (it contains MSVC, CMake and Ninja). Use the
*Developer PowerShell for VS* or a normal PowerShell (CMake finds Visual
Studio automatically).

**Linux (Debian/Ubuntu)**

```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build python3 python3-pip
```

Fedora: `sudo dnf install -y gcc-c++ cmake ninja-build python3-pip`.

**macOS**

```bash
xcode-select --install          # compiler
brew install cmake ninja python # https://brew.sh
```

---

## 2. Build

The commands are the same on every OS (run them in the project folder,
e.g. `D:\prj\kursinis`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Where the executables end up:

| Generator | Binaries |
|---|---|
| Visual Studio (Windows default) | `build\Release\crdt_bench.exe`, `build\Release\crdt_tests.exe` |
| Ninja / Unix Makefiles (Linux, macOS default) | `build/crdt_bench`, `build/crdt_tests` |

Optional CMake switches: `-DCRDT_SANITIZERS=ON` (ASan + UBSan, GCC/Clang),
`-DCRDT_NATIVE=ON` (`-march=native`), `-DCRDT_WARNINGS_AS_ERRORS=ON`.
To use Ninja on Windows add `-G Ninja` (from a Developer PowerShell).

---

## 3. Run everything with one command

The scripts build in Release mode, run the tests, run all nine experiments and
generate graphs and tables.

**Windows (PowerShell)**

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1 -Mode quick   # ~5 minutes
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1 -Mode full    # spec-sized, ~45-90 minutes
```

**Linux / macOS**

```bash
scripts/run_all.sh quick     # ~5 minutes
scripts/run_all.sh full      # spec-sized, ~45-90 minutes
```

`quick` uses `configs/quick/*.ini` (fewer repetitions and smaller volumes);
`full` uses `configs/full/*.ini` (document sizes up to 1,000,000 elements, up to
50 replicas, up to 100,000 operations per replica). Results go to
`results/quick/` or `results/full/`.

---

## 4. Run individual steps

Paths below use `build/`; on Windows with the Visual Studio generator use
`build\Release\` and `.exe`.

**Tests**

```bash
build/crdt_tests                                   # all tests
build/crdt_tests convergence                       # only tests whose name contains "convergence"
build/crdt_tests --json results/tests_summary.json # machine-readable summary
ctest --test-dir build -C Release                  # through CTest
```

**Experiments**

```bash
build/crdt_bench env                                         # print compiler / OS / CPU / RAM
build/crdt_bench list                                        # algorithms and workload patterns
build/crdt_bench run configs/quick/micro.ini --out results/quick
build/crdt_bench run configs/full/network.ini --out results/full --set experiment.replicas=20
```

`--set section.key=value` overrides any configuration value without editing
the file (e.g. `--set micro.algorithms=rga,lseq`).

| Config | Type | What it measures (spec section) |
|---|---|---|
| `micro.ini` | micro | insert / delete / lookup / traversal / merge / (de)serialization latency vs document size (6.1, 7) |
| `merge.ini` | merge | merge latency and synchronization CPU time vs concurrent replicas (6.2, 7.1) |
| `memory.ini` | memory | memory vs document size, vs operation count, tombstones, insert-delete cycles, retention (5) |
| `workloads.ini` | sim | the nine editing patterns through the network simulator (6.4) |
| `network.ini` | sim | LAN / WAN / jitter-reordering / FIFO / packet loss + retransmission (8) |
| `sync_intervals.ini` | sim | synchronization interval / batching (3.3, 8) |
| `scale_replicas.ini` | sim | 2 – 50 concurrent users (6.2, 10) |
| `scale_ops.ini` | sim | 1,000 – 100,000 operations per user (6.3) |
| `intensity.ini` | sim | throughput vs offered load, editing bursts (7.3, 8) |

**Graphs and tables**

```bash
python scripts/analyze.py results/quick
```

---

## 5. Outputs

For every experiment `<name>`:

* `<name>.csv` (and e.g. `micro_sizes.csv`, `merge_sync.csv`) — one row per
  measurement with **n, mean, median (p50), p95, p99, min, max, standard
  deviation and 95 % confidence-interval half-width**;
* `<name>.meta.json` — compiler and version, build type and flags, OS, CPU
  model, core count, RAM, timestamp, the complete configuration (including
  seeds).

`analyze.py` adds `figures/*.png` (all graphs required by spec §10 and more),
`tables.md` (all tables) and `findings.json` (per-criterion rankings).
`tests_summary.json` holds the number of tests, assertions, property cases,
failures, convergence checks and convergence violations.

---

## 6. Project layout

```
include/crdt/common/      shared types, codec, operation envelope, SequenceCRDT interface,
                          order-statistic index, RNG, statistics, platform helpers
include/crdt/algorithms/  woot.hpp  rga.hpp  logoot.hpp  lseq.hpp  treedoc.hpp
include/crdt/sim/         replica.hpp (causal delivery)  network.hpp (simulator)  workload.hpp
src/common/  src/algorithms/  src/sim/      implementations (static library crdt_core)
src/bench/                crdt_bench: config parser, CSV/JSON export, heap tracker, experiments
tests/                    crdt_tests: self-contained test framework + test suites
algorithms_pseudo/        pseudocode of each algorithm as implemented (woot, rga, logoot, lseq, treedoc)
configs/quick/  configs/full/               reproducible experiment configurations
scripts/                  run_all.sh, run_all.ps1, analyze.py, requirements.txt
docs/                     architecture, methodology, evaluation
results/                  generated results (quick / full)
```

---

## 7. Reproducibility notes

* All randomness uses a fixed, portable PRNG (xoshiro256**) seeded from the
  config files, so the *logical* outcome of every experiment (documents,
  message counts, bytes, identifier depths) is identical on every platform.
  Timings naturally depend on the machine and are recorded with the full
  environment description.
* Build in **Release** mode; do not compare results from different compilers
  or machines without documenting the difference (the `.meta.json` files do
  this automatically).
* Close other applications while running benchmarks. On laptops use mains
  power and a fixed performance profile.

## Troubleshooting

| Problem | Fix |
|---|---|
| `cmake` not found (Windows) | `run_all.ps1` finds the CMake bundled with Visual Studio by itself. If it still fails, open the Visual Studio Installer → *Modify* → *Desktop development with C++* and tick *C++ CMake tools for Windows*; or install CMake with `winget install Kitware.CMake` |
| PowerShell refuses to run the script | run it as shown with `-ExecutionPolicy Bypass` |
| `ModuleNotFoundError: pandas` | `python -m pip install -r scripts/requirements.txt` |
| macOS: `xcrun: error: invalid active developer path` | `xcode-select --install` |
| Very long `full` run | run single configs, or lower volumes with `--set` |
