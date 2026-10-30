# Windows client: errors and warnings in Russian

Branch `feat/errors-ru` (based on dev 8bf72fa6). Every user-facing error and warning of the
ShadowLos Throne Windows client, its Russian text, and where it shows.

Surfaces: message boxes (QMessageBox / MessageBoxWarning), the status card, the updater's status
line, the log panel (Продвинутый view), the crash box. The client shows no tray notifications
(no `QSystemTrayIcon::showMessage` call anywhere), so there is nothing to translate there.

Language: the Russian text comes from `res/translations/ru_RU.ts`, loaded when the system
language is Russian or the language setting is «Русский». The crash box is a Win32 box written
before Qt can be trusted, so its text is Russian in place.

## 0. How raw core errors are recognised

sing-box / xray / Windows / Qt network texts cannot be translated literally. The mapper looks for
these words (case-insensitive, first match wins) and shows one sentence; anything else falls back
to «Не удалось подключиться. Попробуйте другой сервер или перезапустите программу.» with the
original under it.

| Kind | Raw text it matches (examples) | Russian |
| --- | --- | --- |
| Cache file locked | `initialize cache-file: timeout`, `cache.db: ... being used by another process` | Файл кэша занят другой копией программы. Закройте вторую копию ShadowLos и попробуйте снова. |
| Rule-set download failed | `initialize rule-set[geoip-ru]: Get "...": dial tcp ...`, `rule-set ... unexpected status: 404` | Не удалось скачать списки правил маршрутизации. Проверьте интернет и попробуйте снова. |
| Admin required (TUN) | `configure tun interface: Access is denied.`, `requires elevation`, `operation not permitted` | Для режима TUN нужны права администратора. Запустите программу от имени администратора или выключите режим TUN. |
| Another VPN | `Fwpm...`, TUN + `already exists` / `file exists` / `in use by another` | Мешает другой VPN. Закройте второй VPN и попробуйте снова. |
| TUN failed | `configure tun interface`, `open tun`, `wintun` | Не удалось включить режим TUN. Закройте другие VPN, перезапустите программу и попробуйте снова. |
| Port in use | `listen tcp 127.0.0.1:2080: bind: Only one usage of each socket address`, `address already in use` | Порт 2080 уже занят. Закройте программу, которая его использует (например, вторую копию ShadowLos), или смените порт в настройках. |
| Already started | `instance already started` | Подключение уже запущено. Отключитесь и подключитесь снова. |
| No network | `network is unreachable`, `no route to host` | Нет подключения к интернету. Проверьте сеть и попробуйте снова. |
| DNS failure | `lookup x: no such host`, `Host x not found`, `server misbehaving` | Не удалось найти адрес сервера (ошибка DNS). Проверьте интернет или выберите другой сервер. |
| TLS handshake | `tls: handshake failure`, `x509: certificate ...`, `reality`, `SSL` | Не удалось установить защищённое соединение с сервером. Выберите другой сервер или обновите подписку. |
| Connection refused | `connection refused`, `actively refused it` | Сервер отклонил подключение. Выберите другой сервер или обновите подписку. |
| Timeout | `i/o timeout`, `context deadline exceeded`, `Operation canceled` | Сервер не ответил вовремя. Проверьте интернет или выберите другой сервер. |
| Connection reset | `connection reset`, `forcibly closed by the remote host`, `unexpected EOF` | Соединение оборвалось. Проверьте интернет и попробуйте снова. |
| HTTP error | `server replied: Forbidden`, `HTTP status 403` | Сервер ответил ошибкой. Попробуйте позже или обновите подписку. |
| Core not responding | `IPC call failed`, RPC without answer | Ядро программы не отвечает. Перезапустите программу. |

The self-test (`ShadowLosThroneDev.exe --journal-selftest`, line «error text mapper») checks each
kind against real wording.

Where the sentence shows: a failed connect opens «Не удалось подключиться» with the sentence and
the original in small text, and the status card keeps the sentence as its red line until the next
try. A start error that names the TUN adapter keeps upstream's own box («Не удалось включить режим
TUN», with «Сброс» to restart the core); its «Ошибка: …» now reads «sentence (original)». Strict
route (Fwpm) errors do the same in their box. Subscription errors and `[Core error]` lines in the
log read «sentence (original)».

## 1. Raw core errors: the mapper (src/ui/shadowlos/ShadowlosErrorText.cpp)

Shown as the Russian sentence with the original error in small text under it (message box), as the red line of the status card after a failed connect, and as «sentence (original)» in the log line of a failed subscription or core call.

| Original | Russian | Where it shows |
| --- | --- | --- |
| Administrator rights are needed for TUN mode. Run the app as administrator or turn TUN mode off. | Для режима TUN нужны права администратора. Запустите программу от имени администратора или выключите режим TUN. | src/ui/shadowlos/ShadowlosErrorText.cpp:86 (connect error box + status card) |
| Another VPN is in the way. Close the other VPN and try again. | Мешает другой VPN. Закройте второй VPN и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:88 (connect error box + status card) |
| Could not turn on TUN mode. Close other VPN apps, restart the app and try again. | Не удалось включить режим TUN. Закройте другие VPN, перезапустите программу и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:90 (connect error box + status card) |
| Port %1 is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings. | Порт %1 уже занят. Закройте программу, которая его использует (например, вторую копию ShadowLos), или смените порт в настройках. | src/ui/shadowlos/ShadowlosErrorText.cpp:94 (connect error box + status card) |
| A port the app needs is already in use. Close the program that uses it (for example, a second copy of ShadowLos) or change the port in Settings. | Нужный порт уже занят. Закройте программу, которая его использует (например, вторую копию ShadowLos), или смените порт в настройках. | src/ui/shadowlos/ShadowlosErrorText.cpp:95 (connect error box + status card) |
| The cache file is busy in another copy of the app. Close the second copy of ShadowLos and try again. | Файл кэша занят другой копией программы. Закройте вторую копию ShadowLos и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:98 (connect error box + status card) |
| Could not download the routing rule lists. Check the internet connection and try again. | Не удалось скачать списки правил маршрутизации. Проверьте интернет и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:100 (connect error box + status card) |
| Could not find the server address (DNS error). Check the internet connection or pick another server. | Не удалось найти адрес сервера (ошибка DNS). Проверьте интернет или выберите другой сервер. | src/ui/shadowlos/ShadowlosErrorText.cpp:102 (connect error box + status card) |
| Could not set up a secure connection to the server. Pick another server or update the subscription. | Не удалось установить защищённое соединение с сервером. Выберите другой сервер или обновите подписку. | src/ui/shadowlos/ShadowlosErrorText.cpp:104 (connect error box + status card) |
| The server did not answer in time. Check the internet connection or pick another server. | Сервер не ответил вовремя. Проверьте интернет или выберите другой сервер. | src/ui/shadowlos/ShadowlosErrorText.cpp:106 (connect error box + status card) |
| The server refused the connection. Pick another server or update the subscription. | Сервер отклонил подключение. Выберите другой сервер или обновите подписку. | src/ui/shadowlos/ShadowlosErrorText.cpp:108 (connect error box + status card) |
| The connection was cut off. Check the internet connection and try again. | Соединение оборвалось. Проверьте интернет и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:110 (connect error box + status card) |
| No internet connection. Check the network and try again. | Нет подключения к интернету. Проверьте сеть и попробуйте снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:112 (connect error box + status card) |
| The server answered with an error. Try again later or update the subscription. | Сервер ответил ошибкой. Попробуйте позже или обновите подписку. | src/ui/shadowlos/ShadowlosErrorText.cpp:114 (connect error box + status card) |
| The app's core is not responding. Restart the app. | Ядро программы не отвечает. Перезапустите программу. | src/ui/shadowlos/ShadowlosErrorText.cpp:116 (connect error box + status card) |
| A connection is already running. Disconnect and connect again. | Подключение уже запущено. Отключитесь и подключитесь снова. | src/ui/shadowlos/ShadowlosErrorText.cpp:118 (connect error box + status card) |
| Could not connect. Try another server or restart the app. | Не удалось подключиться. Попробуйте другой сервер или перезапустите программу. | src/ui/shadowlos/ShadowlosErrorText.cpp:141 (connect error box + status card) |
| Could not connect | Не удалось подключиться | src/ui/shadowlos/ShadowlosErrorText.cpp:141 (connect error box + status card); src/ui/shadowlos/ShadowlosErrorText.cpp:145 (connect error box + status card) |

## 2. Fixed poor translations (ru_RU.ts)

Existing entries that were wrong, literal or said nothing about what to do.

| Original | Russian | Where it shows |
| --- | --- | --- |
| BuildConfig return error | Не удалось собрать конфигурацию | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:287 (message box); src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:490 (message box) |
| Stop return error | Не удалось отключиться | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:525 (message box) |
| Tun device misbehaving | Не удалось включить режим TUN | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:354 (message box) |
| If you have trouble starting VPN, you can force reset Core process here… | Не удалось включить режим TUN. Нажмите «Сброс», чтобы перезапустить ядро, и подключитесь снова.<br><br>Ошибка: %1 | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:355 (message box); src/ui/setting/dialog_vpn_settings.cpp:147 (message box) |
| Please run Throne as admin | Для этого нужны права администратора. Перезапустить программу от имени администратора? | src/ui/mainWindow/mainwindow_system.cpp:249 (message box); src/ui/shadowlos/ShadowlosJournal.cpp:620 (message box) |
| Geo asset missing category | В гео-файле нет нужной категории | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:183 (message box) |
| Geo assets installed | Гео-файлы загружены | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:219 (message box) |
| IPC error | Нет связи с ядром | src/api/RPC.cpp:424 (error text (shown by the caller's box or log)); src/api/RPC.cpp:466 (error text (shown by the caller's box or log)); src/api/RPC.cpp:483 (error text (shown by the caller's box or log)) +6 more |
| Something is very wrong, the subject ent cannot be found! | Внутренняя ошибка: профиль не найден. Перезапустите программу. | src/ui/mainWindow/TestRunner.cpp:222 (log panel); src/ui/mainWindow/TestRunner.cpp:283 (log panel); src/ui/mainWindow/TestRunner.cpp:622 (log panel) |
| Try to start the config, but the core has not listened to the RPC port, so restart it... | Ядро не отвечает, перезапускаю его… | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:456 (log panel) |
| Another profile is starting... | Другой сервер ещё подключается. Подождите несколько секунд. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:443 (message box) |
| Another profile is stopping... | Другой сервер ещё отключается. Подождите несколько секунд. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:447 (message box) |
| VPN endpoint problem | Проблема с конечной точкой VPN | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:800 (error text (shown by the caller's box or log)) |
| Invalid state | Неверное состояние | src/ui/setting/dialog_manage_routes.cpp:86 (message box) |
| Requesting subscription %1 error: %2 | Не удалось обновить подписку %1: %2 | src/configs/sub/GroupUpdater.cpp:586 (log panel) |
| Requesting profile error: %1 | Не удалось скачать профиль: %1 | src/ui/mainWindow/mainwindow_setup.cpp:959 (message box) |
| Core exits too frequently, stop automatic restart this profile. | Ядро слишком часто падает, автоматический перезапуск этого профиля остановлен. | src/sys/Process.cpp:91 (log panel) |
| Core exited, cleaning up... | Ядро неожиданно завершилось, очищаю… | src/sys/Process.cpp:84 (log panel) |

## 3. Unfinished ru_RU.ts entries filled in

Entries that were empty or marked unfinished. Entries kept as they were but marked finished are not listed.

| Original | Russian | Where it shows |
| --- | --- | --- |
| The core could not capture the profile: %1 | Ядро не смогло снять профиль производительности: %1 | src/sys/CoreDiagnostics.cpp:231 (error text (shown by the caller's box or log)) |
| The core returned an empty profile. | Ядро вернуло пустой профиль производительности. | src/sys/CoreDiagnostics.cpp:233 (error text (shown by the caller's box or log)) |
| Delete "%1"? Its secret cannot be recovered. | Удалить «%1»? Секрет нельзя будет восстановить. | src/ui/setting/dialog_otp_manager.cpp:178 (message box) |
| Failed to store the OTP profile. | Не удалось сохранить OTP-профиль. Попробуйте ещё раз. | src/ui/setting/dialog_otp_manager.cpp:204 (message box) |
| This one as otpauth:// link and QR | Этот — ссылкой otpauth:// и QR-кодом | src/ui/setting/dialog_otp_manager.cpp:214 (error text (shown by the caller's box or log)) |
| All as otpauth-migration:// link and QR | Все — ссылкой otpauth-migration:// и QR-кодом | src/ui/setting/dialog_otp_manager.cpp:216 (error text (shown by the caller's box or log)) |
| All as JSON file... | Все — файлом JSON… | src/ui/setting/dialog_otp_manager.cpp:217 (error text (shown by the caller's box or log)) |
| Paste otpauth:// links, an otpauth-migration:// link, a JSON export, or just a base32 secret. | Вставьте ссылки otpauth://, ссылку otpauth-migration://, экспорт JSON или просто секрет base32. | src/ui/setting/dialog_otp_manager.cpp:231 (error text (shown by the caller's box or log)) |
| No QR code found in the clipboard image. | В картинке из буфера обмена нет QR-кода. | src/ui/setting/dialog_otp_manager.cpp:267 (message box) |
| QR images and exports (*.png *.jpg *.jpeg *.bmp *.gif *.json *.txt);;All files (*) | QR-картинки и экспорты (*.png *.jpg *.jpeg *.bmp *.gif *.json *.txt);;Все файлы (*) | src/ui/setting/dialog_otp_manager.cpp:284 (error text (shown by the caller's box or log)) |
| Nothing to import. | Нечего импортировать. | src/ui/setting/dialog_otp_manager.cpp:333 (message box) |
| Imported %1 OTP profile(s). | Импортировано OTP-профилей: %1. | src/ui/setting/dialog_otp_manager.cpp:368 (error text (shown by the caller's box or log)) |
| Skipped %1 duplicate or unusable entries. | Пропущено повторов и негодных записей: %1. | src/ui/setting/dialog_otp_manager.cpp:369 (message box) |
| None of the selected profiles could be exported. | Не удалось экспортировать ни один из выбранных профилей. | src/ui/setting/dialog_otp_manager.cpp:384 (message box) |
| Exported %1 OTP profile(s) to:<br>%2 | Экспортировано OTP-профилей: %1. Файл:<br>%2 | src/ui/setting/dialog_otp_manager.cpp:406 (message box) |
| VPN Authentication | Вход в VPN | include/ui/profile/dialog_vpn_auth.ui:14 (dialog/form text) |
| Reconnect | Переподключиться | src/api/RPC.cpp:25 (error text (shown by the caller's box or log)); src/api/RPC.cpp:133 (error text (shown by the caller's box or log)); src/api/RPC.cpp:139 (error text (shown by the caller's box or log)) +6 more |
| Submit | Отправить | src/main.cpp:58 (error text (shown by the caller's box or log)); src/main.cpp:63 (error text (shown by the caller's box or log)); src/main.cpp:411 (error text (shown by the caller's box or log)) +17 more |
| The VPN server sent a message. | Сервер VPN прислал сообщение. | src/ui/profile/dialog_vpn_auth.cpp:79 (window text) |
| Finish signing in at the address shown below… | Завершите вход по адресу ниже и закройте это окно. Подключение продолжится само, когда сервер примет вход. | src/ui/profile/dialog_vpn_auth.cpp:88 (error text (shown by the caller's box or log)) |
| Open in Browser | Открыть в браузере | src/ui/profile/dialog_vpn_auth.cpp:95 (error text (shown by the caller's box or log)) |
| This server requires single sign-on in a browser… | Этот сервер требует вход через браузер (single sign-on), а Throne его пока не поддерживает. Нажмите «Отмена» и используйте профиль с логином и паролем или укажите cookie входа в расширенных настройках профиля. | src/ui/profile/dialog_vpn_auth.cpp:102 (error text (shown by the caller's box or log)) |
| Unsupported authentication request: %1 | Неподдерживаемый запрос входа: %1 | src/ui/profile/dialog_vpn_auth.cpp:110 (error text (shown by the caller's box or log)) |
| Expires in %1 | Истекает через %1 | src/ui/profile/dialog_vpn_auth.cpp:139 (error text (shown by the caller's box or log)); src/ui/profile/dialog_vpn_auth.cpp:251 (error text (shown by the caller's box or log)) |
| Answer | Ответ | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:626 (error text (shown by the caller's box or log)); src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:729 (error text (shown by the caller's box or log)); src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:817 (error text (shown by the caller's box or log)) +37 more |
| This request expired. | Срок этого запроса истёк. Нажмите «Переподключиться». | src/ui/profile/dialog_vpn_auth.cpp:247 (error text (shown by the caller's box or log)) |
| Submitting... | Отправка… | src/ui/profile/dialog_vpn_auth.cpp:271 (error text (shown by the caller's box or log)) |
| Base64 DER of the enrolled ECDSA P-256 key; it signs the TLS client certificate. | Ключ ECDSA P-256 устройства в Base64 DER; им подписывается клиентский сертификат TLS. | include/ui/profile/edit_masque.ui:23 (dialog/form text) |
| Comma separated list of the tunnel's own addresses… | Собственные адреса туннеля через запятую, например 172.16.0.2/32,2606:4700::1/128 | include/ui/profile/edit_masque.ui:55 (dialog/form text) |
| Register a new Cloudflare WARP device… | Зарегистрировать новое устройство Cloudflare WARP для MASQUE и заполнить поля ключа, адреса и сервера. | include/ui/profile/edit_masque.ui:123 (dialog/form text) |
| Generate WARP identity | Создать данные WARP | include/ui/profile/edit_masque.ui:126 (dialog/form text) |
| Generating identity... | Создаю данные… | src/ui/profile/edit_masque.cpp:68 (window text) |
| Failed to generate WARP identity | Не удалось создать данные WARP. Проверьте интернет и попробуйте снова. | src/ui/profile/edit_masque.cpp:80 (message box) |
| Cipher | Шифр | src/configs/outbounds/shadowsocks.cpp:192 (error text (shown by the caller's box or log)); src/configs/outbounds/shadowsocks.cpp:197 (error text (shown by the caller's box or log)); src/configs/outbounds/shadowsocks.cpp:198 (error text (shown by the caller's box or log)) +10 more |
| All domains — every DNS query goes through this tunnel | Все домены — каждый DNS-запрос идёт через этот туннель | src/ui/stats/dialog_endpoint_details.cpp:262 (error text (shown by the caller's box or log)) |
| Routes through this tunnel | Маршруты через этот туннель | src/ui/stats/dialog_endpoint_details.cpp:266 (error text (shown by the caller's box or log)) |
| Kept outside this tunnel | Вне этого туннеля | src/ui/stats/dialog_endpoint_details.cpp:267 (error text (shown by the caller's box or log)) |
| DNS servers pushed by the server | DNS-серверы, выданные сервером | src/ui/stats/dialog_endpoint_details.cpp:268 (error text (shown by the caller's box or log)) |
| Domains routed to this tunnel | Домены, направляемые в этот туннель | src/ui/stats/dialog_endpoint_details.cpp:269 (error text (shown by the caller's box or log)) |
| Listening on all interfaces (%1) | Доступен со всех сетевых интерфейсов (%1) | src/ui/mainWindow/mainwindow_view.cpp:182 (error text (shown by the caller's box or log)) |
| [VPN] %1: the core did not answer the sign-in prompt. | [VPN] %1: ядро не ответило на запрос входа. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:900 (log panel) |
| [VPN] %1: could not answer the sign-in prompt: %2 | [VPN] %1: не удалось ответить на запрос входа: %2 | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:902 (log panel) |
| [VPN] %1: signed in again with a new one-time code. | [VPN] %1: повторный вход с новым одноразовым кодом выполнен. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:904 (log panel) |
| [VPN] %1 rejected the saved credentials; restarting the profile with a new one-time code. | [VPN] %1 отклонил сохранённые данные входа; перезапускаю профиль с новым одноразовым кодом. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:929 (log panel) |
| Could not update the autostart entry: | Не удалось изменить автозапуск: | src/ui/mainWindow/mainwindow_setup.cpp:802 (message box) |
| This installation cannot grant the core privileges by itself. | Эта установка не может сама выдать ядру нужные права. | src/ui/mainWindow/mainwindow_system.cpp:220 (message box) |
| The selected TLS version is not supported on this system. | Выбранная версия TLS не поддерживается в этой системе. Выберите другую в расширенных настройках группы. | src/global/HTTPRequestHelper.cpp:28 (error text (shown by the caller's box or log)) |
| Response larger than %1 MB | Ответ больше %1 МБ | src/global/HTTPRequestHelper.cpp:89 (error text (shown by the caller's box or log)) |
| Failed to parse the Clash configuration. | Не удалось прочитать конфигурацию Clash. | src/configs/sub/SubscriptionParser.cpp:460 (message box) |
| Failed to import the OpenVPN profile. | Не удалось импортировать профиль OpenVPN. | src/configs/sub/SubscriptionParser.cpp:476 (log panel) |
| Failed to import the OpenConnect profile. | Не удалось импортировать профиль OpenConnect. | src/configs/sub/SubscriptionParser.cpp:490 (log panel); src/configs/sub/SubscriptionParser.cpp:505 (log panel) |
| Failed to decode the vpn:// link. | Не удалось прочитать ссылку vpn://. | src/configs/sub/SubscriptionParser.cpp:601 (log panel) |
| No importable profile found in the vpn:// link. | В ссылке vpn:// нет профиля для импорта. | src/configs/sub/SubscriptionParser.cpp:635 (log panel) |
| Removed %1 duplicate profiles: | Удалены повторяющиеся профили (%1): | src/configs/sub/GroupUpdater.cpp:311 (error text (shown by the caller's box or log)) |
| Removed %1 insecure profiles: | Удалены небезопасные профили (%1): | src/configs/sub/GroupUpdater.cpp:315 (error text (shown by the caller's box or log)) |
| Skipped removing invalid profiles: the core is unreachable. | Недействительные профили не удалены: ядро не отвечает. | src/configs/sub/GroupUpdater.cpp:327 (log panel) |
| Removed %1 invalid profiles: | Удалены недействительные профили (%1): | src/configs/sub/GroupUpdater.cpp:329 (log panel) |
| The running profile was kept. | Запущенный профиль оставлен. | src/configs/sub/GroupUpdater.cpp:340 (message box); src/configs/sub/GroupUpdater.cpp:361 (message box) |
| Removed %1 unavailable profiles: | Удалены недоступные профили (%1): | src/configs/sub/GroupUpdater.cpp:360 (message box) |
| Skipped sorting %1: another sort is in progress. | Сортировка %1 пропущена: уже идёт другая сортировка. | src/configs/sub/GroupUpdater.cpp:370 (log panel) |
| Subscription update already queued: %1 | Обновление подписки уже в очереди: %1 | src/configs/sub/GroupUpdater.cpp:432 (log panel) |
| Subscription server update interval: %1 hour(s) | Сервер подписки просит обновлять её раз в %1 ч | src/configs/sub/GroupUpdater.cpp:677 (log panel) |
| The URL of %1 changed during the update, so the result was discarded. | Ссылка %1 изменилась во время обновления, результат отброшен. Обновите подписку ещё раз. | src/configs/sub/GroupUpdater.cpp:711 (log panel) |
| The server returned a web page instead of a subscription.… | Сервер вернул веб-страницу вместо подписки. Если это капча или проверка на бота, попробуйте другую версию TLS, версию HTTP или User Agent в расширенных настройках группы. | src/configs/sub/GroupUpdater.cpp:720 (log panel) |
| Announcement from %1: %2 | Объявление от %1: %2 | src/configs/sub/GroupUpdater.cpp:724 (log panel) |
| No profiles found in the subscription: %1 was left unchanged. | В подписке нет серверов: группа %1 оставлена без изменений. | src/configs/sub/GroupUpdater.cpp:729 (log panel) |
| Still in use, so kept instead of deleted:<br>%1 | Используются, поэтому не удалены:<br>%1 | src/configs/sub/GroupUpdater.cpp:870 (error text (shown by the caller's box or log)) |
| Working, so kept instead of deleted:<br>%1 | Работают, поэтому не удалены:<br>%1 | src/configs/sub/GroupUpdater.cpp:873 (error text (shown by the caller's box or log)) |
| After the URL test of %1: | После проверки задержки %1: | src/configs/sub/GroupUpdater.cpp:921 (log panel) |
| IPv4 forwarding breaks Tun mode | Раздача интернета мешает режиму TUN | src/sys/Process.cpp:23 (warning box (passive)) |
| IPv4 forwarding is on for the network adapter… | Для сетевого адаптера «%1» включена пересылка IPv4 — обычно из-за мобильного хот-спота или общего доступа к интернету.<br><br>Тогда Windows не даёт собственным соединениям Throne обойти TUN, они зацикливаются и обрываются.<br><br>Чтобы исправить, раздавайте хот-спот с адаптера throne-tun вместо «%1» (Параметры > Мобильный хот-спот > Совместное использование интернет-соединения) или выключите хот-спот, пока работает режим TUN. | src/sys/Process.cpp:24 (warning box (passive)) |
| Auto-update: every %1 | Автообновление: каждые %1 | src/ui/group/GroupItem.cpp:27 (error text (shown by the caller's box or log)); src/ui/mainWindow/mainwindow_groups.cpp:54 (error text (shown by the caller's box or log)); src/ui/widget/SubscriptionInfoCard.cpp:521 (error text (shown by the caller's box or log)) |
| The routing profile lists an endpoint profile (id %1) that no longer exists | В профиле маршрутизации указана конечная точка (id %1), которой больше нет. Уберите её из профиля | src/configs/generate.cpp:636 (error text (shown by the caller's box or log)) |
| %1 is listed as a routing profile endpoint but is empty or corrupted | %1 указан как конечная точка профиля маршрутизации, но он пуст или повреждён | src/configs/generate.cpp:643 (error text (shown by the caller's box or log)); src/configs/generate.cpp:648 (error text (shown by the caller's box or log)); src/configs/generate.cpp:658 (error text (shown by the caller's box or log)) |
| %1 is listed as a routing profile endpoint but a hop of it no longer exists | %1 указан как конечная точка профиля маршрутизации, но одного из его звеньев больше нет | src/configs/generate.cpp:643 (error text (shown by the caller's box or log)); src/configs/generate.cpp:648 (error text (shown by the caller's box or log)); src/configs/generate.cpp:658 (error text (shown by the caller's box or log)) |
| %1 is listed as a routing profile endpoint, so its last hop must be an OpenVPN or OpenConnect profile | %1 указан как конечная точка профиля маршрутизации, поэтому его последнее звено должно быть профилем OpenVPN или OpenConnect | src/configs/generate.cpp:652 (error text (shown by the caller's box or log)) |
| Hops of the routing profile endpoint %1 cannot use an extra core, a full config, or be a chain | Звенья конечной точки %1 не могут использовать дополнительное ядро, полную конфигурацию или быть цепочкой | src/configs/generate.cpp:663 (error text (shown by the caller's box or log)); src/configs/generate.cpp:668 (error text (shown by the caller's box or log)) |
| Hops of the routing profile endpoint %1 cannot run on the Xray core | Звенья конечной точки %1 не могут работать на ядре Xray | src/configs/generate.cpp:663 (error text (shown by the caller's box or log)); src/configs/generate.cpp:668 (error text (shown by the caller's box or log)) |
| %1 is used as an endpoint of the routing profile and by the started profile at the same time, remove it from one of them | %1 используется и как конечная точка профиля маршрутизации, и запускаемым профилем. Уберите его из одного из них | src/configs/generate.cpp:672 (error text (shown by the caller's box or log)) |
| %1 is listed twice in the endpoints of the routing profile | %1 указан в конечных точках профиля маршрутизации дважды | src/configs/generate.cpp:677 (error text (shown by the caller's box or log)) |
| Static Key | Статический ключ | src/configs/outbounds/openvpn.cpp:433 (error text (shown by the caller's box or log)); src/ui/profile/edit_openvpn_advanced.cpp:126 (window text); include/ui/profile/edit_openvpn_advanced.ui:235 (dialog/form text) +2 more |
| Unverified TLS | TLS без проверки | src/configs/outbounds/openvpn.cpp:435 (error text (shown by the caller's box or log)) |
| OpenVPN or OpenConnect profile | Профиль OpenVPN или OpenConnect | src/configs/AutoSelectorPlan.cpp:235 (error text (shown by the caller's box or log)); src/configs/generate.cpp:652 (error text (shown by the caller's box or log)) |
| Xray full config cannot be combined with the group's proxies | Полную конфигурацию Xray нельзя сочетать с прокси группы | src/configs/AutoSelectorPlan.cpp:240 (error text (shown by the caller's box or log)) |
| Waiting for sign-in | Ожидание входа | src/ui/stats/dialog_endpoint_details.cpp:93 (error text (shown by the caller's box or log)) |
| Too much data to fit in a QR code. | Слишком много данных для одного QR-кода. | src/ui/setting/dialog_otp_manager.cpp:67 (window text) |
| Copy | Копировать | src/main.cpp:171 (error text (shown by the caller's box or log)); src/main.cpp:177 (error text (shown by the caller's box or log)); src/main.cpp:197 (error text (shown by the caller's box or log)) +41 more |
| null | null | src/main.cpp:112 (error text (shown by the caller's box or log)); src/main.cpp:113 (error text (shown by the caller's box or log)); src/main.cpp:118 (error text (shown by the caller's box or log)) +940 more |
| boolean | логическое значение | src/configs/sub/clash.cpp:6 (error text (shown by the caller's box or log)); src/configs/sub/clash.cpp:54 (error text (shown by the caller's box or log)); src/configs/sub/clash.cpp:61 (error text (shown by the caller's box or log)) +2 more |
| number | число | src/configs/generate.cpp:293 (error text (shown by the caller's box or log)); src/configs/generate.cpp:2486 (log panel); src/configs/common/multiplex.cpp:31 (error text (shown by the caller's box or log)) +351 more |
| string | строка | src/api/RPC.cpp:158 (error text (shown by the caller's box or log)); src/api/RPC.cpp:266 (error text (shown by the caller's box or log)); src/api/RPC.cpp:281 (error text (shown by the caller's box or log)) +2445 more |
| array | массив | src/configs/outbounds/hysteria.cpp:175 (error text (shown by the caller's box or log)); src/configs/outbounds/masque.cpp:22 (error text (shown by the caller's box or log)); src/configs/outbounds/openconnect.cpp:14 (error text (shown by the caller's box or log)) +75 more |
| object | объект | src/configs/generate.cpp:880 (error text (shown by the caller's box or log)); src/configs/generate.cpp:881 (error text (shown by the caller's box or log)); src/configs/generate.cpp:1369 (error text (shown by the caller's box or log)) +2408 more |
| Empty document | Пустой документ | src/ui/widget/json/JsonTree.cpp:51 (error text (shown by the caller's box or log)) |
| Unexpected content after the end of the document | Лишний текст после конца документа | src/ui/widget/json/JsonTree.cpp:62 (error text (shown by the caller's box or log)) |
| The document is nested too deeply | Слишком глубокая вложенность документа | src/ui/widget/json/JsonTree.cpp:126 (error text (shown by the caller's box or log)) |
| Unexpected end of document | Документ неожиданно закончился | src/ui/widget/json/JsonTree.cpp:128 (error text (shown by the caller's box or log)) |
| Expected a property name in quotes | Ожидалось имя свойства в кавычках | src/ui/widget/json/JsonTree.cpp:153 (error text (shown by the caller's box or log)) |
| Expected a colon after the property name | Ожидалось двоеточие после имени свойства | src/ui/widget/json/JsonTree.cpp:156 (error text (shown by the caller's box or log)) |
| Trailing comma | Лишняя запятая в конце | src/ui/widget/json/JsonTree.cpp:166 (error text (shown by the caller's box or log)); src/ui/widget/json/JsonTree.cpp:196 (error text (shown by the caller's box or log)) |
| Expected a comma or a closing brace | Ожидалась запятая или закрывающая фигурная скобка | src/ui/widget/json/JsonTree.cpp:174 (error text (shown by the caller's box or log)) |
| Expected a comma or a closing bracket | Ожидалась запятая или закрывающая квадратная скобка | src/ui/widget/json/JsonTree.cpp:204 (error text (shown by the caller's box or log)) |
| Unterminated string | Строка не закрыта кавычкой | src/ui/widget/json/JsonTree.cpp:213 (error text (shown by the caller's box or log)) |
| Unterminated escape sequence | Незавершённая escape-последовательность | src/ui/widget/json/JsonTree.cpp:223 (error text (shown by the caller's box or log)) |
| Incomplete unicode escape sequence | Неполная escape-последовательность Unicode | src/ui/widget/json/JsonTree.cpp:237 (error text (shown by the caller's box or log)) |
| Invalid unicode escape sequence | Неверная escape-последовательность Unicode | src/ui/widget/json/JsonTree.cpp:240 (error text (shown by the caller's box or log)) |
| Invalid escape sequence | Неверная escape-последовательность | src/ui/widget/json/JsonTree.cpp:246 (error text (shown by the caller's box or log)) |
| Control character in string | Управляющий символ в строке | src/ui/widget/json/JsonTree.cpp:250 (error text (shown by the caller's box or log)) |
| Invalid value | Неверное значение | src/ui/widget/json/JsonTree.cpp:258 (error text (shown by the caller's box or log)); src/ui/widget/json/JsonTree.cpp:288 (error text (shown by the caller's box or log)) |
| Invalid number | Неверное число | src/ui/widget/json/JsonTree.cpp:291 (error text (shown by the caller's box or log)) |
| The schema is empty | Схема пуста | src/ui/widget/json/JsonSchemaValidator.cpp:138 (error text (shown by the caller's box or log)) |
| The schema has no definition at %1 | В схеме нет определения %1 | src/ui/widget/json/JsonSchemaValidator.cpp:143 (error text (shown by the caller's box or log)) |
| Expected %1, got %2 | Ожидалось: %1, получено: %2 | src/ui/widget/json/JsonSchemaValidator.cpp:211 (error text (shown by the caller's box or log)); src/ui/widget/json/JsonSchemaValidator.cpp:546 (error text (shown by the caller's box or log)) |
|  or  |  или  | src/api/RPC.cpp:180 (error text (shown by the caller's box or log)); src/configs/AutoSelectorPlan.cpp:229 (error text (shown by the caller's box or log)); src/configs/AutoSelectorPlan.cpp:235 (error text (shown by the caller's box or log)) +227 more |
| Expected %1 here | Здесь ожидалось: %1 | src/ui/widget/json/JsonSchemaValidator.cpp:222 (error text (shown by the caller's box or log)) |
| %1 is not valid here (expected: %2) | %1 здесь недопустимо (ожидалось: %2) | src/ui/widget/json/JsonSchemaValidator.cpp:239 (error text (shown by the caller's box or log)) |
| This option is deprecated | Этот параметр устарел | src/ui/widget/json/JsonSchemaValidator.cpp:248 (error text (shown by the caller's box or log)) |
| "%1" is not in the expected format | «%1» не в ожидаемом формате | src/ui/widget/json/JsonSchemaValidator.cpp:255 (error text (shown by the caller's box or log)) |
| Value must be at least %1 | Значение должно быть не меньше %1 | src/ui/widget/json/JsonSchemaValidator.cpp:265 (error text (shown by the caller's box or log)) |
| Value must be at most %1 | Значение должно быть не больше %1 | src/ui/widget/json/JsonSchemaValidator.cpp:273 (error text (shown by the caller's box or log)) |
| Unknown field "%1" | Неизвестное поле «%1» | src/ui/widget/json/JsonSchemaValidator.cpp:305 (error text (shown by the caller's box or log)); src/ui/widget/json/JsonSchemaValidator.cpp:349 (error text (shown by the caller's box or log)) |
| Missing required field "%1" | Нет обязательного поля «%1» | src/ui/widget/json/JsonSchemaValidator.cpp:357 (error text (shown by the caller's box or log)) |
| Unknown %1 %2 (expected: %3) | Неизвестное значение %1 %2 (ожидалось: %3) | src/ui/widget/json/JsonSchemaValidator.cpp:488 (error text (shown by the caller's box or log)) |
| Value does not match any accepted form | Значение не подходит ни под один допустимый вид | src/ui/widget/json/JsonSchemaValidator.cpp:543 (error text (shown by the caller's box or log)) |
| Empty OpenVPN configuration. | Пустая конфигурация OpenVPN. | src/configs/sub/vpnFileImport.cpp:227 (error text (shown by the caller's box or log)) |
| <connection> block without a remote, skipped. | Блок <connection> без remote пропущен. | src/configs/sub/vpnFileImport.cpp:277 (error text (shown by the caller's box or log)) |
| Ignored an unreadable peer fingerprint: %1 | Пропущен нечитаемый отпечаток узла: %1 | src/configs/sub/vpnFileImport.cpp:300 (error text (shown by the caller's box or log)); src/configs/sub/vpnFileImport.cpp:429 (error text (shown by the caller's box or log)) |
| PKCS#12 bundles are not supported; export the CA, certificate and key as PEM. | Пакеты PKCS#12 не поддерживаются. Экспортируйте CA, сертификат и ключ в формате PEM. | src/configs/sub/vpnFileImport.cpp:303 (error text (shown by the caller's box or log)); src/configs/sub/vpnFileImport.cpp:319 (error text (shown by the caller's box or log)) |
| Ignored inline block: <%1> | Пропущен встроенный блок: <%1> | src/configs/sub/vpnFileImport.cpp:305 (error text (shown by the caller's box or log)) |
| This is an OpenVPN server configuration (%1), not a client profile. | Это конфигурация сервера OpenVPN (%1), а не профиль клиента. | src/configs/sub/vpnFileImport.cpp:315 (error text (shown by the caller's box or log)) |
| TAP (layer 2) tunnels are not supported; only `dev tun` profiles can be imported. | Туннели TAP (уровень 2) не поддерживаются. Импортировать можно только профили с `dev tun`. | src/configs/sub/vpnFileImport.cpp:324 (error text (shown by the caller's box or log)) |
| This is an OpenVPN server configuration (mode server), not a client profile. | Это конфигурация сервера OpenVPN (mode server), а не профиль клиента. | src/configs/sub/vpnFileImport.cpp:330 (error text (shown by the caller's box or log)) |
| `proto %1` is a server transport. | `proto %1` — транспорт для сервера. | src/configs/sub/vpnFileImport.cpp:341 (error text (shown by the caller's box or log)) |
| Unknown transport: proto %1 | Неизвестный транспорт: proto %1 | src/configs/sub/vpnFileImport.cpp:342 (error text (shown by the caller's box or log)) |
| Ignored an unreadable IPv6 interface address: %1 | Пропущен нечитаемый IPv6-адрес интерфейса: %1 | src/configs/sub/vpnFileImport.cpp:361 (error text (shown by the caller's box or log)) |
| Credentials live in %1; enter them in the profile editor. | Данные входа лежат в %1. Введите их в редакторе профиля. | src/configs/sub/vpnFileImport.cpp:375 (error text (shown by the caller's box or log)) |
| A hash-directory CRL is not supported; point `crl-verify` at a PEM or DER file. | CRL в виде каталога не поддерживается. Укажите в `crl-verify` файл PEM или DER. | src/configs/sub/vpnFileImport.cpp:414 (error text (shown by the caller's box or log)) |
| Ignored a symbolic route target: route %1 | Пропущен символьный адрес маршрута: route %1 | src/configs/sub/vpnFileImport.cpp:535 (error text (shown by the caller's box or log)) |
| Ignored an unreadable route: %1 | Пропущен нечитаемый маршрут: %1 | src/configs/sub/vpnFileImport.cpp:549 (error text (shown by the caller's box or log)); src/configs/sub/vpnFileImport.cpp:563 (error text (shown by the caller's box or log)) |
| redirect-gateway flag has no sing-box equivalent: %1 | У флага redirect-gateway нет аналога в sing-box: %1 | src/configs/sub/vpnFileImport.cpp:581 (error text (shown by the caller's box or log)) |
| Not supported by the OpenVPN endpoint, ignored: %1 | Не поддерживается конечной точкой OpenVPN, пропущено: %1 | src/configs/sub/vpnFileImport.cpp:639 (error text (shown by the caller's box or log)) |
| Unknown OpenVPN directive, ignored: %1 | Неизвестная директива OpenVPN, пропущена: %1 | src/configs/sub/vpnFileImport.cpp:642 (error text (shown by the caller's box or log)) |
| `%1` turns the client certificate off; it was dropped and the server has to accept password login. | `%1` отключает клиентский сертификат: он убран, и сервер должен принимать вход по паролю. | src/configs/sub/vpnFileImport.cpp:660 (error text (shown by the caller's box or log)) |
| `cert` without a `key`: add the private key, or `client-cert-not-required` for password-only login. | `cert` без `key`: добавьте закрытый ключ или `client-cert-not-required` для входа только по паролю. | src/configs/sub/vpnFileImport.cpp:668 (error text (shown by the caller's box or log)) |
| `key` without a `cert`: add the client certificate. | `key` без `cert`: добавьте клиентский сертификат. | src/configs/sub/vpnFileImport.cpp:670 (error text (shown by the caller's box or log)) |
| No `remote` server in the OpenVPN configuration. | В конфигурации OpenVPN нет сервера `remote`. | src/configs/sub/vpnFileImport.cpp:679 (error text (shown by the caller's box or log)) |
| Ignored an unreadable interface address: %1 | Пропущен нечитаемый адрес интерфейса: %1 | src/configs/sub/vpnFileImport.cpp:707 (error text (shown by the caller's box or log)) |
| `remote-cert-eku` replaces `remote-cert-tls`; the latter was dropped. | `remote-cert-eku` заменяет `remote-cert-tls`, поэтому второй убран. | src/configs/sub/vpnFileImport.cpp:722 (error text (shown by the caller's box or log)) |
| Unsupported OpenConnect protocol, ignored: %1 | Неподдерживаемый протокол OpenConnect, пропущен: %1 | src/configs/sub/vpnFileImport.cpp:808 (error text (shown by the caller's box or log)) |
| Unknown compression mode, ignored: %1 | Неизвестный режим сжатия, пропущен: %1 | src/configs/sub/vpnFileImport.cpp:886 (error text (shown by the caller's box or log)) |
| Unknown reported OS, ignored: %1 | Неизвестная ОС в настройке, пропущена: %1 | src/configs/sub/vpnFileImport.cpp:960 (error text (shown by the caller's box or log)) |
| Unsupported token mode, ignored: %1 | Неподдерживаемый режим токена, пропущен: %1 | src/configs/sub/vpnFileImport.cpp:972 (error text (shown by the caller's box or log)) |
| Expected --form-entry=FORM:OPTION=VALUE, ignored: %1 | Ожидалось --form-entry=FORM:OPTION=VALUE, пропущено: %1 | src/configs/sub/vpnFileImport.cpp:984 (error text (shown by the caller's box or log)) |
| Configure a proxy through Throne's chain instead, ignored: %1 | Прокси настраивается цепочкой Throne, пропущено: %1 | src/configs/sub/vpnFileImport.cpp:995 (error text (shown by the caller's box or log)) |
| Unknown OpenConnect option, ignored: %1 | Неизвестный параметр OpenConnect, пропущен: %1 | src/configs/sub/vpnFileImport.cpp:999 (error text (shown by the caller's box or log)) |
| Unknown OpenConnect option, ignored: -%1 | Неизвестный параметр OpenConnect, пропущен: -%1 | src/configs/sub/vpnFileImport.cpp:1033 (error text (shown by the caller's box or log)) |
| An AnyConnect profile carrying a DTD is not accepted. | Профиль AnyConnect с DTD не принимается. | src/configs/sub/vpnFileImport.cpp:1079 (error text (shown by the caller's box or log)) |
| Malformed AnyConnect profile: %1 | Повреждённый профиль AnyConnect: %1 | src/configs/sub/vpnFileImport.cpp:1107 (error text (shown by the caller's box or log)) |
| The AnyConnect profile lists no host entry. | В профиле AnyConnect нет ни одного хоста. | src/configs/sub/vpnFileImport.cpp:1111 (error text (shown by the caller's box or log)) |
| Skipped a host entry without an address. | Пропущен хост без адреса. | src/configs/sub/vpnFileImport.cpp:1121 (error text (shown by the caller's box or log)) |
| Skipped "%1": IKEv2/IPsec is not spoken by the OpenConnect endpoint. | Пропущен «%1»: конечная точка OpenConnect не поддерживает IKEv2/IPsec. | src/configs/sub/vpnFileImport.cpp:1125 (error text (shown by the caller's box or log)) |
| Skipped an unreadable host address: %1 | Пропущен нечитаемый адрес хоста: %1 | src/configs/sub/vpnFileImport.cpp:1131 (error text (shown by the caller's box or log)) |
| Empty OpenConnect profile. | Пустой профиль OpenConnect. | src/configs/sub/vpnFileImport.cpp:1165 (error text (shown by the caller's box or log)) |
| The AnyConnect profile has no host entry this endpoint can use. | В профиле AnyConnect нет хоста, который может использовать эта конечная точка. | src/configs/sub/vpnFileImport.cpp:1181 (error text (shown by the caller's box or log)) |
| No OpenConnect options found. | Параметры OpenConnect не найдены. | src/configs/sub/vpnFileImport.cpp:1213 (error text (shown by the caller's box or log)) |
| No OpenConnect server address found. | Адрес сервера OpenConnect не найден. | src/configs/sub/vpnFileImport.cpp:1220 (error text (shown by the caller's box or log)) |
| Digits must be between %1 and %2 | Число цифр должно быть от %1 до %2 | src/global/OTP.cpp:237 (error text (shown by the caller's box or log)) |
| Period must be between %1 and %2 seconds | Период должен быть от %1 до %2 секунд | src/global/OTP.cpp:239 (error text (shown by the caller's box or log)) |
| Not a readable OTP export | Это не читаемый экспорт OTP | src/global/OTP.cpp:459 (error text (shown by the caller's box or log)) |
| Unreadable migration link | Не удалось прочитать ссылку переноса | src/global/OTP.cpp:471 (error text (shown by the caller's box or log)) |
| Not an otpauth link or secret: %1 | Это не ссылка otpauth и не секрет: %1 | src/global/OTP.cpp:489 (error text (shown by the caller's box or log)) |
| Database error | Ошибка базы данных | src/database/Database.cpp:84 (warning box (passive)) |
| %1 failed: %2. Details are in the log file. | %1: ошибка: %2. Подробности в файле журнала. | src/database/Database.cpp:85 (warning box (passive)) |
| Traffic statistics paused | Статистика трафика приостановлена | src/database/TrafficStatsRepo.cpp:312 (warning box (passive)) |
| The statistics database is unusable (%1).… | База статистики повреждена (%1). Статистика приостановлена до конца сеанса, а файл будет пересоздан при следующем запуске Throne. | src/database/TrafficStatsRepo.cpp:314 (warning box (passive)) |
| Writing statistics keeps failing (%1). Statistics are paused until Throne restarts. | Статистику не удаётся записать (%1). Она приостановлена до перезапуска Throne. | src/database/TrafficStatsRepo.cpp:315 (warning box (passive)) |
| %1: the private key is empty | %1: закрытый ключ не задан | src/configs/outbounds/masque.cpp:97 (error text (shown by the caller's box or log)) |
| %1: no address is set | %1: адрес не задан | src/configs/outbounds/masque.cpp:98 (error text (shown by the caller's box or log)) |
| Failed to reach the core. | Ядро не отвечает. Перезапустите программу. | src/configs/sub/warp.cpp:36 (error text (shown by the caller's box or log)) |
| Cloudflare WARP | Cloudflare WARP | src/configs/sub/warp.cpp:60 (message box); src/configs/sub/warp.cpp:62 (message box); include/ui/profile/edit_masque.ui:123 (dialog/form text) +1 more |
| Generating a WARP identity registers a new device with Cloudflare.… | Чтобы создать данные WARP, в Cloudflare регистрируется новое устройство.<br><br>Вы принимаете <a href="%1">условия использования Cloudflare WARP</a>? | src/configs/sub/warp.cpp:61 (message box) |
| OpenVPN / OpenConnect profiles started alongside this routing profile.… | Профили OpenVPN / OpenConnect, которые запускаются вместе с этим профилем маршрутизации. Трафик к сетям, которые они объявляют, идёт через них, остальное — по правилам этого профиля. Для каждой конечной точки на вкладке «Дополнительно» появляется правило, которое можно двигать среди своих правил. Цепочка может выдать по правилу на каждое внутреннее звено OpenVPN / OpenConnect, чтобы трафик можно было направить на звено до выхода. | include/ui/setting/RouteItem.ui:233 (dialog/form text) |
| Allow routing to inner hops | Разрешить маршруты к внутренним звеньям | src/ui/setting/RouteItem.cpp:464 (window text) |
| Give each OpenVPN/OpenConnect hop behind the exit its own rule… | Дать каждому звену OpenVPN/OpenConnect за выходом своё правило, чтобы трафик можно было направить и на него. | src/ui/setting/RouteItem.cpp:466 (window text) |
| Endpoint rule: move it to choose where the endpoint claims traffic. Managed by the Endpoints tab. | Правило конечной точки: перемещайте его, чтобы выбрать, где конечная точка забирает трафик. Управляется на вкладке «Конечные точки». | src/ui/setting/RouteItem.cpp:663 (window text) |
| This rule installs a 'preferred by' rule… | Это правило добавляет правило предпочтения, чтобы сети, объявленные конечной точкой %1, шли в её туннель. | src/ui/setting/RouteItem.cpp:955 (error text (shown by the caller's box or log)) |
| This rule belongs to "%1", an inner hop of the endpoint "%2".… | Это правило принадлежит «%1» — внутреннему звену конечной точки «%2».<br><br>Перестать направлять трафик на внутренние звенья этой конечной точки? | src/ui/setting/RouteItem.cpp:998 (message box) |

## 4. Hardcoded English, now tr() + ru_RU.ts



| Original | Russian | Where it shows |
| --- | --- | --- |
| Failed to get admin access, cannot listen on port %1 without it | Нет прав администратора: без них нельзя слушать порт %1. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:235 (log panel) |
| Failed to stop profile tests! | Не удалось остановить проверки серверов. | src/ui/mainWindow/mainwindow_profile_lifecycle.cpp:518 (log panel) |
| Build config error | Не удалось собрать конфигурацию | src/ui/mainWindow/mainwindow_profiles.cpp:201 (message box); src/ui/mainWindow/mainwindow_setup.cpp:1066 (message box) |
| Build Test config error | Не удалось собрать тестовую конфигурацию | src/ui/mainWindow/mainwindow_profiles.cpp:209 (message box); src/ui/mainWindow/mainwindow_setup.cpp:1084 (message box) |
| Action already in progress | Действие уже выполняется | src/ui/mainWindow/mainwindow_setup.cpp:469 (message box); src/ui/mainWindow/mainwindow_setup.cpp:526 (message box); src/ui/mainWindow/mainwindow_setup.cpp:601 (message box) +1 more |
| A sort action is already in progress | Сортировка уже идёт. Подождите, пока она закончится. | src/ui/mainWindow/mainwindow_setup.cpp:469 (message box); src/ui/mainWindow/mainwindow_setup.cpp:526 (message box); src/ui/mainWindow/mainwindow_setup.cpp:601 (message box) +1 more |
| No such group: %1 | Группа не найдена: %1 | src/ui/mainWindow/mainwindow_groups.cpp:86 (message box) |
| Invalid Operation | Недопустимое действие | src/ui/mainWindow/mainwindow_system.cpp:291 (message box) |
| Cannot set system proxy when mixed inbound is disabled. | Нельзя включить системный прокси, когда выключен смешанный (mixed) вход. Включите его в настройках. | src/ui/mainWindow/mainwindow_system.cpp:291 (message box) |
| Could not find current group! | Текущая группа не найдена. | src/ui/mainWindow/mainwindow_view.cpp:354 (log panel) |
| URL test error: %1 | Ошибка проверки задержки: %1 | src/ui/mainWindow/mainwindow_view.cpp:436 (log panel) |
| IPC connection from an unexpected process was rejected | Отклонено подключение к IPC от постороннего процесса | src/ui/mainWindow/mainwindow_setup.cpp:228 (log panel) |
| Wrong type | Неверный тип профиля | src/ui/profile/dialog_edit_profile.cpp:225 (message box) |
| A profile with this id already exists. | Профиль с таким id уже существует. | src/ui/profile/dialog_edit_profile.cpp:330 (message box) |
| Select custom image error | Не удалось выбрать картинку | src/ui/setting/dialog_basic_settings.cpp:215 (message box) |
| Internal error | Внутренняя ошибка | src/configs/sub/GroupUpdater.cpp:337 (message box); src/configs/sub/GroupUpdater.cpp:357 (message box); src/configs/sub/GroupUpdater.cpp:770 (message box) +1 more |
| DB Error when deleting profiles, data may be corrupted | Ошибка базы данных при удалении профилей, данные могут быть повреждены. Перезапустите программу. | src/configs/sub/GroupUpdater.cpp:337 (message box); src/configs/sub/GroupUpdater.cpp:357 (message box); src/configs/sub/GroupUpdater.cpp:827 (message box) |
| DB Error when deleting profiles, Please try again. | Ошибка базы данных при удалении профилей. Попробуйте ещё раз. | src/configs/sub/GroupUpdater.cpp:770 (message box) |
| The last subscription update has not exited. | Предыдущее обновление подписки ещё не закончилось. | src/configs/sub/GroupUpdater.cpp:448 (log panel) |
| Subscription task failed: %1 | Не удалось обновить подписку: %1 | src/configs/sub/GroupUpdater.cpp:566 (log panel) |
| Subscription task failed. | Не удалось обновить подписку. | src/configs/sub/GroupUpdater.cpp:568 (log panel) |
| Extra Core exited, stopping profile... | Дополнительное ядро завершилось, отключаю профиль… | src/sys/Process.cpp:48 (log panel) |
| Could not start the core: %1 | Не удалось запустить ядро: %1 | src/sys/Process.cpp:62 (log panel) |
| No connection to the core (code %1) | Нет связи с ядром (код %1) | src/api/RPC.cpp:261 (log panel) |
| Failed to query connections: no connection to the core | Не удалось получить список соединений: нет связи с ядром | src/api/RPC.cpp:511 (log panel) |
| Config build error: %1 | Ошибка сборки конфигурации: %1 | src/configs/generate.cpp:2387 (log panel) |
| Could not reach the core: %1 | Нет связи с ядром: %1 | src/configs/generate.cpp:2454 (log panel) |
| Invalid auto selector: %1 | Неверный автовыбор: %1 | src/configs/generate.cpp:2463 (log panel) |
| Custom Xray full config is not valid JSON | Полная конфигурация Xray — некорректный JSON | src/configs/generate.cpp:1472 (error text (shown by the caller's box or log)); src/configs/generate.cpp:2511 (log panel) |
| Invalid Xray server %1: %2 | Неверный сервер Xray %1: %2 | src/configs/generate.cpp:2522 (log panel); src/configs/generate.cpp:2532 (log panel); src/configs/generate.cpp:2542 (log panel) |
| Invalid server %1: %2 | Неверный сервер %1: %2 | src/configs/generate.cpp:2559 (log panel) |
| Skipping invalid custom Xray full config: %1 | Пропущена неверная полная конфигурация Xray: %1 | src/configs/generate.cpp:2609 (log panel) |
| Custom Xray full config produced no Xray config: %1 | Полная конфигурация Xray не дала настроек Xray: %1 | src/configs/generate.cpp:2623 (log panel) |
| Skipping invalid config: %1 | Пропущен неверный сервер: %1 | src/configs/generate.cpp:2635 (log panel) |
| TLS errors: %1 | Ошибки TLS: %1 | src/global/HTTPRequestHelper.cpp:65 (log panel); src/global/HTTPRequestHelper.cpp:138 (log panel) |
| TLS errors (ignored): %1 | Ошибки TLS (пропущены): %1 | src/global/HTTPRequestHelper.cpp:65 (log panel); src/global/HTTPRequestHelper.cpp:138 (log panel) |
| Skipped invalid headers: %1 | Пропущены неверные заголовки: %1 | src/configs/common/transport.cpp:32 (log panel); src/configs/common/xrayStreamSetting.cpp:403 (log panel); src/configs/common/xrayStreamSetting.cpp:559 (log panel) |
| Failed to parse FinalMask JSON: %1 | Не удалось прочитать JSON FinalMask: %1 | src/configs/common/xrayStreamSetting.cpp:767 (log panel) |
| Skipping an endpoint rule whose endpoint is no longer in the routing profile | Пропущено правило конечной точки, которой больше нет в профиле маршрутизации | src/database/entities/RouteProfile.cpp:646 (log panel) |
| Aborted generating routing section, an error has occurred | Не удалось собрать маршрутизацию: произошла ошибка | src/database/entities/RouteProfile.cpp:651 (log panel) |
| The outbound described in the rule chain is missing, maybe your data is corrupted | Сервер из правила не найден, возможно, данные повреждены | src/database/entities/RouteRule.cpp:168 (log panel) |
| Could not find group with id %1 | Группа с id %1 не найдена | src/database/ProfilesRepo.cpp:403 (log panel) |

## 5. Translated in place

| Original | Russian | Where it shows |
| --- | --- | --- |
| Throne crashed / Throne %s (%s) crashed. ... Please attach the .txt file ... | Throne аварийно завершилась / Throne %s (%s) аварийно завершилась. ... Приложите файл .txt (и .dmp, если получится) к сообщению об ошибке. | src/sys/windows/MiniDump.cpp (Win32 crash box) |

## 6. Already Russian, kept

The ShadowLos updater (ShadowlosUpdater context: «Нет связи с сервером обновлений», «Файл
обновления не прошёл проверку», «Сервер обновлений ответил ошибкой», «Не удалось сохранить
обновление», «Не удалось проверить обновления», «Не удалось запустить обновление»; one sentence
per core selfupdate kind network / verify / server / disk) and the ShadowLos windows (consent,
«Улучшение обхода», census) were Russian already. The updater line stays as it is: it sits in a
narrow slot next to «Обновить», a longer hint would be cut off.

## 7. Left in English, on purpose

- Raw sing-box / xray lines in the log panel (the mapper's sentence goes on the message box and
  the status card; the log keeps the original for support).
- The journal markers `[Core error]`, `[Fatal]`, `[ERROR]`, `[Warn]` at the start of log lines:
  the dev journal sorts lines by them.
- Internal invariant lines nobody should see (generate.cpp «Corrupted data ...», «INVALID ENT TYPE»,
  «Null ent in validator», RPC «dropped malformed response»).
- Linux and macOS only texts (pkexec, chown/chmod, sudo).
- Profile editor field names and tooltips of protocol settings (OpenVPN / OpenConnect advanced,
  XHTTP, AmneziaWG, TLS tricks; about 450 unfinished ts entries): these are option labels, not
  errors or warnings.

## 8. Screenshots (team\mockups\errors-ru)

Before: dev at 8bf72fa6, built locally; after: this branch. Both 1.0.15-dev, a copied config with
enable_tun=false, Russian UI.

| # | Error | How it was triggered | Files |
| --- | --- | --- | --- |
| 1 | Port in use on connect | real: the test held 127.0.0.1:2781 (the copy's mixed port) | 1-port-in-use-before.png, -before-window.png, -after.png, -after-window.png |
| 2 | Cache file locked | real: the test held config\cache.db open without sharing | 2-cache-locked-before.png, -after.png, -after-window.png (status card line) |
| 3 | Subscription DNS failure | real: subscription URL on a non-existent .invalid host | 3-subscription-dns-before.png, -after.png (log panel) |
| 4 | Admin rights for TUN | demo switch SHADOWLOS_UI_DEMO=err-admin (dev only); no before: TUN was off on purpose | 4-tun-admin-after-demo.png |
| 5 | Another VPN | demo switch SHADOWLOS_UI_DEMO=err-vpn (dev only); no before | 5-another-vpn-after-demo.png |
