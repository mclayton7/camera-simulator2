# CI Runner Setup (Phase 28A)

CamSim's UE5-dependent CI jobs (`unit-tests`, `integration-test`,
`docker-build`, `docker-release`) run on a **self-hosted GitHub Actions runner**
labelled `camsim-ue5`. This document covers one-time setup.

## What the runner needs

| Requirement                                 | Why                                                                |
| ------------------------------------------- | ------------------------------------------------------------------ |
| Linux x86_64 (Ubuntu 22.04 tested)          | Matches `deploy/Dockerfile` base image                             |
| UE5.7 at `/opt/UE/Engine/Binaries/Linux/UnrealEditor` | `scripts/run.sh` and `scripts/package_for_docker.sh` auto-discover via `find /opt -path '*/Binaries/Linux/UnrealEditor'` |
| Docker + Docker Buildx                      | `integration-test` runs `docker compose up`, `docker-release` pushes to GHCR |
| Python 3.10+                                | `scripts/parse_automation_report.py` (stdlib only)                 |
| Build deps: `cmake nasm yasm pkg-config libx264-dev libssl-dev` | `scripts/build_thirdparty_linux.sh` first run                 |
| ~80 GB free disk                            | UE5 install (~50 GB) + DDC cache + Docker layers + staged packages |

## One-time registration

1. **Provision UE5.7** at `/opt/UE` on the host. If it's already a CamSim
   development box, this is already done; otherwise install via Epic Games
   Launcher or the official Linux tarball and symlink:
   ```bash
   sudo ln -s /opt/UnrealEngine-5.7 /opt/UE
   ```
   Verify:
   ```bash
   /opt/UE/Engine/Binaries/Linux/UnrealEditor -version
   ```

2. **Install Docker** (rootless or with the runner user in the `docker` group):
   ```bash
   sudo apt-get install -y docker.io docker-buildx
   sudo usermod -aG docker "$USER"
   newgrp docker
   docker version
   ```

3. **Register the GitHub Actions runner.** From the repo on github.com:
   _Settings → Actions → Runners → New self-hosted runner → Linux_.
   Follow the on-screen `./config.sh ...` and `./run.sh` instructions, but
   **add `--labels camsim-ue5`** to `./config.sh`:
   ```bash
   ./config.sh --url https://github.com/<owner>/<repo> \
               --token <token from UI> \
               --labels camsim-ue5 \
               --unattended
   ```

4. **Install the runner as a systemd service** so it survives reboots:
   ```bash
   sudo ./svc.sh install
   sudo ./svc.sh start
   sudo systemctl status actions.runner.<owner-repo>.<host>
   ```

5. **Flip the kill-switch variable.** From a host with `gh` authenticated:
   ```bash
   gh variable set CAMSIM_UE5_RUNNER_AVAILABLE --body true \
       --repo <owner>/<repo>
   ```
   This unlocks the four gated jobs in `.github/workflows/ci.yml`.

6. **Smoke test.** Open a draft PR with a trivial change (whitespace edit, doc
   typo). The `unit-tests` job should pick up within ~30 s, build the editor
   target on the runner, and run all 185 tests in ~10 s.

## What the jobs do on the runner

- `unit-tests` (every PR + push) — compiles the editor target via
  `scripts/run.sh --build-only`, runs the headless automation suite, parses
  `.cache/automation-report/index.json`. Wall time: ~30 s warm, ~5 min cold.
- `integration-test` (push-to-`main` only) — calls
  `scripts/package_for_docker.sh` (BuildCookRun → `deploy/staged/LinuxNoEditor/`),
  then `docker compose up`, then `scripts/ci_validate.sh`. Wall time:
  10-30 min depending on DDC cache.
- `docker-build` / `docker-release` — same packaging step, then a Docker
  image build (no push for `docker-build`, GHCR push for `docker-release`
  on tag pushes).

Every job ends with a `Cleanup` step (`if: always()`) that `rm -rf`s
`deploy/staged/` and `.cache/automation-report/` to keep the runner tidy.
The DDC cache at `.cache/ue-ddc/` is preserved across runs to amortise
shader compilation.

## Disk-space monitoring

Recommended cron (root, weekly):
```bash
df -h /opt /var/lib/docker
du -sh /opt/UE /opt/actions-runner/_work/*/_work/*/.cache 2>/dev/null
docker system prune -af --filter "until=168h"
```
Set a Prometheus / `node_exporter` alert on `node_filesystem_avail_bytes`
below 20 GB on the partition holding `_work/`.

## Troubleshooting

| Symptom                                                       | Fix                                                                                              |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Job queues forever, never assigned                            | Check `systemctl status actions.runner.*`; verify the runner is online in repo Settings → Runners |
| `UnrealEditor not found` in `Locate UnrealEditor` step        | Either `/opt/UE` symlink missing, or runner user lacks read access on `/opt/UE/Engine/Binaries/Linux/UnrealEditor` |
| `scripts/build_thirdparty_linux.sh` fails on apt packages     | Runner user needs `sudo` rights or a one-time pre-install of `cmake nasm yasm pkg-config libx264-dev libssl-dev` |
| `parse_automation_report.py` exits 2 ("report missing")       | Shader compile or harness crash before tests ran; check `.cache/automation-report/` artifact (uploaded on `always()`) and the live job log |
| Packaging step times out at 45 min on cold DDC                | Pre-warm with `scripts/prewarm_shaders.sh` on the runner once, or extend `timeout-minutes` in `ci.yml` |
| Disk fills mid-run on the runner                              | Add `docker system prune -af` to the cleanup step, or shrink DDC retention |

## Disabling CI

To skip the four UE5-dependent jobs (e.g. while the runner is being
re-imaged) without editing the workflow:
```bash
gh variable set CAMSIM_UE5_RUNNER_AVAILABLE --body false --repo <owner>/<repo>
```
The `lint`, `build-thirdparty`, and `python-tests` jobs continue to run on
`ubuntu-latest` regardless.
