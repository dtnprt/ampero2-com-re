# Ampero II – communication & format reverse engineering

Unofficial documentation and tools for talking to the **Hotone Ampero II Stomp** (MP-350) over USB-MIDI,
and for reading and writing its preset, firmware and file formats – for interoperability with your own
software and hardware. Not affiliated with Hotone.

The browser-based editor built on this work lives in a separate repository:
[dtnprt.github.io/ampero](https://github.com/dtnprt/dtnprt.github.io/tree/main/ampero).

## Contents

| Path | What |
|---|---|
| `tools/FORMAT.md` | The main document (German): SysEx framing, LZO payloads, command catalog, preset TLV layout, global settings, footswitches, quick knobs, expression pedal, I/O nodes, MIDI messages, firmware container, IR / NAM upload |
| `tools/ampero_sysex.py` | SysEx decoder / encoder (pcapng, trace text files, raw dumps) |
| `tools/ampero_preset.py` | Preset tool: `.prst` / `.arr` / `.bin` / raw TLV ↔ JSON |
| `tools/ampero_fw.py` | Firmware container unpacker / repacker |
| `tools/usbmon_capture.py` | Capture the pedal's USB-MIDI traffic on Linux (usbmon) as a text trace |
| `tools/ampero_ping.sh` | Does the pedal still answer over USB-MIDI? |
| `tools/lzo1x.py` | LZO1X-1 compressor / decompressor (byte-identical to the pedal's software) |
| `tools/nam2namb.py` | Neural Amp Modeler `.nam` → `.namb` converter (port of TONE3000's converter) |
| `tools/gen_*.py`, `tools/extract_valtostr.py` | Generators for effect name / parameter tables – they read data from **your own** Hotone software installation |
| `arduino/` | Stand-alone displays / controllers (ESP32-S3, RP2040, Pico 2) that act as USB host for the pedal |
| `csharp/` | .NET libraries (protocol, presets, MIDI device) and a small WPF remote, with tests |
| `presets/` | Test presets created for this project (used by the tests) |

## Not included – on purpose

Firmware images, the Hotone editor and its files, factory presets and IRs, captures and screenshots of
Hotone's software are copyrighted by Hotone (or third parties) and are **not** part of this repository.
The documentation refers to some of these files by name; get them from Hotone yourself.

Some tools and sketches need tables generated from the Hotone editor's data:

* `arduino/AmperoDisplayPico2/AlgNames.h`, `arduino/AmperoDisplayS3/AlgNames.h` (effect names):
  `python3 tools/gen_alg_names.py <editor data dir> arduino/AmperoDisplayPico2/AlgNames.h`
* `tools/data/valtostr.json` (value display tables): `tools/extract_valtostr.py`
* effect names in `tools/ampero_preset.py`: `--alg-db <v*_alg_data.json>` or `AMPERO_ALG_DB=…`

These generated files stay on your machine (they are in `.gitignore`).

## Safety

The Ampero II Stomp can freeze on values it does not expect (e.g. scenes 4/5, Ctrl 5/6, block changes
outside scene 1). Only send what the official editor for your model sends; keep a backup of your presets.
Use at your own risk.

## License

MIT for the original code and documentation – see [LICENSE](LICENSE), including the third-party parts
and the exclusions. "Hotone" and "Ampero" are trademarks of Changsha Hotone Audio Co., Ltd.
