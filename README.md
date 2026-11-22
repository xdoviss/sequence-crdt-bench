# Sequence CRDT bench

| Algorithm | Identifier | Deleted elements | Needs causal delivery |
|---|---|---|---|
| **WOOT** | (site, clock) + neighbours | tombstones | no |
| **Logoot** | dense position, base 2^32 | removed | yes |
| **RGA** | Lamport timestamp + reference element | tombstones | no |
| **LSEQ** | dense position, base 2^(4+d) | removed | yes |
| **Treedoc** | binary-tree path + disambiguators | tombstones | yes |

## Prerequisites

- C++17 compiler + CMake 3.16+
  - Windows: Visual Studio w/ C++ desktop workload
  - macOS: `brew install cmake`
  - Linux: `sudo apt install build-essential cmake`
- Python 3 + `python -m pip install -r scripts/requirements.txt`

## Running

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1 # quick
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1 -Mode full # full
powershell -ExecutionPolicy Bypass -File scripts\run_all.ps1 -BuildDir build-msvc -Python C:\Python312\python.exe # custom build dir
```

### Linux / macOS (bash)

```bash
bash scripts/run.sh # quick
bash scripts/run.sh full # full
bash scripts/run.sh full build-gcc # custom build dir
```