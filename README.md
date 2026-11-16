# Záchrana Niceboy Watch Pixel 2 (JieLi JL7012 / BR28)

Všechno na Macu, žádný Windows. Hodinky se bricknuly vlastní OTA; v čipu je
ale **Mask-ROM USB bootloader**, který nejde zničit — a bricknutý čip se do
download módu klidně dostane sám (nemá z čeho nabootovat).

## Co máme kde

| Co | Kde |
|---|---|
| SoC | JieLi **JL7012** (BR28 / AC701N) — per `cpu/br28/ui_entry.c` v `.ufw` |
| Firmware | `.ufw` kontejner, magic `JLUFW`, LFSR keystream + CRC-16, envelope klíč `0xFFFF`, chipkey `0xB165` (z `isd_config.ini`) |
| Tool (flash) | `kagaimiq/jl-uboot-tool` — `tools/jl-uboot-tool/` (BR28 podporován, loader `br28loader.bin` už v repu) |
| Firmware nástroje | `kagaimiq/jl-misctools` — `tools/jl-misctools/` (`fwunpack_newfw.py`, `recrypt.py`, …) |
| Refs na čipy | `kagaimiq/jielie` — `tools/jielie/` |
| **macOS SCSI backend** | **přidaný sem**: `jl-uboot-tool/scsiio/jlsgio.c` + `macosport.py` (tool měl jen Linux/Windows) |
| Arduino | `arduino/jl_usbkey/jl_usbkey.ino` — generátor USB_KEY `0x16EF` @ 50 kHz |

## Zapojení (debug pady hodinek: DP · GND · VCC · DM, zleva, kolečko vpravo)

```
DP (D+) ──470R──┬── Arduino pin 8   (clock)
DM (D-) ──470R──┬── Arduino pin 9   (data)
GND     ──────────── Arduino GND
VCC     ──────────── napájení (bateriová lišta 3,7–4,2 V nebo 5 V podle toho, jak jsi to
                      napájel minule — OVĚŘ multimetrem, ať něco nespálíš!)
                 └ 1 kΩ pullup DP→3,3 V a 1 kΩ DM→3,3 V (kvůli detekci ACK)
```

Pro samotné flashování z Macu: DP/DM hodinek do **volného USB portu Macu**
(přes breakout/pogo kabel). Mega jen „probouzí" — USB host neumí.

## Setup (jedenkrát)

```bash
./setup.sh
```

Vytvoří `tools/.venv`, nainstaluje závislosti a zkompiluje `scsiio/jlsgio`.
Pak všechna volání Pythonu přes `tools/.venv/bin/python`.

## Postup

### 0. Nejprv zkus, jestli se čip sám přihlásí

Bricknutý čip → ROM nemá z čeho nabootovat → **sám** vleze do USB download módu.
Takže: napájení na VCC, DP/DM do Macu, pak:

```bash
cd tools/jl-uboot-tool
../.venv/bin/python jldevfind.py     # hledá UBOOT/UDISK/DEVICE zařízení, opakuje každou sekundu
```

Pokud najde `BR28 UBOOT1.00` — skoro vyhráno, jdi na krok 2. (Když macOS hlásí
„disk který nejde připojit" — to je ono.)

### 1. Když se nepřihlásí — USB_KEY přes Arduino

Nahraj `jl_usbkey.ino`, Serial Monitor @115200, **zapni napájení VCC**.
Sketch posílá `0x16EF` (D+ clock 50 kHz, D− data MSB-first) dokud čip
nepotáhne obě linky dolů (ACK). Jakmile uvidíš `ACK!`, **přepoj DP/DM na Mac**
a hned běž `../.venv/bin/python jldevfind.py`.

### 2. Dump fleše (záloha + diagnóza)

```bash
# interaktivně (nebo --device /dev/diskX rovnou)
../.venv/bin/python jluboottool.py --chip br28

=>JL: read 0x0 <velikost> flash_dump.bin     # velikost: 0x100000=1M … 0x400000=4M, viz fleš čip
=>JL: exit
```

Pokud macOS hlásí zamčený disk, odmounti nejdřív: `diskutil unmountDisk /dev/diskX`.
Z dumpu zjistíme, co přesně brick zabil (header/uboot/app) a co přežilo.

### 3. Čistý tovární firmware

Stáhni znovu originální OTA (postup z tvého PCAPdroid záchytu, endpoint
`api.iot-solution.net`, soubor `…_WATCH_PIXEL_2_V008_ota_release_2508271417.ufw`,
~1,67 MB). **Ne** custom_update.ufw — ten je ta brick příčina.

Rozbal (klíče 0xFFFF/0xB165 si `fwunpack_newfw` vezme z hlavičky/sám):

```bash
cd tools/jl-misctools/firmware
../../.venv/bin/python fwunpack_newfw.py ../../../firmware/WATCH_PIXEL_2_V008_ota_release_2508271417.ufw
```

→ dostaneš dekryptovaný flash image + `uboot.boot`, `app.bin`, … 

### 4. Sestavení image k zápisu

Porovnej dekryptovaný image s tvým dumpem (krok 2). Typicky stačí **přepsat jen
rozbité místo**, a hlavně **zachovat `key_mac`** (BT MAC + efuse data) z dumpu —
je na konci fleše a je per-kus. Logika:

1. vezmi dekryptovaný image z `.ufw` jako základ,
2. na offset `key_mac` nakopíruj data z dumpu,
3. `jluboottool.py` → `write <offset> <file>` + `reset`.

### 5. Reset a ověření

`=>JL: reset` (nebo odpoj napájení a znovu připoj). Hodinky by měly nabootovat.
Znovu `read`/porovnej a případně sprav.

## Bezpečnostní poznámky

- **Napětí**: VCC pin — pokud jde na bateriovou lištu (3,7 V), nedávej 5 V.
  Zjisti minule použité napětí, jinak změř.
- Mega IO je 5 V, USB linky 3,3 V — sériové odpory + pullupy na 3,3 V (viz schéma).
- Než začneš zapisovat: **dump je jediná kopie** jediného kusu — drž si ho stranou.
- Po flashnutí bacha na to, že úprava APK + MITM server ti zůstaly — OTA z `api.iot-solution.net`
  ti teď pošle čistý V008, ale tvá modifikovaná aplikace by mohla poslat zase custom.
  Pro flash přes USB na tom ale nezáleží.

## Stav nástrojů

- [x] `scsiio/jlsgio.c` + `macosport.py` — postaveno a scan běží na Macu
- [x] `jldevfind.py` — darwin větev
- [x] `jl_usbkey.ino`
- [ ] Ověřeno na živém hardwaru — **další krok: připoj hodinky a spusť krok 0**