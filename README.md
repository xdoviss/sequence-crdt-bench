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
