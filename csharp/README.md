# Ampero II – C#-Bibliotheken und „Ampero II Remote“

| Projekt | Ziel | Inhalt |
|---|---|---|
| `src/Ampero.Protocol` | net8.0, ohne Abhängigkeiten | LZO1X (byte-identisch zum Editor), SysEx-Codec/Reassembler, Preset-Modell (TLV), `.prst/.arr/.bin`, Algorithmus-DB, Presetliste |
| `src/Ampero.Midi` | net8.0 + DryWetMIDI 8 | `AmperoDevice`: Verbindung über USB-MIDI, Requests mit Antwort, Events für Szenen-/Presetwechsel |
| `src/Ampero.Gui` | net8.0-windows (WPF) | Programm **AmperoRemote.exe** |
| `tests/Ampero.Protocol.Tests` | xUnit | 17 Tests gegen `wireshark.pcapng` und die Preset-Dateien |

```
dotnet build Ampero.sln
dotnet test
dotnet run --project src/Ampero.Gui
```

## AmperoRemote (GUI)

* MIDI-In/Out werden automatisch gewählt, wenn der Portname „Ampero“ oder „MP-300“ enthält.
* **Der Hotone-Editor muss geschlossen sein** – Windows-MIDI-Ports sind exklusiv.
* Links: Presetliste (300 Einträge, Filter nach Name/Nummer); Doppelklick wählt das Preset auf dem Gerät.
* Mitte: Presetname groß, drei Szenen-Buttons (aktive Szene orange, Klick wechselt die Szene),
  darunter die Effekt-Slots der aktiven Szene mit Parameternamen.
* Rechts: Footswitch-Funktionen FS1–FS3.
* „Gerät abfragen (1 s)“ fragt Preset und Szene zyklisch ab, damit Änderungen am Gerät erscheinen.
* *Datei → Preset-Datei öffnen* zeigt `.prst/.arr/.bin/.raw` offline an; *speichern* als `.raw` (TLV) oder `.prst`.
  Hinweis: Ein vom Gerät geladenes Preset hat 5068 Byte (Block 7 ist länger als in Editor-Dateien mit 5004 Byte);
  ob der Hotone-Editor solche `.prst` importiert, ist nicht getestet.
* Effektnamen kommen aus `v1.0.9_alg_data.json` des Editors; die Datei wird beim Build neben die Exe kopiert,
  wenn sie unter `..\..\private\windows\windows software\data\flutter_assets\assets\data\` liegt (private Arbeitskopie;
  nicht Teil dieses Repos – sie stammt aus der Hotone-Software)
  (sonst *Datei → Algorithmus-DB laden…*).

## Bibliothek – Beispiel

```csharp
using Ampero.Midi;
using Ampero.Protocol;

var (inName, outName) = AmperoDevice.FindPorts();
using var amp = AmperoDevice.Open(inName!, outName!);
amp.SceneChanged += s => Console.WriteLine($"Szene am Gerät: {s + 1}");

amp.Connect();
int index = await amp.GetCurrentPresetAsync();
AmperoPreset preset = await amp.GetPresetAsync(index);
Console.WriteLine($"{preset}  Szenen: {preset.GetSceneName(0)} | {preset.GetSceneName(1)} | {preset.GetSceneName(2)}");

amp.SelectScene(2);            // Szene 3
amp.SelectPreset(8);           // Preset #008

// Dateien
var p = PresetFile.Load("A75-2-ABCTESTHIJKLMNOP.prst");
p.SetSceneName(0, "Clean");
PresetFile.Save(p, "neu.prst");

// reines Protokoll (z. B. für eigene MIDI-Schichten)
foreach (byte[] sysex in AmperoSysEx.Encode(AmperoCommand.CurrentScene, new byte[] { 1 }))
    Console.WriteLine(Convert.ToHexString(sysex));
```

## Was getestet ist

* Protokoll und Dateiformate: 17 Unit-Tests, u. a. alle 50 Host-Nachrichten des Mitschnitts werden
  byte-identisch erzeugt, alle Geräte-Antworten bis auf die (anders berechnete) Prüfsumme;
  die vier Preset-Dumps und die Presetliste werden korrekt gelesen; `.prst`/`.bin` aller Beispiel-Presets
  werden byte-identisch neu geschrieben.
* GUI: baut und startet; **die Verbindung zum echten Gerät ist nicht getestet** (kein Ampero hier).
  Abläufe und Befehle entsprechen dem Mitschnitt des Hotone-Editors.
* Footswitch-Schreibbefehle (`SetFootswitchFunctions`, `SetFootswitchSlots`) sind in der Bibliothek
  vorhanden, in der GUI bewusst nur lesend, solange die Speicherung in Block 8 nicht bestätigt ist.
