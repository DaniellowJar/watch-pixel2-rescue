# Hacking Niceboy Watch Pixel 2 (JieLi JL7012) ⌚️🔨

Tento dokument slouží jako post-mortem a technický deník z pokusu o reverzní inženýrství a úpravu firmwaru chytrých hodinek Niceboy Watch Pixel 2. 

Významnou část automatizace, dekompilace a psaní Python skriptů obstaral AI agent **OpenCode**, což extrémně urychlilo celý proces od analýzy sítě až po úpravu Android aplikace.

## Fáze 1: Sběr dat a odposlech sítě
Vše začalo snahou zjistit, jak oficiální aplikace Niceboy WATCH stahuje aktualizace. 
K odposlechu HTTPS provozu byl použit **PCAPdroid**. Aplikace sice zpočátku odmítala uživatelské certifikáty, ale díky zachycení komunikace na endpointy `api.iot-solution.net` jsme zjistili strukturu požadavku na OTA (Over-The-Air) update.

API nám vrátilo URL s firmwarem:
`1757070751898_WATCH_PIXEL_2_V008_ota_release_2508271417.ufw` (velikost cca 1.67 MB).

## Fáze 2: Analýza `.ufw` kontejneru a překvapení z hardwaru
Původní předpoklad byl, že hodinky běží na platformě Sifli. OpenCode analyzoval hlavičku `.ufw` a zjistil, že:
1. Kontejner obsahuje magické footery `JLUFW`.
2. Odkryté textové řetězce (např. `cpu/br28/ui_entry.c`) jasně ukázaly na architekturu **JieLi BR28 (série JL7012)**.
3. Payload byl šifrovaný a z aplikace nešlo vyčíst žádný klíč (hardwarové zabezpečení v SoC).

## Fáze 3: Prolomení "šifrování"
OpenCode byl vyslán na průzkum asijských fór a GitHubu, kde objevil repozitáře vývojáře `kagaimiq` (`jl-misctools`). 

**Obrovský průlom:** Zjistili jsme, že JieLi nepoužívá k ochraně firmwaru silné AES šifrování, ale pouze triviální **LFSR keystream + CRC-16**.
* **Klíč obálky:** `0xFFFF`
* **Klíč k čipu (Chipkey):** `0xB165` (nedbale uložený v nešifrovaném konfiguračním souboru `isd_config.ini` přímo ve firmwaru).

OpenCode následně úspěšně dešifroval a rozbalil celý souborový systém SFC. Dostali jsme se ke grafice, zvukům i k hlavnímu binárnímu kódu `app.bin`.

## Fáze 4: Modifikace a Repacking
Pro Proof-of-Concept jsme se rozhodli přepsat název zařízení přímo v binárce.
1. OpenCode vyhledal v `app.bin` řetězec `WATCH_PIXEL_2` a nahradil ho za `HACKD_PIXEL_2` (zachována stejná délka bytů).
2. Pro opětovné zašifrování použil OpenCode elegantní metodu **diferenciálního XOR keystreamu**, čímž jsme se vyhnuli složité rekonstrukci LFSR.
3. Skripty znovu přepočítaly všechny CRC-16 součty a vytvořily validní `custom_update.ufw`.

## Fáze 5: MITM útok přes upravené APK
Abychom donutili hodinky stáhnout náš upravený firmware, OpenCode zautomatizoval úpravu oficiální aplikace:
* Pomocí `apktool` dekompiloval `base.apk`.
* V `.smali` souborech nahradil produkční URL `api.iot-solution.net` za lokální IP testovacího serveru (např. `http://192.168.3.190:80`).
* Aplikaci znovu sestavil a podepsal lokálním klíčem (`modded_watch.apk`).

Zároveň OpenCode napsal Python skript `serve_ufw.py`, který odpovídal na POST požadavky podvrženým JSONem (s verzí `9.9.9`) a následně servíroval náš `custom_update.ufw`.

## Fáze 6: Zkouška ohněm a Brick
Po instalaci upravené aplikace a spuštění OTA aktualizace hodinky přijaly soubor. Nicméně pokus zapsat upravená data (pravděpodobně kvůli přesunu payloadu na nebezpečný offset) způsobil pád bootloaderu a hodinky se **bricknuly** (zůstaly s černou obrazovkou a bez Bluetooth signálu).

## Fáze 7: Plán na hardwarovou záchranu (Work in Progress)
Čipy JieLi obsahují nezničitelný **Mask-ROM s USB bootloaderem**.
* K oživení je nutné hodinky fyzicky otevřít (přes zadní "snap-off" dýnko).
* Napájet datové vodiče na USB piny základní desky (D+, D-).
* Poslat bit-bang záchranný signál (`0x16EF` na 50kHz) přes Arduino nebo Raspberry Pi, aby se čip probral a přihlásil k PC, což umožní nahrát čistý tovární firmware přes nástroj `isd_download`.

## Budoucí vize (Až budou hodinky oživeny)
* Proxy aplikace v telefonu komunikující přes Bluetooth GATT (ovládání Spotify, Home Assistant).
* Běh odlehčeného RT-Thread OS.
* Nativní aplikace s kompilovaným C kódem (Pomodoro, Kalkulačka).