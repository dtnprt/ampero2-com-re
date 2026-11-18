using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace Ampero.Protocol;

/// <summary>Layers of an editor preset export.</summary>
public enum PresetFileFormat { Prst, Arr, Bin, Raw, SysExDump }

/// <summary>
/// Editor preset files: <c>.prst = base64(.arr)</c>, <c>.arr = "[b0,b1,…]"</c>,
/// <c>.bin = LZO1X-1(raw)</c>, raw = TLV preset.
/// </summary>
public static class PresetFile
{
    public static AmperoPreset Load(string path) => Decode(File.ReadAllBytes(path), out _);

    public static AmperoPreset Decode(byte[] data, out PresetFileFormat format)
    {
        var text = data.Length > 0 && data.All(b => b < 0x80) ? Encoding.ASCII.GetString(data).Trim() : null;
        if (text != null && text.StartsWith("["))
        {
            format = PresetFileFormat.Arr;
            return AmperoPreset.Parse(Lzo1x.Decompress(ParseArray(text)));
        }
        if (text != null && text.Length > 0 && text.All(c => char.IsLetterOrDigit(c) || c is '+' or '/' or '=' or '\r' or '\n'))
        {
            var arr = Encoding.ASCII.GetString(Convert.FromBase64String(text)).Trim();
            if (arr.StartsWith("["))
            {
                format = PresetFileFormat.Prst;
                return AmperoPreset.Parse(Lzo1x.Decompress(ParseArray(arr)));
            }
        }
        if (AmperoPreset.IsTlv(data))
        {
            format = PresetFileFormat.Raw;
            return AmperoPreset.Parse(data);
        }
        if (data.Length > 4 && AmperoPreset.IsTlv(data.AsSpan(4)))
        {
            format = PresetFileFormat.SysExDump;
            return AmperoPreset.FromDump(data);
        }
        format = PresetFileFormat.Bin;
        return AmperoPreset.Parse(Lzo1x.Decompress(data));
    }

    public static byte[] Encode(AmperoPreset preset, PresetFileFormat format)
    {
        var raw = preset.ToBytes();
        if (format == PresetFileFormat.Raw) return raw;
        var bin = Lzo1x.Compress(raw);
        if (format == PresetFileFormat.Bin) return bin;
        var arr = Encoding.ASCII.GetBytes("[" + string.Join(",", bin) + "]");
        return format == PresetFileFormat.Arr ? arr : Encoding.ASCII.GetBytes(Convert.ToBase64String(arr));
    }

    /// <summary>Saves the preset; the format follows the file extension (.prst/.arr/.bin/.raw).</summary>
    public static void Save(AmperoPreset preset, string path)
    {
        var format = Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".arr" => PresetFileFormat.Arr,
            ".bin" => PresetFileFormat.Bin,
            ".raw" => PresetFileFormat.Raw,
            _ => PresetFileFormat.Prst,
        };
        File.WriteAllBytes(path, Encode(preset, format));
    }

    static byte[] ParseArray(string json) =>
        JsonSerializer.Deserialize<int[]>(json)!.Select(v => (byte)v).ToArray();
}
