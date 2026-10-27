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
| Shadowlos desktop UI | `feat: Shadowlos theme and card layout from the Figma design` | **Low by design** — see below |
| Rule-set downloads go direct | `fix: download remote rule sets over the direct outbound` | Low — one `download_detour` key in `buildRuleSetArray` (`src/configs/generate.cpp`); re-apply if upstream rewrites that function. Must stay the legacy `download_detour`: `http_client{detour:"direct"}` is refused at start for a plain direct outbound. `core/internal/boxmain/ruleset_download_test.go` pins this; sing-box removes the legacy key in 1.16, so when a bump trips that test switch to a second direct outbound that carries a `domain_resolver` and point `http_client.detour` at it |
| AmneziaWG subscription links | `feat: import awg:// subscription links` | Low — `awgLink` plus a three-line dispatch in `Parser::link` (`src/configs/sub/SubscriptionParser.cpp`); re-apply if upstream rewrites `Parser::link`. Our subscription serves `awg://<base64 .conf>#name`, which upstream drops because it only knows `wg://` and `wireguard://`. The WireGuard parser already reads the Amnezia keys, so this only unwraps the base64 |
| Start retries over DoH when the system DNS times out | `feat: retry start over DoH when the system DNS times out` | Medium — edits upstream's `src/ui/mainWindow/mainwindow_profile_lifecycle.cpp` (one retry on a DNS timeout) and `src/configs/generate.{h,cpp}` (a direct-DNS override for that start). Only when the direct DNS is `localhost`; the retry uses `https://1.1.1.1/dns-query` and is not saved |
| "Throne Shadowlos" in the window title | `feat: name the build Throne Shadowlos in the window title` | Low — `software_name` in `src/ui/mainWindow/mainwindow_setup.cpp`; it also titles message boxes and the tray tooltip |
| Log holds still while read, pause button | `feat: log pause button, follow the log tail only at the bottom` | Low — three lines in upstream's `MainWindow::flush_log_batch` (`src/ui/mainWindow/mainwindow_log.cpp`): follow the tail only when the view is at the bottom and `masterLogBrowser` has no `slLogPaused`. The button itself is `installLogPause` in `ShadowlosChrome.cpp` |
| Subscription strip: plan only, details on click | `feat: plan-only subscription strip, Telegram app link, plain Russian mode names` | Medium — edits upstream's `src/ui/widget/SubscriptionInfoCard.cpp` in place (`planLine`, `intervalText`, `showDetails`, the plane glyph, layout order). Mode and tab names are only `ru_RU.ts` translations (`Tun Mode`, `System Proxy`, `Connections` in `MainWindow`) |
| Latency test that does not cry wolf | `fix: latency test over DoH, three targets, one retry, 8 s` | Medium — core `internal/probe/urltest.go` (`raceURLTest` needs the test URL plus one of `crossCheckURLs`, retry in `BatchURLTest`); `BuildTestConfig` in `src/configs/generate.cpp` sets the DoH override when the direct DNS is localhost; `url_test_timeout_ms` default 8000 and `TestRunner` RPC budget of 3 requests per tag; `applyTestDefaults` in `ShadowlosBootstrap.cpp` raises an old 3000 once |
| Start picks the fastest non-Russian server | `feat: start picks the fastest non-Russian server when none is selected` | Low — `bestProfileToStart`/`isRussianExit` (VLESS only, results under 30 min old; the last server stays while it works) ahead of upstream's fallbacks in `get_profile_to_start` (`mainwindow_profile_lifecycle.cpp`), plus one log line in `profile_start` |
| Routing profiles named for what they do, Debug in Tools | `routing: name the profiles «Все через VPN» and «Удобный»` | Low — `RouteProfile::DefaultChainName`, `renameRoutes` in `ShadowlosBootstrap.cpp`; `mainwindow_view.cpp` hides that name from the title. Debug moved from its card to `menuTools` in `ShadowlosChrome.cpp` |
| A copy of «Удобный» is the user's own and stays selected | `routing: a copy of «Удобный» is the user's own, and their pick survives a restart` | Low — `on_clone_route_clicked` in `src/ui/setting/dialog_manage_routes.cpp` drops the remote URL and auto-update from the copy (otherwise the daily refresh and «Обновить все» overwrite it); `applyRouting` in `ShadowlosBootstrap.cpp` selects «Удобный» only on a first install or when the chosen profile is gone (also on a revision bump or when «Удобный» was deleted and is recreated), and turns copies made before this change, which still carry «Удобный»'s URL, into local ones. The Windows/Linux «Гибкий» guide relies on both |
| Dev/prod flag, strict version, ShadowLos Throne exe names | `build: SHADOWLOS_DEV flag, strict X.Y.Z[-dev] version, ShadowLosThrone exe names` | Low — `cmake/nkr.cmake` (on Windows the version must be `X.Y.Z` or `X.Y.Z-dev`, `-dev` iff `-DSHADOWLOS_DEV=ON`; `SHADOWLOS_VERSION_CODE` = X*10000+Y*100+Z; exe `ShadowLosThrone.exe` / `ShadowLosThroneDev.exe`), the defines in `cmake/NkrVersion.h.in`, one `OUTPUT_NAME` in `CMakeLists.txt`, the app name (`%APPDATA%` fallback folder) in `src/main.cpp`, the accepted names in `core/internal/parentcheck`, `script/deploy_windows.sh`, and the `channel` input of the workflow. The User-Agent stays `Throne/<version>`: shadowlos recognises the client by that token |
| Self-update from our channel, «Обновить» button | `feat: self-update from the Shadowlos channel, signed, with rollback` | Low — the work is `core/internal/selfupdate` (`ThroneCore selfupdate check|apply`; ed25519 public key and the signed message `app_id|windows|version_code|size|sha256` in `verify.go`; apply journals to `update/apply.json`, rolls back a build that does not come up, never touches `config/`) and `src/ui/shadowlos/ShadowlosUpdater.cpp` (check 30 s after start then every 6 h, `SHADOWLOS_UPDATE_BASE` overrides the channel URL for tests, the button, a start that finds apply's `pending` marker writes `ok` once the core is up and resumes the profile that was running). Upstream-owned files carry hooks only: one dispatch line at the top of `core/main.go`; `CheckUpdate`/`isNewer` (throneproj GitHub, unsigned) deleted from `mainwindow_system.cpp` and replaced by `RequestSelfUpdate`, the `RunUpdater` exit branch calls `LaunchApply`; `TakeResumeProfile` before `core_process->Start()` in `mainwindow_setup.cpp`; the pill in `ShadowlosChrome.cpp`; `script/build_go.sh` no longer fetches throneproj's updater on Windows. The upstream "Check for updates" action stays hidden. Channel contract: shadowlos `mobile-api/deploy/desktop-update-channel.md` |
| Routing switch on the main window | `feat: routing switch «Удобный» / «Настройка под ваш регион» / «Все через VPN»` | Low — `src/ui/shadowlos/ShadowlosRouting.cpp` (three segments in the menu card, so also in the simple view; selecting is what the Routing menu does: `current_route_id`, save, restart a running connection). «Настройка под ваш регион» is a local profile we own (`shadowlos_regional_route`), refreshed daily while the VPN is off from shadowlos `GET /api/mobile/routing/regional?format=throne&platform=windows` with Sl-Sub auth (token = `user_id_hash` of the managed subscription, `shadowlos_install_id`); without a throne-route-profile answer it is a copy of «Удобный». Hooks: two settings in `SettingsRepo`, `RuleFromRoutingJson`/`ComfyRouteProfileName` exported from `ShadowlosBootstrap` |
| Dev-build error journal and upload | `feat: dev-build error journal, scrubbed, sent to client-logs` | Low — dev builds only (`SHADOWLOS_DEV`; prod gets empty inline hooks). The work is `src/ui/shadowlos/ShadowlosJournal.cpp` (Android journal format in `<config>/journal/journal.jsonl`, 2 MiB cap with a `gap` line; POST `/api/mobile/client-logs` with Sl-Sub auth every 3 min, contiguous `seq` packets ≤ 90000 chars, 429/503/413/4xx handling, backoff 6/12/24/30 min; a crash of the previous run sent once with its last 150 lines; the Tools action «Отправлять журнал ошибок разработчикам», `shadowlos_send_journal`, off clears everything) and `ShadowlosScrub.cpp` (the scrub.dart rules; `--journal-selftest`). Dialogs, taps and warning boxes come from an application event filter, so no upstream dialog is touched. Hooks: `Start()` and the self-test in `src/main.cpp`; `CoreLog` in the `MW_show_log` lambda (`mainwindow_setup.cpp`); `ConnectStart`/`ConnectEnd`/`Disconnect` in `profile_start`/`profile_stop` (`mainwindow_profile_lifecycle.cpp`); one setting in `SettingsRepo`. `ApiToken`/`InstallId` moved from ShadowlosRouting to `ShadowlosBootstrap`, with `ApiBase` (always `https://master.shadowlos.xyz`: the subscription host answers 404 to `/api/mobile/*`). Contract: shadowlos `mobile-api/deploy/windows-client-auth.md` |
| Servers from the archive when the subscription does not answer | `feat: start with the archive's servers while the group is empty` | Low — `ImportStaticProfilesIfEmpty` in `ShadowlosBootstrap.cpp`, called from `ShadowlosChrome::Install`: shadowlos.json `static_profiles` (the subscription body when the archive was built, shadowlos `lib/throne_zip.go`) goes into the managed group only while it is empty, queued before the startup refresh, which replaces it when it works. Never logged |
| Dev journal: where the user is (`net` lines, `geo`) | `feat: dev journal geo: net lines, whereami, geo in uploads` | Low — dev builds only, all in `src/ui/shadowlos/ShadowlosGeo.cpp` (prod has none of it: the file is empty without `SHADOWLOS_DEV`). The active adapter (GetAdaptersAddresses: up, with a gateway, lowest metric, not loopback/tunnel/virtual, so not our Wintun) gives `net_type` ethernet/wifi/other/none; the network is known only by a SHA-256 of adapter, gateway, DNS suffix and gateway MAC, kept in memory. shadowlos `GET /api/mobile/whereami` (Sl-Sub, `X-Shadowlos-App: shadowlos-throne-dev`, no proxy; skipped while connected in TUN mode) cached per network 30 min, 5 min after a failure; country, region, asn and org only, never a city (shadowlos `2feb9c58`). `app/net` lines with a nested `geo` at start +6 s, each connect and a network change (checked every 15 s); every upload carries the latest `geo`. `SanitizeGeo` is the only way in or out (server key set minus `city`, no IP/host/link, org that is not plain text becomes `AS<asn>`): `ScrubJsonLine` only scrubs top-level strings. Hooks: `InstallGeo` from `Journal::Install`, `GeoConnectStart` from `Journal::ConnectStart`; no upstream file touched. Contract: shadowlos `mobile-api/deploy/whereami.md`, `handlers/client_logs.go` (`clientLogGeo`) |
| Dev blocking badge (census) | `feat: dev blocking badge: census verdict, region and operator` | Low — dev builds only, all in `src/ui/shadowlos/ShadowlosCensus.cpp` (empty without `SHADOWLOS_DEV`; prod `MakeBlockingBadge` is an inline `nullptr`). The Android census (shadowlos `app/lib/modules/probes/network_class.dart`, a lift of `network_class_probe.dart` `classifyNetwork`): direct TLS handshakes (`QNetworkProxy::NoProxy`) to the control pair, five foreign and five resilient hosts, one 3 s pass and at most one 7 s re-ask merged over it, never while connected in TUN mode, and only after a torn-down `throne-tun` is gone (≤ 15 s; a TUN connect seen by `ConnectCount` counts even when it was over before a tick). The network key is read fresh (`FreshNetworkKey`) when a measurement starts and when it lands; a changed key drops it. Probe sockets load the system CA store up front, so OpenSSL never waits on the on-demand root lookup. A real verdict (none/regular/whitelist) is kept per network (`CurrentNetworkKey` from `ShadowlosGeo.cpp`), «unknown» falls back to it, «no internet» is shown but not kept. Badge words are the Android dev badge's (`home_screen.dart` `blockingNoteLabel`, `badge_geo.dart` `badgeText`/`shortOrg`), joined with « · »; an `app/census` journal line only when the verdict or tier changes (counts only, no hosts); without a real verdict it re-measures after 60 s, 120 s, then every 300 s. Hooks: one `MakeBlockingBadge` call in `ShadowlosChrome.cpp` (a fixed-height row on top of the status panel, under the routing switch: the menu card has no room at the default width; prod gets a column with `data_view` alone), `QLabel[slRole="badge"]` in `shadowlos.qss`; the verdict cases run in `--journal-selftest` |

### Shadowlos provisioning

Reads `config/shadowlos.json` (shipped inside the per-user archive) and
provisions the subscription group on every launch. Fields: `subscription_url`,
`group_name`, `auto_update_minutes`, `enable_tun`, `routing`, and `static_profiles`
(the subscription body at archive time, imported only into an empty group). See
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

### Shadowlos desktop UI

The look from the "Windows" page of the `Shadolos APK` Figma file. It is the default
theme ("Shadowlos" in Settings -> Theme); every install is switched to it once, gated by
the `shadowlos_ui_revision` setting, and a user who picks another theme keeps it.

Almost all of it lives in files upstream will never have:

- `res/shadowlos/shadowlos.qss` — the stylesheet. Paints every window, dialog, menu,
  form and table, so no upstream `.ui` file is edited for the look.
- `res/shadowlos/icons/` (PNGs at 1x and @2x, rendered from SVG; Windows' static Qt may
  lack the SVG plugin), `res/shadowlos/fonts/PTRootUI-VF.ttf` (ParaType's unmodified
  variable font, OFL), `res/shadowlos.qrc`.
- `src/ui/shadowlos/ShadowlosChrome.cpp` — rebuilds the main window into the design's
  cards at runtime by re-parenting the widgets uic made, so `mainwindow.ui` stays
  upstream's. Also the power button (a `StartStopButton` subclass that only repaints),
  the connected-row highlight, latency colours, and the dark Windows caption.

Upstream-owned files carry only one-line hooks:

- `CMakeLists.txt` — the two sources, `res/shadowlos.qrc`, and `SKIP_UNITY_BUILD_INCLUSION` for `ShadowlosChrome.cpp` (a unity batch-mate that defines `MW_INTERFACE` hides `Ui::MainWindow`)
- `ThemeManager.cpp` — the `shadowlos` palette, its sheet path, and `ApplyThemeFont()`
- `mainwindow_setup.cpp` — `Shadowlos::Chrome::Install(this, ui)` right after
  `setupUi`, and `shadowlos` in `themeUsesDarkLog`
- `mainwindow_view.cpp` — the `slVariableWidthMenus` early return in
  `applyTopBarMetrics`, and `StatusLine()` around the three status-label `setText`s
- `dialog_basic_settings.cpp` — the "Shadowlos" theme entry
- `SettingsRepo.h/.cpp` — `shadowlos_ui_revision`, `shadowlos_simple_view` (the "Упрощённый вид" switch, its own card right of Debug: hides the Type and Address columns and the logs/connections panel, and shows bigger rows with the columns packed left). ShadowlosChrome also runs the group latency test once at startup, when the core is up.

If upstream renames or removes a widget `Install()` moves (the five menu buttons,
`toolButton_startstop`, `toolButton_update_subs`, `toolButton_debug`, the three mode
checkboxes, `data_view`, `splitter`, the three status labels, or the layouts
`horizontalLayout_2`, `verticalLayout_3`, `verticalLayout_4`, `horizontalLayout`), the
build fails there rather than at runtime — fix `Install()` to match.

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
         ApplyBootstrap shadowlos_managed_group WantsTunOnStart DebugCheck Shadowlos.exe \
         "Chrome::Install" ApplyThemeFont slVariableWidthMenus StatusLine shadowlos_ui_revision; do
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
| 2026-10-07 | `1.3.2` | `1.3.1` | 14 commits. One conflict, `ru_RU.ts`: both sides appended messages at the same spot, resolved as a union (no duplicate sources). `build_go.sh` and upstream `build.yml` unchanged, so the workflow was not re-derived. 1.3.2's `SubscriptionInfoCard` lives in the profile table header. 1.4.0-beta.1 skipped as a pre-release. |
