# Ampero II Stomp – Firmware- und Preset-Format

Reverse Engineering anhand von `Ampero II Stomp Firmware V2.5.0.bin`, dem Windows-Editor
(`HTUSBTools.dll`, `data/flutter_assets/assets/data/*.json`) sowie den Presets in `presets/`.
Adressen `RVA 0x…` beziehen sich auf `HTUSBTools.dll` (x64, ImageBase 0x180000000).

Zusätzliche Quelle: die Android-App „Ampero Stage“ (`com.hotoneaudio.amperostage` 1.2.2, Flutter, Dart 3.3.4
arm64), deren `libapp.so` mit [blutter](https://github.com/worawit/blutter) dekompiliert wurde. Der Dart-Code
heißt intern `package:mp350/…`. Feld- und Funktionsnamen in `code`-Schrift ohne RVA stammen von dort
(v. a. `model/HTModelPreset.dart` und `bluetooth/trans.dart`). **[Dart]** = aus diesem Code abgeleitet.
Achtung: Die App ist für den Ampero II **Stage** (MP-380, mit ESP32-Bluetooth) gebaut; Teile davon
(BLE-Transport, Split/Mixer) betreffen den Stomp nicht.

> **Hinweis:** Firmware, Hotone-Software, Mitschnitte (`captures/…`), Screenshots (`data/…`) und Werkspresets, auf
> die dieses Dokument verweist, sind urheberrechtlich geschützt bzw. enthalten Hotone-Material und liegen **nicht**
> in diesem Repository. Die Verweise dienen nur der Nachvollziehbarkeit.

Legende: **[verifiziert]** = durch Prüfsumme bzw. byte-identischen Roundtrip bestätigt,
**[Code]** = aus der Disassembly abgeleitet, **[Hypothese]** = aus Daten geschlossen, nicht bestätigt.

## Benutzung

```
python tools/ampero_fw.py info --verify  "Ampero II Stomp Firmware V2.5.0.bin"
python tools/ampero_fw.py unpack "Ampero II Stomp Firmware V2.5.0.bin" out/fw250
python tools/ampero_fw.py pack   out/fw250 neu.bin          # wieder HTFW (byte-identisch, wenn unverändert)

python tools/ampero_preset.py decode preset.prst            # Zusammenfassung mit Effekt-/Parameternamen
python tools/ampero_preset.py decode preset.prst -o p.json  # vollständiges, editierbares JSON
python tools/ampero_preset.py build  p.json -o neu.prst     # JSON -> .prst/.arr/.bin/.raw
python tools/ampero_preset.py convert preset.bin -o preset.prst
python tools/ampero_preset.py decode out/fw250/factory_presets/001_Slick\ Drive.raw

python tools/ampero_sysex.py decode wireshark.pcapng --dump out/sysex   # Protokoll-Zeitleiste
python tools/ampero_preset.py decode out/sysex/050_in_01000000.bin      # Preset aus SysEx-Dump
python tools/ampero_sysex.py scene 2                        # SysEx für „Szene 3 wählen“
python tools/ampero_sysex.py encode 01030001 0c2021         # beliebigen Befehl erzeugen

python tools/lzo1x.py d|c <in> <out>                        # LZO roh

python tools/usbmon_capture.py -o trace.txt                 # Linux: USB-MIDI mitschneiden (usbmon) → Trace für den Web-Sniffer
```

Nur Python ≥ 3.8, keine externen Pakete.

---

## 1. Kompression: miniLZO 2.10

* Die DLL enthält miniLZO 2.10 („Mar 01 2017“).
  `toolStartDecompress` → `lzo1x_decompress` (RVA 0xd3660), `toolStartCompress` → `lzo1x_1_compress` (RVA 0xd3a20).
* `tools/lzo1x.py` ist ein reiner Python-Port beider Funktionen. Der Kompressor bildet auch die
  64-Bit-Match-Verlängerung des x64-Builds nach und erzeugt deshalb **byte-identische** Ausgaben:
  5/5 Editor-Presets, 99/99 Werkspresets, komplette Firmware **[verifiziert]**.

## 2. Firmware-Container „HTFW“ (ab V2.5.0)

**Es gibt keine Verschlüsselung.** Die Nutzdaten sind ein einziger LZO1X-Stream.

| Offset | Typ | Inhalt (V2.5.0) |
|---|---|---|
| 0x00 | char[4] | `HTFW` |
| 0x04 | u16 **BE** | CRC-16/MODBUS über Datei[0x06..EOF] (`26 83`) **[verifiziert]** |
| 0x06 | u16 | Formatversion? (1) |
| 0x08 | u32 | Dateigröße (0x00B530DF) **[verifiziert]** |
| 0x0C | char[16] | Modell `MP-300` |
| 0x1C | u8[4] | Version `'V',2,5,0` |
| 0x20 | u8 | unbekannt (0) → wird zu FRMW+0x08 |
| 0x21 | u8 | 1 = Payload LZO-komprimiert |
| 0x22 | u8 | Anzahl Sektionen (1…8, hier 4) |
| 0x23 | u8 | reserviert |
| 0x24 | u32 | Größe des entpackten Payloads (0xFC1AF4) |
| 0x28 | u8[16] | reserviert (Bytes 0/1 → FRMW+0x20/0x21) |
| 0x38 | 16 B × n | Sektionstabelle |
| 0x38+16n | u32 + LZO | entpackte Größe (nochmals), dann LZO1X-Stream bis EOF |

Sektionseintrag (16 Byte): `u16 BE crc16_modbus(sektion)`, `u8 reserviert`, `u8 typ`,
`u32 reserviert`, `u32 offset` (im entpackten Payload), `u32 größe`. Alle 4 CRCs **[verifiziert]**.

| Typ | Größe V2.5.0 | Inhalt |
|---|---|---|
| 0x00 | 0x458000 | ARM-Firmware (NXP i.MX RT1052) |
| 0x01 | 0x03C3C0 | Werks-IR-Bibliothek: 30 × (`char name[32]` + 2048 × int32 LE) |
| 0x02 | 0x018C00 | Werkspresets: 99 Slots à 0x400 |
| 0x10/0x11 | 0xB14B34 | DSP-Firmware (gleicher Aufbau wie in V2.3.1) |
| 0x20/0x21 | – | nicht vorhanden; die DLL liest dort einen `V x.y.z`-String ab +0x30 |

`ampero_fw.py pack` erzeugt aus den entpackten Sektionen wieder eine Datei, die **byte-identisch**
zum Original ist **[verifiziert]**.

### 2.1 Was der Editor daraus macht [Code]

`HTUSBTools.dll` RVA 0xcc3a0 (bzw. 0xd02e0 im Update-Pfad) entpackt HTFW und baut daraus ein
**Legacy-„FRMW“-Image**, das an das Gerät geschickt wird (Format wie V1.0.4–V2.3.1):

* Header 0x30: `FRMW`, `u32 gesamt = 0x30 + Σ Sektionsgrößen`, `u16 = HTFW[0x20]`,
  `[0x0A] = HTFW[0x1D]`, `[0x0B] = HTFW[0x1C]`, `[0x0E] u16 = Σ Bytes[0x30 .. 0x30+gesamt) & 0xFF`,
  `[0x10] Modell`, `[0x20..0x21] = HTFW[0x28..0x29]`, `[0x22..0x29]` Versionen aus Sektion 0x20/0x21.
* Tabelle 0x30: 6 × (`u32 offset relativ zu 0x30`, `u32 größe`) in der Reihenfolge ARM, DSP, IR, Presets, 0x20, 0x21.
* Die 8-Bit-Prüfsumme bei 0x0E stimmt bei V1.0.4, V2.2.0 und V2.3.1 **[verifiziert]**.
* `ampero_fw.py unpack` schreibt dieses Image als `legacy_frmw.bin`, damit bestehende Ghidra-Workflows
  (z. B. das Projekt zu V2.3.1) weiter funktionieren.

### 2.2 ARM-Sektion

| Offset | Inhalt |
|---|---|
| 0x00 | u32 Flash-Adresse Teil A (0x0008001C), u32 Größe A, u32 Flash-Adresse Teil B, u32 Größe B |
| 0x10 | Magic `55555555 AAAAAAAA A5A5A5A5` |
| 0x1C | Teil A: Vektortabelle (SP 0x20004000, Reset 0x000004E9) → läuft aus ITCM ab Adresse 0 |
| 0x1C+A | Teil B: beginnt mit `V2.5.0\0\0MP-300`, SDRAM-Code. Pointer-Analyse: B+0x198 ≙ 0x80E00000 **[Hypothese]** |

In Teil B liegen zwei GIFs (u. a. Boot-Animation 480×800); sie sind der einzige hoch-entropische
Bereich im ARM-Code und werden nach `gif/` extrahiert. Bei 0xE37C0 liegt eine CRC-16/CCITT-Tabelle.

### 2.3 Werkspresets (Sektion 0x02)

99 Slots à 0x400: `u32 0`, `u32 komprimierte Länge`, `u32 CRC32(zlib) der komprimierten Bytes`
**[verifiziert 99/99]**, `u32 0`, `u32 0`, LZO-Stream, Rest 0xFF. Entpackt **8700 Byte**, flaches Layout
(siehe 4.).

## 3. Editor-Preset (`.prst` / `.arr` / `.bin`)

```
.prst = base64(.arr)    .arr = "[b0,b1,…]"    .bin = LZO1X-1(raw)    raw = 5004 Byte
```

`raw` ist eine **TLV-Kette** `u32 tag, u32 len, data[len]` mit den Tags 0–9 **[verifiziert]**.
Dieselbe Kette kommt auch per SysEx vom Gerät (Abschnitt 6), dort 5068 statt 5004 Byte, weil
Tag 7 auf dem Gerät 178 statt 114 Byte lang ist; Tag 1 hat dort `version = 3` statt 2.

Die Zuordnung Tag → Sektion und die Feldnamen stammen aus `HTModelPreset.getSendData()`, das die Sektionen
in genau dieser Reihenfolge zusammensetzt (`getSettingData`, `getInfoData`, `getKnobData`, …) **[Dart]**.
Die Byte-Layouts unten ergeben exakt die beobachteten Längen und wurden an `presets/*.bin` (5004 B) und
am Geräte-Dump `out/sysex/050_in_01000000.bin` (5068 B) gegengeprüft.

| Tag | Länge | Name im Tool | Dart-Sektion | Inhalt |
|---|---|---|---|---|
| 0 | 10 | header | `Setting` | `dataST` (int8-Array); `03 02 0C 02 03 03 01 01 03 06` in allen Samples (Datei und Gerät) – vermutlich Schema-/Dimensionsangaben **[Hypothese]** |
| 1 | 101 | info | `Info` | `u32 version`, `char name[17]`, `char patchAuthorInfo[20]`, `char info[50]`, `char pathTypeinfo[10]` (Style, z. B. „Acoustic“) **[Dart]** |
| 2 | 6 | tag2 | `Knob` (Quick-Knobs) | `u8 obj[3]` = Ziel je Knopf: **Kettenposition** 0–11, `0x0C` Patch Volume, `0x0D` Patch Tempo (am Pedal getestet), `0xFF` OFF, `u8 param[3]` = `Widget.ID` (0 bei Funktionen und OFF). OFF bestätigt am Export `data/A98-1-MIDI-Test2.prst` (Para 3 auf OFF: obj `FF`, param `00`). Set für OFF `knob FF FF FFFFFFFF FF` (8 Byte) – vom Web-Editor gesendet, am Pedal getestet, nicht mitgeschnitten. Export `data/A98-1-MIDI-Test 2.prst`: obj `01 0C 04`, param `01 00 02` = A2 Blue Sustainer (Slot 0!) Attack, Patch Volume, A5 Green Drive Volume [Mitschnitt `captures/wine-quick-knobs-2026-11-18.txt`, Screenshots `data/quick1..4.png`]. Der Hotone-Editor bietet beim Stomp außerdem „Input Level A“ an (Code noch unbekannt) |
| 3 | 8 | footswitch | `FS` | `u8 click[3]` = Funktion FS1–FS3 bei Druck **[verifiziert per SysEx 0x0103]**, `u8 hold[3]` = Funktion bei Halten (0xFF = keine), `u8 comb[2]` = Kombinationen (`10 0F` in allen Samples) **[Dart]**. Codes siehe Footswitch-Funktionscodes in 6.5, z. B. 0x0C Ctrl 1, 0x0D Tap Tempo, 0x1B/0x1C Scene 1/2, 0x20/0x21 Ctrl 2/3, 0x29 EXP 1/2 |
| 4 | 72 | slots | `Effect` | `u8 seq[12]` (Slot-Reihenfolge), **`u8 mod[12]`** = Modulkategorie je Slot = `Catalog[].Index` der Algorithmus-DB (0 DYN, 2 WAH, 3 DRV, 4 AMP, 6 CAB, 9 MOD, 10 DLY, 11 RVB, 15 VOL …; 0xFF = leer) – der frühere „0x0F-Marker“ ist das VOL-Modul. **`u32 alg[12]`** (0xFFFFFFFF = leer) |
| 5 | 3989 | scenes | `Scene` | siehe unten |
| 6 | 540 | tag6_exp ¹ | `Temp` (Routing-Vorlage) | `i16 inputSource[2]`, `i16 outputSource[2]`, `f32 inputSourceParam[56]`, `f32 outputSourceParam[56]`, `u8 splitNodeAddr`, `u8 mixerNodeAddr` (0xFF = kein Split/Mixer), `u8 splitNodeMode`, `u8 mixerNodeMode`, `f32 splitNodeModeParam[10]`, `f32 mixerNodeModeParam[10]` **[Dart]**. Mixer-Defaults `100 0 100 0 0 50 0` passen zu den Split-Mixer-Parametern A LEVEL/A PAN/B LEVEL/B PAN/B POLARITY/MASTER … |
| 7 | 114/178 | tag7 | `Exp` | `u8 expState[2]` (EXP1/EXP2), `u8 expTargetAlgorObjectList[8]` (Ziel-Slot), `u8 expTargetAlgorEffectNum[8]` (Parameterindex), `f32 expTargetAlgorMin[8]` (0.0), `f32 expTargetAlgorMax[8]` (100.0), `f32 expTargetAlgorCurve[8]` – 8 Ziele, 0xFF = unbenutzt **[Dart]**. Nur auf dem Gerät (178 B) folgen `f32 autoOff[8]` und `f32 offTime[8]` (600.0) aus `getExpAutoOffAndOffTimeData` – das erklärt die 64 B Differenz zwischen Datei und Gerät |
| 8 | 39 | tag8_fs_slots ¹ | `Ctrl` | `u8 ctrlLED[3]` (LED je FS), `u8 ctrlTargetSlot[3][12]` = welche **Kettenpositionen** (A1–B6, nicht Slots!) FS1–FS3 schalten (1 = ja) – entspricht SysEx 0x01070001 **[Dart]**; beim Verschieben eines Blocks bleibt die Zuordnung an der Position (Mitschnitt `captures/wine-fs-position-2026-11-18.txt`) |
| 9 | 45 | tag9_midi | `Midi` | `u8 midiData[45]`, in allen Samples 15 × `FF 00 00` – vermutl. 15 MIDI-Messages (Kanal aus, CC, Wert) **[Hypothese]** |

¹ Die Namen im Tool stammen aus der Zeit vor der Dart-Analyse: Tag 6 ist **nicht** das Expression-Pedal
(das ist Tag 7), und Tag 8 ist die FS→Slot-Zuordnung inkl. LED-Bytes vorn.

**Tag 5 – Szenen** (3 Szenen, 12 Slots, 25 Parameter):

| Offset | Typ | Inhalt |
|---|---|---|
| 0 | float32 [3][12][25] | Parameterwerte je Szene/Slot; Index = `Widget.ID` aus der Algorithmus-DB **[verifiziert]** |
| 3600 | u8 [3][12] | Slot an/aus je Szene |
| 3636 | u16 [3] | Szenen-Lautstärke (100) |
| 3642 | u16 [3] | Szenen-Tempo in BPM (120) |
| 3648 | 317 B | unbekannt (`01 00 00` + 0xFF-Füllung). Dart-Felder nach `patchTempo`: `empty` (int8-Array), `vol` (1), `bpm` (1), `sta` (int8-Array), `para` (int8-Array) – vermutlich Leer-Flags je Szene und „Scene Follow“-Einstellungen (`HTModelSceneFollow`) **[Hypothese]** |
| 3965 | char [3][8] | Szenennamen („Scene 1“ …) |

Die Parameter sind **Anzeigewerte** (z. B. 50.0 = 50 %, Hi Cut 20001.0 Hz), keine normierten Werte.

## 4. Werkspreset-Layout (8700 Byte, nur in der Firmware)

| Offset | Inhalt |
|---|---|
| 0x000 | Info wie Tag 1 (101 B: `u32 2`, Name, …, Style) |
| 0x065 | beginnt wie Tag 2 (Quick-Knobs, 6 B) und Tag 3 (FS, 8 B, endet auf `10 0F`); danach `seq[12]` (`00…0B`) und `mod[12]` (Kategorien, 0x0F = VOL) wie in Tag 4 **[Hypothese]** |
| 0x0A0 | **u32 alg[12]** **[verifiziert]** |
| 0x0D8 | **float32 [12][25]**, Slot s bei `0xD8 + s·0x64` **[verifiziert]** |
| 0x558–0x1C30 | 0xFF (Platz für weitere Szenen?) |
| 0x1C31–0x200C | weitere variable Daten (FS/EXP/Szenen?) – nicht dekodiert |

Validierung: Bei allen 99 Werkspresets liegen 810 von 848 belegten Slots vollständig im Bereich
`DisplayMin..DisplayMax` der Algorithmus-DB. Die restlichen 38 sind ausnahmslos Delay-`Time`/
Mod-`Rate`-Werte bei aktivem **Sync** – dann steht dort ein Notenwert-Index statt ms/Hz.

## 5. Algorithmus-Codes

`u32 code`: höchstes Byte = Modultyp, Rest = Algorithmus-Nummer. Quelle der Namen/Parameter:
`flutter_assets/assets/data/v1.0.9_alg_data.json` (`Catalog[].Alg[].Code`, `Widget[].ID`).

| Typ | Modul | Typ | Modul |
|---|---|---|---|
| 0x00 | DYN | 0x07/0x08 | AMP / PRE AMP |
| 0x01 | FREQ / EQ | 0x09 | CAB |
| 0x03 | DRV | 0x0A | IR |
| 0x04 | MOD | 0x0B | DLY |
| 0x05 | WAH | 0x0C | RVB |
| 0x06 | FX LOOP/SND/RTN, VOL | 0x0E | FREQ (weitere) |
| 0x0F | CLONE | 0x10 | NAM |

Die Android-App enthält dieselben Datenbanken (`v1.0.4`–`v1.0.9_alg_data.json`, plus Excel
`Ampero II Series_FX List_V21_260706.xlsx`). Gegenüber dem Windows-Editor kommen in v1.0.8/v1.0.9 vier
Codes dazu, die vermutlich nur der **Stage** hat:

| Code | Modul | Name |
|---|---|---|
| 0x0100004B | Global EQ | Global EQ 2 |
| 0x06000004 | Split Mixer | Split (Mode I) |
| 0x06000005 | Split Mixer | Mixer (A/B LEVEL, A/B PAN, B POLARITY, MASTER …) |
| 0x06000006 | Split Mixer | Split (Mode II) |

Die restlichen Unterschiede sind Beschreibungstexte und Zahlenformate (`"0"` statt `"0.0"`).

## 6. SysEx-Protokoll (USB-MIDI) [verifiziert am Mitschnitt `wireshark.pcapng`]

Der Ampero meldet sich als USB-MIDI-Gerät (VID 0x84EF, PID 0x0030), SysEx in USB-MIDI-Eventpaketen
(CIN 4–7) auf Endpoint 0x02/0x82. `tools/ampero_sysex.py` dekodiert pcapng, `.syx` und Hex-Dumps.

**Nachricht** (max. 384 Byte):

```
F0 21 25 4D 50 00 00 | cs | kind | totLo totHi | offLo offHi | Nibbles … | F7
```

| Byte | Bedeutung |
|---|---|
| 1–2 | `ManufactID1`/`ManufactID2` = `21 25` **[Dart]** |
| 3–4 | `productID` = `"MP"` **[Dart]** |
| 5 | `serId` **[Dart]** |
| 6 | `resv` (0) **[Dart]** |
| 7 | Prüfsumme (im Dart-Code `moduleCode` genannt). Host→Gerät: `Σ Nibbles & 0x7F` **[verifiziert 50/50]**. Der Editor prüft eingehende Frames mit **derselben** Regel (`HTUSBTools.dll` RVA 0xce1fa–0xce25e: Nibbles ab Byte 13 bis vor `F7` summieren, `& 0x7F`, mit Byte 7 vergleichen) **[Code]**. Das Gerät hält sich nicht daran; bei Abweichung loggt die DLL `crc fail != crcFlag` und verarbeitet den Frame trotzdem. Die Geräteregel steckt also in der Firmware – getestet und verworfen wurden: Echo der Anfrage, CRC8 (Poly 0x31) und CRC16 aus `lib5868USB.so`, Summen/XOR über Nibbles, Rohbytes, Stream oder entpackte Nutzdaten. **Empfänger sollten Byte 7 ignorieren.** |
| 8 | `transType`: 0x11 = Anfrage, 0x12 = Setzen / Antwort, 0x13 = Fehler (`TransErrorStatus`, im BLE-Pfad gesehen) **[Dart]** |
| 9–10 | Gesamtlänge des Streams in Byte (7-Bit-Hälften) |
| 11–12 | Byte-Offset dieses Chunks im Stream (0, 185, 370, …) |
| 13… | Daten als Nibble-Paare (High zuerst), max. 185 Byte pro Nachricht |

**Stream** = `u32 cmd`, `u32 len`, `data[len]` (Dart: `CmdCode`, `dataLen`, `data`). **`data` ist immer ein LZO1X-1-Stream**, auch bei
1–4 Byte Nutzdaten (`15 29 01 00 00 11 00 00` = Literal-Run 4 Byte + Endmarke). Ein leerer Request hat
`len = 0`. `ampero_sysex.py encode` erzeugt alle 50 Host-Nachrichten des Mitschnitts **byte-identisch**;
die Geräteantworten stimmen bis auf das Prüfsummenbyte ebenfalls überein.

Korrekturen gegenüber dem älteren C#/Arduino-Decoder: Byte 7 ist kein Befehl, sondern die Prüfsumme;
`12 6D 03` = Typ 0x12 + Gesamtlänge 0x6D + 3·128 = 493; die „4 unbekannten Bytes“ sind `cmd`, das
4-Byte-„Präfix“ des entpackten Presets ist die **Presetnummer**.

| cmd | Richtung | Nutzdaten (entpackt) |
|---|---|---|
| 0x05000017 | → | Verbindungsaufbau (leer) |
| 0x00090000 | ↔ | aktuelles Preset: `u32 index` (0x11 = abfragen, 0x12 = wählen) |
| 0x01000000 | ↔ | Preset-Dump: Anfrage `u32 index` → `u32 index` + TLV-Preset (5068 B) |
| 0x01080000 | ← | Presetliste: `u16 order[300]`, `char name[300][17]`, weitere 24000 B |
| 0x03000001 | ↔ | Szene: Setzen `u8 szene` (0–2), Antwort/Meldung `u32 szene` |
| 0x03000003 | ← | Statusmeldung `u32` (kam nach Footswitch-Änderung mit 1) |
| 0x03000005 | ← | Firmwareversion `"V2.5.0"` |
| 0x04000003 | ← | User-IR-Namen (50 × char[32]) |
| 0x08000003 | ← | 540 B, beginnt mit „Fortin Cali DI 1“ (Clone/NAM-Liste?) |
| 0x01030000 | ← | Footswitch-Block = TLV-Tag 3 (8 B) |
| 0x01030001 | → | Footswitch-Funktionen setzen: `u8 fs_func[3]` |
| 0x01070001 | → | FS-Zuordnung: `u8 fs (0–2)`, `u8 an[12]` je Kettenposition (A1–B6) |
| 0x010b0001 | → | 11 B (`02 00 … 07 00 00`), nach FS3-Zuordnung – unbekannt |
| 0x000a0000, 0x0300000a, 0x07000003, 0x05000013, 0x00070000, 0x010a0003 | ← | beim Verbinden abgefragt, Bedeutung offen (0x00070000 endet mit u16 120 = Tempo?) |

Ablauf im Mitschnitt: Preset wählen (`00090000` mit Index 0/8/6) → `01000000` holt den Dump →
`03000001` fragt die Szene ab. Szenenwechsel 1→2→0→1→2 per `03000001` mit 1 Byte; ein Wechsel am
Gerät kommt als `03000001`-Meldung zurück. Footswitch-Zuweisung bei „Juicy Fusion“: FS1 → Slots 1 (WAH)
+ 6 (MOD), FS2 auf Funktion 0x20 → Slot 8 (RVB), FS3 auf 0x21 → Slot 4 (AMP).

Daneben gibt es Kurzframes mit Header `F0 21 25 7F 4D 50 01 2C` (z. B. Befehl 0x3800 als Keepalive
alle 200 ms beim Verbinden); die baut die DLL in RVA 0xc5630 und sie sind nicht LZO-komprimiert.

### 6.1 Android / Bluetooth

* `lib5868USB.so` (Android) hat dieselbe API wie `HTUSBTools.dll` (30 von 31 Exporten gleich, es fehlt
  nur `checkDeviceNeedUpdateBoot`). Über USB läuft MIDI dort per JUCE über die Android-MIDI-API. Den
  `crc fail`-Check gibt es in der Android-Bibliothek nicht.
* Der Dart-Ordner `bluetooth/` (`trans.dart`, `commProcess.dart`, `HTaskManager.dart`) ist der
  BLE-MIDI-Transport zum ESP32 des **Stage** (Standard-Characteristic `7772e5db-3868-4112-a1a9-f2669d106bf3`).
  Er nutzt denselben SysEx-Header, dazu Quittungen (`SendTransRSP`), Nachforderung verlorener Chunks
  (`SendRetransreq`, „[PackageLost]“) und `transType` 0x13. Für den Stomp ohne Bluetooth irrelevant.
* `CRCCalculator.crc16` in Dart ist CRC-16/MODBUS (für Tone-Clone-Dateien, `tcCrc`); `calcCRC` ist eine
  16-Bit-Summe für Preset-/Cloud-Uploads. Beide haben mit SysEx nichts zu tun.

### 6.2 Edit-Befehle [verifiziert am Mitschnitt `captures/wine-edit-2026-11-17.txt`]

Mitschnitt: Hotone-Editor unter Wine, aufgezeichnet mit `tools/usbmon_capture.py` (usbmon, Linux). Die
Layouts stammen aus der Android-App (Funktionsname in Klammern) und stimmen mit dem Mitschnitt überein; der
Preset-Dump nach dem Speichern enthält jede Änderung. Alle als `kind` 0x12, alle Zahlen little-endian.

| cmd | App-Funktion | Payload |
|---|---|---|
| 0x01040003 | `algKnobChangeValue` | `u16 slot`, `u16 param` (= `Widget.ID`), `f32 value` (Anzeigewert). Gilt für die **aktuelle Szene** (Parameter sind pro Szene, Tag 5; die Szene steht nicht im Befehl, vorher ggf. mit `0x03000001` wählen). Beim Drehen ~12/s |
| 0x01040002 | `setCurretAlgWithMsg` | `u8 slot`, `u8 category` (`Catalog[].Index`), `u32 alg code`, `u8 on`. Setzt die Parameter des Slots in **allen drei Szenen** auf die Standardwerte |
| 0x01040001 | `effectChainReorder` | `u8 seq[12]` (wie Tag 4 `seq`) |
| 0x01090004 | `setCurretAlgStateWithScene` | `u8 scene`, `u8 on[12]` |
| 0x01090007 | `setSceneName` | `u32 scene`, `char name[8]` – das Gerät übernimmt nur bis zur ersten 0 (der Rest des alten Namens bleibt dahinter stehen) |
| 0x03000001 | `changeScene` | `u8 scene` |
| 0x05000000 | `savePreset` | `u32 preset`, `char name[17]`; danach liest der Editor das Preset neu |

Das Gerät quittiert diese Befehle nicht einzeln. Es meldet beim ersten Ändern `0x03000003 = 1`
(Preset geändert), nach dem Speichern `= 0`. Nach einem Algorithmuswechsel schickt es `0x01040004`:
`u32 slot`, `u32 category`, `u32 on`, `u32 alg code`, `f32 params[25]` (116 B). `0x010a0003`
(Anfrage `u32 slot`) beantwortet es mit `u32 slot`, `f32` (Rechenzeit/DSP-Last des Slots, z. B. 660.0).

Alle weiteren Befehle der App (Routing `0x0105xxxx`, Expression `0x0106xxxx`, Quick-Knobs `0x01020001`,
Global-EQ, Drum-Machine, IR/Clone/NAM-Verwaltung, Preset löschen/verschieben …) sind mit ihren
App-Layouts im Web-Sniffer hinterlegt (`ampero/js/sniffer.js`, `KNOWN`), aber noch nicht mitgeschnitten.

Pro Szene: Parameterwerte (`0x01040003`, nur aktuelle Szene), Slot an/aus (`0x01090004`, Szene im Befehl),
Lautstärke/Tempo (`0x01090001/2`), Name. Für alle Szenen gemeinsam: Algorithmus je Slot (`0x01040002`) und
Reihenfolge (`0x01040001`).

### 6.3 IR- und NAM-Upload [verifiziert am Mitschnitt `captures/wine-upload-nam-ir-2026-11-17.txt`]

Beide Uploads quittiert das Gerät mit `0x06000000`: `u32 cmd`, `u32 1` (ok), `u32 Nummer`. Danach liest der
Editor die Namensliste neu (`0x04000003` bzw. `0x08000003`). Ablauf und Ziel-Platz zusätzlich im Flutter-Log des
Editors (`data/wine.log`: `[IR] … dest-location:46`).

**User-IR `0x04000000`** (8230 B):

| Offset | Inhalt |
|---|---|
| 0 | `u32` IR-Nummer = **30 + Platz** (Platz 0–49 = „User IR 1–50“, Algorithmus `0x0A100000 + Platz`; 0–29 sind die Werks-IRs) |
| 4 | `char name[32]`, der Editor kürzt auf 16 Zeichen |
| 36 | `i32 sample[2048]`: die WAV auf **44,1 kHz** resampelt (der Editor nutzt dafür `getNormalWav` der DLL), 24-Bit-Werte, Pegel unverändert |
| 8228 | `u16` Prüfsumme = Summe aller Bytes 0–8227 `& 0xFFFF` |

**NAM-Modell `0x08000000`** (32 B + NAMB):

| Offset | Inhalt |
|---|---|
| 0 | `u32` Prüfsumme = Summe aller folgenden Bytes `& 0xFFFF` |
| 4 | `u32` NAM-Platz 0–29 (Algorithmus `0x10000000 + Platz` = „NAM Slot n+1“) |
| 8 | `char name[16]` (ohne 0-Terminator, wenn 16 Zeichen) |
| 24 | `u32 0`, `u32 1` (Bedeutung offen, in der App `namRefresh`) |
| 32 | **NAMB**-Datei |

**NAMB** ist das offene Binärformat von TONE3000 ([nam-binary-loader](https://github.com/tone-3000/nam-binary-loader),
MIT). Der Editor konvertiert mit dessen `nam2namb` (in `HTUSBTools.dll`: `convertNamToNambWithSlim`); NAM-A2-Modelle
(`SlimmableContainer`) werden dabei auf das **kleinste** Teilmodell reduziert (`slim=0`). `tools/nam2namb.py` ist ein
Python-Port und erzeugt **byte-identisch** dieselbe Datei wie im Mitschnitt (7980 B für „TF JTM50 3 - Normal“).
Aufbau: 32 B Kopf (`'NAMB'`, Version 1, Dateigröße, Offset/Anzahl der Gewichte, Größe Modellblock, CRC32 ohne
das CRC-Feld), 48 B Metadaten (Version, Loudness/Pegel als f64, Sample-Rate), Modellblock (Architektur + Konfiguration:
Layer, Dilations, Kernelgrößen, Aktivierungen, Gating, FiLM), danach alle Gewichte als f32.

`0x08000003` (NAM-Liste, 540 B): `char name[30][17]`, dahinter 30 Bytes (Flags?).

### 6.4 Befehlskatalog (alle Codes der Hotone-App)

Quelle: alle Aufrufe von `CommProcess.sendData/requestData` in der dekompilierten Android-App mit den Feldtabellen
der jeweiligen Funktion, dazu die Befehlsgruppen-Tabelle der Firmware (`COMMAND_GROUPS` im Web-Sniffer).
**Status:** ✔ = am Mitschnitt verifiziert, A = Layout aus der App (nicht mitgeschnitten), ? = Bedeutung/Layout offen.
`get` = kind 0x11 (Anfrage, leere Payload, Antwort mit 0x12), `set` = kind 0x12. Felder little-endian.

**Globale Einstellungen** sind *adressierte Parameter*: `0xGGGG0000` (get) liefert die ganze Gruppe, jeder einzelne
Parameter hat einen eigenen Code `0xGGGG00nn` (set, generisch über `setParamValueWithAddr`; Datentyp je Adresse aus
`HTDataAnalysis.getDateCodeTypeWithAddr`). Die App ist für den **Stage** gebaut (Bluetooth, mehr Ausgänge) – Anzahl und
Bedeutung der Parameter beim Stomp müssen per Mitschnitt geklärt werden.

| Gruppe | get | Parameter (set) | App-Seite | Status |
|---|---|---|---|---|
| Ein-/Ausgänge | 0x00010000 | 0x00010001–0x00010011 (13 im get) | `HTSetingInpuOutput` | ? |
| USB-Audio | 0x00020000 | 0x00020001–0x00020008 | `HTSettingUsbAudio` (u. a. „USB Out 1/2“) | ? |
| Global-EQ | 0x00030000 | 0x00030001 an/aus `u32`, 0x00030002 `u32 index, f32 value`, 0x00030003 Ausgang `u32`, 0x00030004 Reset | `HTGlobalEQ` | A |
| ? | 0x00040000 | 0x00040001–0x00040006 | `HTSettingDisplay`/`Control` | ? |
| Drum (Stage) | 0x00050000 | 0x00050001–0x00050003 | Drum-Seite | ? |
| MIDI | 0x00060000 | 0x00060001–0x00060009 (9) | `HTSettingMIDI` („TRS Only“, „BT Only“, „Omni“) | ? |
| Control | 0x00070000 | 0x00070001–0x00070003, 0x00070005 `u32`, 0x00070006, 0x00070007 Global-Tempo `u32` | `HTSettingControl` | A |
| Display/System | 0x00080000 | 0x00080001–0x0008000d (13); 0x00080003 = Namenseditor-Meldung | `HTSettingDisplay` („RGB Normal“, „Intensity“) | ? |
| Drum-Machine | 0x000a0000 | 0x000a0001 Sync, 0x000a0002 Volume, 0x000a0003 BPM, 0x000a0004 Style, 0x000a0005 Play-State (je `u32`), 0x000a0006 | Drum-Seite | A |

**Preset** (aktuelles Preset, live):

| cmd | App-Funktion | Payload | Status |
|---|---|---|---|
| 0x00090000 | `requestCurretIndex` / Preset wählen | `u32 preset` | ✔ |
| 0x01000000 | `FSM_RequestPreset` | get `u32 preset` → `u32 preset` + TLV | ✔ |
| 0x01000001 | `importPresetData` | `u32 preset` + TLV → Quittung 0x06000000 | ✔ |
| 0x01000002 | `getUserTemplateList` (?) | – | ? |
| 0x01010000 | `addPatchRemark` | `u32 preset, u32 version, char name[17], author[20], info[50], style[10]` | A |
| 0x01010001 | `presetRename` | `u32 preset, char name[17]` | A |
| 0x01010003 | `patchBindStyle` | `u32 preset, char style[10]` | A |
| 0x01020000 / 0x01020001 | Quick-Knobs get / `setQKnobTarget/Param` | set: `u8 knob, u8 pos, u8 category, u32 alg` + `u8 param` (Ziel gewählt, 8 Byte) bzw. `u16 param` (nur Parameter geändert, 9 Byte); pos = **Kettenposition**, `0C` Patch Volume (category/alg/param `FF`) – siehe Tag 2 | ✔ |
| 0x01030000 / 0x01030001 | Footswitches get / set | `u8 click[3]` … (Tag 3) | ✔ |
| 0x01040001 | `effectChainReorder` | `u8 seq[12]` | ✔ |
| 0x01040002 | `setCurretAlgWithMsg` / `deleteSlotData` | `u8 slot, u8 category, u32 alg, u8 on`; löschen = `FF`, `FFFFFFFF`, 0 | ✔ / löschen A |
| 0x01040003 | `algKnobChangeValue` | `u16 slot, u16 param, f32 value` (aktuelle Szene) | ✔ |
| 0x01040004 | Gerät → Host nach Algorithmuswechsel | `u32 slot, category, on, alg, f32 params[25]` | ✔ |
| 0x01050001–0x0105000b | Routing (`setInputSource`, `setOutputSource`, `…SourceParam`, `setSplitNodeAddr`, `setMixNodeAddr`, `setSplitMode`, `setSplitNodeParam`, `setMixNodeParam`) | siehe Sniffer `KNOWN` | A |
| 0x01060000–0x01060004 | Expression (`setExpTarget`, `setExpRange`) | siehe Sniffer | A |
| 0x01070000 / 0x01070001 | Ctrl get / `setCtrlSetting` | `u8 fs, u8 on[12]` je Position | ✔ |
| 0x01080000 / 0x01080001 | Presetliste | `u16 order[300], char name[300][17], …` | ✔ |
| 0x01090001 / 0x01090002 | `setVol` / `setBpmWithInt` | `u16 scene, u16 value` | ✔ |
| 0x01090004 | `setCurretAlgStateWithScene` | `u8 scene, u8 on[12]` | ✔ |
| 0x01090005 | `_restoreSlotParams` | `u16 slot, u16 algID, f32 params[25]` | A |
| 0x01090007 | `setSceneName` | `u32 scene, char name[8]` | ✔ |
| 0x010a0002 / 0x010a0003 | `parseSlotData` / `getSlotTimeData` | `u16 slot, u8 mod, u8 state, u32 alg, f32 params…` / `u32 slot` → `u32 slot, f32 time` | A / ✔ |
| 0x010b0001 / 0x010b0002 | `setFSMidiParam` / `setPatchMidiParam` | `u32 fs, u32 id, data` / `u32 id, data` | A |

**Vorlagen, Laufzeit, Dateien, System:**

| cmd | App-Funktion | Payload | Status |
|---|---|---|---|
| 0x02000002 / 0x02000003 / 0x02000004 | `setUserTmplate` / `setEffectData` (Vorlage wählen) / `getUserTemplateList` | `u32 id, char name[12]` / `u32 id` / – | A / ✔ / ✔ |
| 0x03000001 | `changeScene` / `requesetCurretScene` | `u8 scene` | ✔ |
| 0x03000003 / 0x03000005 / 0x0300000a | Änderungsflag / Firmware / Algorithmus-Zeiten | – | ✔ |
| 0x03000006 / 0x03000007 / 0x03000008 | Pegelanzeigen / ? | – | ✔ / ? |
| 0x04000000–0x04000003 | User-IR: Upload / `deleteUserIr` `u32 id` / `renameUserIr` `u32 id, char name[32]` / Namen | | ✔ / A / A / ✔ |
| 0x05000000 | `savePreset` | `u32 preset, char name[17]` | ✔ |
| 0x05000001 | `deletePreset` | `u32 preset` | A |
| 0x05000004 | `onReorder` (Preset verschieben) | `u32 ziel, u32 quelle` | A |
| 0x05000006 | Werkseinstellungen (`HTFactoryResetPage`) | `u32 type, u32 value` | A – **nicht implementiert** |
| 0x05000007 | Gerätename | `char name[17]` | A |
| 0x05000016 / 0x05000017 | `cancelListener` / Verbinden (`requestScrrenIsLock`) | – | ✔ (0x05000017) |
| 0x07000000–0x07000003 | Tone Catch (Clone): Import/Export, löschen `u32 id`, umbenennen `u32 id, char name[16]`, Liste | | A |
| 0x08000000–0x08000003 | NAM: Upload / `deleteNam` `u32 id` / `renameNam` `u32 id, char name[16]` / Liste | | ✔ / A / A / ✔ |

Alle Codes sind im Web-Sniffer (`ampero/js/sniffer.js`, `KNOWN`) mit Namen und Decodern hinterlegt.

### 6.5 Globale Einstellungen des Stomp [Mitschnitt `captures/wine-global-settings-2026-11-17.txt` + Screenshots]

Jede Einstellung ist eine Adresse `0xGGGG00nn`. `get 0xGGGG0000` liefert **ein Byte pro Parameter, Parameter n
in Byte n−1** (dahinter Füllbytes). Setzen: `set` mit der Adresse als cmd und **einem Byte** als Payload (bei Pegeln
vorzeichenbehaftet). Das Gerät antwortet nicht. Zuordnung über die Reihenfolge der Bedienung im Mitschnitt und die
Seiten des Windows-Editors, Optionslisten aus dessen aufgeklappten Dropdowns (**?** = nicht bestätigt).
Die Gruppe 0x0007 hat im get ein eigenes Layout: [0] EXP/CTRL Function, [5] Global Tempo, [7] PATCH FS FUNC,
[8..9] `u16` Global-BPM (0x00070007).

| Seite | Adresse | Einstellung | Werte |
|---|---|---|---|
| Input/Output | 0x00010001 / 0x00010002 | Input Mode (L) / (R) | 0 Electric Guitar, 1 Acoustic Guitar, 2 Line |
| | 0x0001000e | Output Level | 0 Instrument, 1 Line |
| | 0x00010009 | Bypass Mode | 0 True Bypass, 1 DSP Bypass |
| | 0x0001000f / 0x00010008 | Output Source / FX Send Source | 0 Normal, 1 USB Only, 2 Dry |
| | 0x00010010 / 0x00010011 | No Cab Mode (L) / (R) | 0 OFF, 1 CAB Only, 2 IR Only, 3 CAB+IR |
| USB Audio | 0x00020001–0x00020004 | Rec Level 1/2, 3/4, 5/6, 7/8 | −61 (Mute) … +20 dB, `i8` |
| | 0x00020005 | Monitor Level | −61 (Mute) … +6 dB |
| | 0x00020009 / 0x00020007 | Output Source / FX Send Source | 0 USB Out OFF, 1 1/2, 2 3/4, 3 5/6, 4 7/8 |
| Controls | 0x00070005 | PATCH FS FUNC | `u32` 0x33 Patch, 0x34 Patch/FX Slot 1, 0x35 Patch/Scene 2, 0x36 Patch/Bypass (get: Byte 7) |
| | 0x00080004 | Bank Select Mode | 0 Initial, 1 Wait |
| | 0x00080001 | Auto Cab Match | 0/1 |
| | 0x00070001 | EXP/CTRL Function | 0 EXP 1/2, 1 Single FS, 2 Dual FS; set 4 Byte, Wert in Byte 2 (`00 00 01 00` = Single FS) [am Pedal bestätigt]; get: Byte 0 |
| | 0x00070002 | EXT FS1 bei EXP/CTRL = Single FS | Footswitch-Funktionscode, set `00 00 code 00`; get Byte 1 [Mitschnitt `captures/wine-ext-fs2-2026-11-18.txt` + get am Pedal] |
| | 0x00070003 | EXT FS1 / EXT FS2 bei EXP/CTRL = Dual FS | set `u8 schalter (0 FS1, 1 FS2), 00, code, 00`; get Byte 2 (FS1) / 3 (FS2). Single und Dual haben getrennte Werte. Der Hotone-Editor zeigt die Zeilen nur im jeweiligen Modus. Auswahl der App (`kFSTapList`): Looper-Funktionen, Tuner, Drum, Bank ±, Tap Tempo, Patch ±, Scene 1–5, EXP 1/2, A1–B6 |
| | 0x00070006 | Global Tempo | 0/1 |
| MIDI | 0x00060001 | MIDI In Source | 0 DIN Only, 1 USB Only, 2 Mixed |
| | 0x00060002 / 0x00060003 | Input Channel (DIN) / (USB) | 0–15 = Kanal 1–16, 16 = Omni |
| | 0x00060008 | Clock Source | 0 Internal, 1 DIN Only, 2 USB Only, 3 External, 4 Mixed [am Pedal bestätigt] |
| | 0x00060006 / 0x00060007 | Clock Out (DIN) / (USB) | 0/1 |
| Device Display | 0x00080002 | Patch Mode | 0 Mode 1, 1 Mode 2 |
| | 0x0008000d | Stomp Mode | 0 Footswitch, 1 Effect Chain |
| | 0x00080006 | Color | 0–6 (7 Farben) |
| | 0x00080007 | Display Time | 0 Always On, 1 1 Min, 2 5 Min, 3 10 Min, 4 20 Min, 5 30 Min, 6 40 Min, 7 50 Min, 8 60 Min |
| Drum / Metronom | 0x000a0001 Sync, 0x000a0002 Volume (0–100), 0x000a0003 BPM (40–300, set `u16`), 0x000a0004 Rhythmus (0–99), 0x000a0005 Play | set je 1 Byte (BPM 2 Byte, Dart `setGlobalControlValue(…, lenght: 2)`); get `0x000a0000` = [0] Sync, [1] Volume, [2..3] u16 BPM, [4] Rhythmus, [5] Play, [6..7] ? (am Pedal: `00 26 78 00 05 00 01 81` = Sync aus, 38, 120 BPM, „6. Metro 6/4“). Namen: Dart-Liste `drumName` (100 Einträge, „Metro 1/4“ … „11/8 Beat“). Set noch nicht mitgeschnitten |

**Footswitch-Funktionscodes** (Preset Tag 3 `click`/`hold`, CTRL-Schalter, PATCH FS FUNC; aus `HTBaseDefine.kFSMapFunc`
der App, verifiziert an PATCH FS FUNC und den Presets):

| Code | Funktion | Code | Funktion |
|---|---|---|---|
| 0xFF | Off | 0–11 | A1–A6, B1–B6 On/Off |
| 12 | Ctrl 1 (schaltet die zugewiesenen Slots, Tag 8) | 13 | Tap Tempo |
| 15 / 16 | Bank + / Bank − | 17 | Looper |
| 18 | Tuner | 19 | Bypass |
| 20 | Mode Change | 21 | Ctrl Temp Change |
| 22–26 | Patch 1–5 | 27–31 | Scene 1–5 |
| 32–37 | Ctrl 2–7 | 38 / 39 | Patch + / Patch − |
| 40 | Bank Select (in der App „UI Open“; Name laut Stomp) | 41 | EXP 1/2 |
| 42–45 | Looper Rec/Play, Stop, 1/2 SPD, Rev | 46–49 | Loop FX, Looper Undo/Redo, Drum Play/Stop, Looper Exit |
| 50 | Drum | 51–54 | Patch, Patch/FX Slot 1, Patch/Scene 2, Patch/Bypass |

**Global EQ (0x0003):** `get` liefert `f32[37]`: [0] EQ an/aus, dann 8 Bänder (Low Cut: an, Freq, Q; Low Shelf,
Band 1–4, High Shelf: an, Freq, Q, Gain; High Cut: an, Freq, Q), [31] Lautstärke (50). Setzen: `0x00030001 u32` an/aus,
`0x00030002 u32 index, f32 wert` mit *index = Position in dieser Liste − 1* (verifiziert: 8 = Band 1 Freq,
10 = Band 1 Gain, 16 = Band 3 Freq), `0x00030004 u32 0` = Reset.

Der Web-Editor (Seite *Global Settings*, `js/globals.js`) liest und setzt alle Einstellungen dieser Tabelle.

### 6.6 Szenen 4 und 5 beim Stomp

Die Footswitch-Funktionen „Scene 4/5“ (Code 30/31) schalten auch den Stomp in Szene 4/5, obwohl seine Presets nur
3 Szenen enthalten. **Jede Bearbeitung in Szene 4/5** (z. B. `0x01040003`, `0x01090004` mit Szene 3) lässt die Firmware
V2.5.0 mit „Record the error and restart – error code: Scene < SCENE_NUM, PresetInterface.c, Zeile 4196“ stehen
(Neustart nötig). Der Web-Editor sperrt deshalb alle Änderungen, solange das Gerät in Szene 4/5 steht.

### 6.7 MIDI-Nachrichten im Preset (Tag 9) [verifiziert am Preset `data/A98-1-MIDI-Test.raw`]

Tag 9 = 15 Nachrichten zu je 3 Bytes: `u8 channel` (0xFF = OFF, 0..15 = Kanal 1..16), `u8 command` (0..127 = CC n,
128 = PC), `u8 data` (0..127).

| Index | Bedeutung |
|---|---|
| 0..8 | FS-MIDI: FS n (0..2), Zeile r (0..2) bei `3n + r` |
| 9..14 | Patch-MIDI, Zeile 1..6 (wird beim Laden des Presets gesendet) |

Testpreset (im Hotone-Editor gesetzt): FS1/1 = `01 07 64` (Kanal 2, CC 7, 100), FS3/3 = `0f 80 40` (Kanal 16, PC,
Data 64 – vorgegeben war 5, offenbar anders eingetragen), Patch/1 = `02 0b 40` (Kanal 3, CC 11, 64).

Setzen (aus der Hotone-App, `HTModelPreset.setFSMidiParam` / `setPatchMidiParam`, noch nicht im Mitschnitt gesehen):
`0x010B0001` u32 fs, u32 id, char[3] und `0x010B0002` u32 id, char[3]; id = Zeile innerhalb des FS bzw. der Patch-MIDI.

### 6.8 Ein- und Ausgangsknoten [verifiziert am Mitschnitt `captures/wine-io-nodes-2026-11-18.txt` + Screenshots]

Tag 6: `i16 inputSource[2]` @0, `i16 outputSource[2]` @4, `f32 inputSourceParam[2][7][4]` @8,
`f32 outputSourceParam[2][7][4]` @232 – Parameter je Kette (A/B) **und je Quelle** (Index = Quellcode).

| | Codes | Parameter |
|---|---|---|
| Eingang | 0 Input, 1 FX Return, 2 USB Out 3/4, 3 USB Out 5/6, 4 USB Out 7/8, 5 Chain A, 6 None | 0 Kanal (0 L, 1 R, 2 Sum, 3 Stereo), 1 Level dB |
| Ausgang | 0 Output (All Outputs), 1 FX Send, 2 Chain B, 3 None, 4 Unbalanced, 5 Balanced & Phones | 0 Level dB, 1 Pan (−50 L … +50 R) |

Level −61 = Mute … +20 dB. Stomp-Editor bietet: A-Eingang Input/FX Return/USB Out 3/4, B zusätzlich Chain A/None;
A-Ausgang Output/FX Send/Chain B, B Output/FX Send/None. Setzen: `0x01050001`/`0x01050002` `i16 chain, i16 code`
(das Pedal antwortet mit dem Routing-Push `0x0105000b`; B-Eingang = Chain A stellt A-Ausgang auf Chain B);
`0x01050003`/`0x01050004` `i16 chain, i16 param, f32 wert` (für die aktuelle Quelle).

### 6.9 Expression-Ziele [verifiziert am Mitschnitt `captures/wine-exp-targets-2026-11-18.txt` + Screenshots `data/exp1..7.png`]

Patch Settings → EXP1/EXP2 Settings: je Pedal 4 Ziele („Target 1–4“). Ein Ziel ist OFF, ein Block-Parameter
(„Slot FX“) oder eine Funktion (Patch Volume, Patch Tempo).

**Ziel setzen** `0x01060002` (9 Byte): `u8 exp` (0 EXP1, 1 EXP2), `u8 target` (0–3), `u8 slot`, `u8 category`,
`u32 alg`, `u8 param` – derselbe Aufbau wie `setAlgPayload` / Quick-Knob-Ziel.
* Block: slot = **Kettenposition** 0–11 (A1–B6; Blue Sustainer = Slot 0 auf A2 → 1, bestätigt am Export), category = Modul-Index (DYN = 0), alg = Algorithmuscode
  (Blue Sustainer = 2), param = Parameterindex des Algorithmus (Sustain 0, Attack 1, Output 2).
* Funktion: slot = `0x0C` Patch Volume, `0x0D` Patch Tempo; category, alg, param = `FF`.
* OFF: nicht im Mitschnitt (vermutlich slot = `FF`, Rest `FF`).

**Bereich setzen** `0x01060003` (24 Byte): `u16 exp`, `u16 target`, `f32 min`, `f32 max`, `f32 curve`
(0 Line, 1 Exp, 2 Log), `f32 autoOff` (0/1), `f32 offTime` (ms, Default 600). Min/Max in Anzeigewerten des Ziels
(Blockparameter 0–100, Patch Tempo 40–300 BPM, Patch Volume 0–100); min > max ist erlaubt (umgekehrte Richtung,
Screenshot exp7: 190 → 100). Der Editor schickt bei jeder Änderung alle Felder. Auto-OFF (+ OFF Time) bietet
der Editor nur für Block-Ziele an. Beim Wechsel des Ziels schickt der Editor keinen Bereich, die Werte setzt das
Pedal selbst (Patch Tempo zeigt danach 40–300).

Das Pedal antwortet nicht. Mit Auto-OFF schaltet es den Block selbst ein/aus und meldet das mit `0x01040002`
`00 00 02 00 00 00 01` / `…00` (u8 slot 0, category 0, alg 2, on) – dort der Slot, im Ziel dagegen die
Kettenposition (wie bei der FS-Zuordnung in Tag 8). Nach dem ersten Setzen kam `0x03000003` (Preset geändert).

Im Preset (Tag 7, 178 Byte; [verifiziert am Export `data/A98-1-MIDI-Test.prst` nach dem Mitschnitt]):
`u8 expState[2]` (`01 01`), `u8 pos[8]` (Kettenposition bzw. `0C`/`0D`, `FF` = OFF/unbenutzt), `u8 param[8]`
(`FF` bei Funktionen), `f32 min[8]`, `f32 max[8]`, `f32 curve[8]`, `f32 autoOff[8]`, `f32 offTime[8]`.
Index = `exp * 4 + target`. Kategorie und Algorithmus stehen nicht im Preset, sie folgen aus der Position.
Beim Wechsel des Parameters setzt das Pedal den Bereich auf 0–100 zurück (Ziel 1 im Export: Sustain 0–100).
OFF (Export `data/A98-1-MIDI-Test 1.prst`): pos = `FF`, param = `FF`, Bereich 0–100, Line, Auto-OFF aus.

### 6.10 Split- und Mix-Knoten verschieben [verifiziert am Mitschnitt `captures/wine-split-mix-nodes-2026-11-18.txt` + Screenshots]

Der Hotone-Editor schickt beim Verschieben immer beide Positionen nacheinander: `0x01050005` `u32 split`, dann
`0x01050006` `u32 mix` (0–6 = vor Position k der oberen Reihe, 6 = Ende; `FFFFFFFF` = kein Knoten). Das Pedal
antwortet mit dem Routing-Push `0x0105000b` (`i16 inputSource[2], i16 outputSource[2], u8 split, u8 mix, 2 Byte`)
und passt dabei die Ein-/Ausgangsquellen selbst an.

| Vorlage | gesehen (split, mix) |
|---|---|
| Split → Mix | (0,6) (2,6) (2,4) (2,3) (3,4) (3,5) (4,5) (0,3) – immer split < mix, auch direkt benachbart |
| Y → A/B | (3,FF) (4,FF) |
| Serial | (3,FF) → Pedal macht daraus einen Y-Split (in1 = None, out1 = None); (6,FF) → wieder Serial |
| A/B → Y | nicht verschoben |

Im selben Mitschnitt: `0x01050007` `u32 mode` (Split-Modus), `0x01050009` `u32 index, f32 wert` (Split-Parameter:
Level To A/B …), `0x0105000a` `u32 index, f32 wert` (Mixer: A Level, A Pan, B Level, B Pan, B Polarity, Master,
Master SUM).

## 7. Offene Punkte

* Tag 8 (`ctrlTargetSlot`) am Gerät bestätigen: dazu nach einer Footswitch-Änderung das Preset
  erneut abrufen (im aktuellen Mitschnitt fehlt der Dump *nach* der Änderung).
* Bedeutung der FS-Funktionscodes (0x0C, 0x0D, 0x1B, 0x1C, 0x29 …) – am einfachsten im Editor jede
  Funktion einmal setzen und mitschneiden. Die Code-Tabelle steht vermutlich in `HTModelPresetFS`
  bzw. den FS-Views des Dart-Codes.
* Prüfsumme der Geräteantworten (nur in der Firmware zu finden, siehe 6).
* Tag 0 (`Setting`), Tag 9 (`midiData`) und der Rest von Tag 5 (`empty`/`vol`/`bpm`/`sta`/`para`)
  sind benannt, aber noch nicht byteweise belegt. Die Parameter-Layouts von Tag 6 (`inputSourceParam`)
  sind ebenfalls offen.
  Mit gezielten Test-Exports (nur eine Einstellung ändern, z. B. Footswitch-Zuweisung, MIDI-CC, Split)
  lässt sich jedes Feld per Diff zuordnen – das Tool behält unbekannte Bytes als Hex und baut die Datei
  wieder exakt zusammen.
* Die Umrechnung Werkspreset (8700) ↔ Editor (5004) steckt im Dart-Code (`app.so`, Klassen
  `HTModelPreset{Info,Slot,Scene,FS,Ctrl,Exp2,QKnob,Midi}`) bzw. in der ARM-Firmware. In der
  Android-Dekompilierung (`android/blutter_out/asm/mp350/model/HTModelPreset.dart`) sind die Leser
  `readInfoData`, `readKnobData`, `readFSData`, `readEffectData`, `readSceneData`, `readTempData`,
  `readExpData`, `readCtrlData`, `readMidiData` und `readDefineData` die nächsten Anlaufstellen.
