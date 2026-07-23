# Fork maintenance

This is a permanent hard fork of [throneproj/Throne](https://github.com/throneproj/Throne),
maintained for Shadowlos. We pull upstream releases in periodically; we never
send anything back. Nothing here is written with upstream mergeability in mind.

`dev` is our product branch. Upstream is a source of updates, not a base we
track — no local branch tracks `upstream/*`.

## Our changes

| Area | Commits | Conflict risk on bump |
| --- | --- | --- |
| Auto-update subscriptions on open | `auto update`, `Added updating + better buttons` | **High** — edits `mainwindow.cpp`, `GroupUpdater.cpp`, `mainwindow_rpc.cpp` in place |
| VLESS config checking | `Added checking of vless configs` | Medium — adds a Go RPC (`DebugCheck`) plus UI wiring |
| Russian translations | `Added translations` | Medium — `ru_RU.ts`, and wraps upstream strings in `tr()` |
| Windows build / icons | `windows build`, `hide update client button` | Low |
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
for s in "UI_update_all_groups(false)" actionDebug_Check_All_Vless \
         toolButton_update_subs ApplyBootstrap shadowlos_managed_group; do
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
