# OrcaSlicer — Bambu Cloud Build

> See also [AGENTS.md](AGENTS.md) for code style, key entry points, and critical constraints inherited from the upstream project.

## What this is

A personal fork based on [FULU-Foundation/OrcaSlicer-bambulab](https://github.com/FULU-Foundation/OrcaSlicer-bambulab) (itself a repost of a now-removed repo), which adds full Bambu cloud networking support on top of [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer). The goal is a portable Windows build that supports **full Bambu cloud capabilities** — including pushing custom filaments (not just profiles) up to the cloud so they land on the printer. This is functionality the stock OrcaSlicer build does not expose.

The host machine is an Ubuntu 24.04 VM on Hyper-V with 16 GB RAM allocated. Target printer: **Bambu Lab P2S**.

---

## Branch / Remote Structure

| Name | What it is |
|---|---|
| `local/v2.4.2` | **Active working branch.** Always build from this. |
| `origin` | User's fork: `https://github.com/dmuth23/Orca-Bambu` |
| `origin/release/v2.4.2` | Remote counterpart of `local/v2.4.2` |
| `upstream` | Official OrcaSlicer: `https://github.com/SoftFever/OrcaSlicer` |
| `upstream/release/v2.4` | Upstream branch to monitor for new commits |
| `fulu` | FULU Foundation repo: `https://github.com/FULU-Foundation/OrcaSlicer-bambulab` |
| `fulu/main` | Base of our v2.4.2 branch |

Never work on `main`. The old `release/v2.4` branch has the Claude-generated bridge (non-functional) — leave it alone.

**Note:** FULU's repo has no shared git history with upstream OrcaSlicer (it was created as a fresh snapshot). Upstream patches must be reviewed and applied manually via `git diff` + `git apply`.

---

## The Bambu Cloud Bridge

The custom code lives in:

```
src/slic3r/Utils/PJarczakLinuxBridge/   — bridge shim (DLL/SO loaded by OrcaSlicer)
shared/pjarczak_linux_plugin_bridge_core/ — core RPC protocol library
shared/BambuBridge/                       — auth layer
```

How it works:
- On **Windows**: OrcaSlicer loads `pjarczak_bambu_networking_bridge.dll`, which spawns a WSL2 distro containing the Linux host binary. All Bambu cloud calls go over JSON-RPC to that host, which runs Bambu's real Linux `.so`.
- On **Linux**: The host binary runs natively (opt-in via `PJARCZAK_BRIDGE_ENABLED=1`).
- On **macOS**: Uses Lima VM instead of WSL2.

**Windows requires WSL2.** On first launch, OrcaSlicer installs a small WSL2 distro automatically via a PowerShell script.

Key files modified in core OrcaSlicer:
- `src/slic3r/Utils/BBLNetworkPlugin.cpp` — loads the bridge instead of stock Bambu DLL
- `src/slic3r/Utils/BBLCloudServiceAgent.cpp` — extended cloud agent
- `src/slic3r/GUI/Plater.cpp` — bridge initialization

**Golden rule: never let any merge overwrite anything in `PJarczakLinuxBridge/`, `shared/`, or the bridge hook points in `BBLNetworkPlugin.cpp`.**

---

## Building (Windows — primary target)

Builds happen via **GitHub Actions**, not locally. Push to `release/v2.4.2` and the `build_windows_bridge.yml` workflow runs automatically.

### What the Windows build does
1. **Job 1 (Ubuntu):** Compiles the Linux host binary + WSL2 rootfs, uploads as artifacts
2. **Job 2 (Windows):** Downloads Linux artifacts, builds OrcaSlicer with MSVC, packages as portable ZIP + installer

### Triggering a build
```bash
git push origin local/v2.4.2:release/v2.4.2
```
Then go to **github.com/dmuth23/Orca-Bambu/actions** and watch it run. Or trigger manually from the Actions tab.

### Output
Download `OrcaSlicer_Windows_V2.4.2_portable.zip` from the Actions artifacts. Extract anywhere and run.

### Windows runtime requirement
WSL2 must be enabled. On first OrcaSlicer launch, it will prompt to install the WSL2 runtime automatically. If WSL2 isn't enabled yet:
```
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```
Then restart Windows.

---

## Linux Build (optional / testing)

```bash
# First time only
./build_linux.sh -u   # system packages
./build_linux.sh -d   # C++ deps (30-60 min)

# Regular rebuild
./build_linux.sh -s -i -j 3
```

Output: `build/OrcaSlicer_ubu64.AppImage`

---

## Upstream Update Workflow

### Check for new upstream commits (read-only)
```bash
./check-upstream.sh
```

### Apply upstream patches manually
Since FULU has no shared git history with upstream, patches must be applied manually:
```bash
# See what changed in a specific upstream commit
git show upstream/release/v2.4:<file>

# Generate a patch and apply it
git diff <base>..<commit> -- <file> | git apply
```

### Pushing to GitHub
```bash
git push origin local/v2.4.2:release/v2.4.2
```

---

## Bambu Network Plugin

OrcaSlicer downloads Bambu's proprietary network library automatically the first time you connect a printer. It lands in the OrcaSlicer data directory under `plugins/`. The bridge wraps this downloaded library to expose full cloud capabilities including custom filament sync.

---

## Hard Rules

- **No `git push --force`** under any circumstances
- **No `git reset --hard`** without explicit user confirmation
- **Never delete or overwrite** `src/slic3r/Utils/PJarczakLinuxBridge/` or `shared/`
- **Never rebase** the working branch — merge only
