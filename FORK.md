# Fork maintenance

This is a permanent hard fork of [throneproj/Throne](https://github.com/throneproj/Throne),
maintained for Shadowlos. We pull upstream releases in periodically; we never
send anything back. Nothing here is written with upstream mergeability in mind.

`dev` is our product branch. Upstream is a source of updates, not a base we
track — no local branch tracks `upstream/*`.

## Our changes

| Area | Commits | Conflict risk on bump |
| --- | --- | --- |
| Auto-update subscriptions on open, "Update Conf" button | `auto update`, `Added updating + better buttons` | **High** — edits `src/ui/mainWindow/mainwindow_setup.cpp` and `mainwindow_deeplink.cpp` in place |
| VLESS config checking | `Added checking of vless configs` | Medium — a Go RPC (`core/internal/rpc/debugcheck.go`, `core/internal/probe/debug_check_utils.go`, one `dispatch.go` line, proto messages) plus UI in our own `src/ui/mainWindow/mainwindow_debugcheck.cpp` |
| Russian translations | `Added translations` | Medium — our strings appended to upstream's `ru_RU.ts`; merge by (context, source), keep upstream's translations |
| Slim core | `script/build_go.sh` drops `with_tailscale`, `with_openvpn`, `with_openconnect` and NaiveProxy (`with_naive_outbound`, `with_purego`, `libcronet.dll`) on Windows and Linux | Low — a few lines; re-apply if upstream rewrites them. The archive is sent as a Telegram document, capped at 50MB; shadowlos's `TestThroneArchivesFitTelegramUploadLimit` fails first if it grows past that |
| Desktop build / icons | `windows build`, `hide update client button` | Low — `.github/workflows/windows64-artifact.yml` (name kept so it stays dispatchable; it builds Linux too) is upstream's `build.yml` cut to windows-amd64 + linux-amd64; re-derive it from `build.yml` each bump |
| Shadowlos provisioning | `provision Shadowlos subscription from shadowlos.json` | **Low by design** — see below |

### Shadowlos provisioning

Reads `config/shadowlos.json` (shipped inside the per-user archive) and
provisions the subscription group on every launch. See
`src/global/ShadowlosBootstrap.cpp`.

Deliberately built to survive bumps: 148 of its 156 lines live in two files
upstream will never have. It touches upstream-owned files in only **8 added
lines**, all pure insertions:

- `CMakeLists.txt` — 2 lines adding the sources
- `src/global/Configs.cpp` — include + `ApplyBootstrap()` call at the end of `initDB`
- `include/database/SettingsRepo.h` + `src/database/SettingsRepo.cpp` — one
  `shadowlos_managed_group` int setting

Two traps to avoid if this is ever refactored:

- **Do not store the marker in `Group::info`.** `GroupUpdater.cpp` overwrites it
  with the `Subscription-UserInfo` header on every successful update, and the
  UI parses it for quota display. That is why the group id lives in `settings`.
- **Do not add a column to the `groups` table.** It would mean editing four
  places in `GroupsRepo.cpp` and re-resolving them on every bump.

The archiver side lives in `shadowlos` at `lib/throne_zip.go`. The JSON field
names are the contract between the two — change both together.

## Bumping to a new upstream release

```bash
git fetch upstream --tags
git tag --sort=-creatordate | head          # pick a release tag, not upstream/dev
git branch backup-dev-pre-bump-$(date +%Y%m%d) dev
git checkout -b bump/upstream-<version> dev
git rebase --onto <version> <previous-base> bump/upstream-<version>
```

`<previous-base>` is the upstream commit the current `dev` was built on. Record
it below each time so the next bump does not have to go looking for it.

Prefer a release tag over `upstream/dev` — a moving dev head makes a fork's
history hard to reason about later.

After resolving conflicts, verify our features survived the replay. The rebase
merges much of this silently, so grep rather than trust it:

```bash
for s in "RefreshAll(false)" actionDebug_Check_All_Vless toolButton_update_subs \
         ApplyBootstrap shadowlos_managed_group WantsTunOnStart DebugCheck Shadowlos.exe; do
  printf '%-32s -> ' "$s"; grep -rl "$s" src/ include/ core/ | tr '\n' ' '; echo
done
```

Then build before letting it near `dev`:

```bash
./build_local.sh
```

Landing it rewrites history, so `dev` needs a force push:

```bash
git push --force-with-lease origin bump/upstream-<version>:dev
```

Use `--force-with-lease`, never `--force` — it aborts if someone else pushed to
`dev` since your last fetch.

### Bump log

| Date | Upstream base | Previous base | Notes |
| --- | --- | --- | --- |
| 2026-07-23 | `1.2.0` | `ed7f6dcc` | 97 commits. Two conflicts, both include-block collisions (`mainwindow.ui`, `mainwindow_rpc.cpp`), resolved as unions. |
| 2026-09-28 | `1.3.1` | `1.2.0` | 215 commits. Upstream split `mainwindow.cpp`/`mainwindow_rpc.cpp` into `src/ui/mainWindow/*`, moved the core to `core/internal/{rpc,probe,parentcheck}`, and replaced `UI_update_all_groups` with `Subscription::updater()->RefreshAll()`. Every UI patch was hand-ported; `CheckNaive` (unused, always true) dropped. Toolchain moved to Go 1.27 / Qt 6.11.2, hence the re-derived workflow. |
