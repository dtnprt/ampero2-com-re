using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Ampero.Protocol;

/// <summary>One TLV block of a preset (<c>u32 tag, u32 len, data</c>).</summary>
public sealed class PresetBlock
{
    public PresetBlock(uint tag, byte[] data) { Tag = tag; Data = data; }
    public uint Tag { get; }
    public byte[] Data { get; }
}

/// <summary>
/// Ampero II preset: a TLV chain with tags 0..9. Editor files hold 5004 bytes, device dumps
/// 5068 bytes (tag 7 is longer on the device). All accessors read and write the block data
/// directly, unknown bytes are preserved.
/// </summary>
public sealed class AmperoPreset
{
    public const int SceneCount = 3, SlotCount = 12, ParamsPerSlot = 25;
    public const uint EmptyAlgorithm = 0xFFFFFFFF;

    public const uint TagInfo = 1, TagFootswitch = 3, TagSlots = 4, TagScenes = 5;

    // tag 5 layout
    const int SceneParamBytes = SceneCount * SlotCount * ParamsPerSlot * 4;   // 3600
    const int SceneEnabledOffset = SceneParamBytes;                           // u8[3][12]
    const int SceneVolumeOffset = SceneEnabledOffset + SceneCount * SlotCount; // u16[3]
    const int SceneTempoOffset = SceneVolumeOffset + 6;                         // u16[3]
    const int SceneNameLength = 8;                                             // char[3][8] at the end

    readonly List<PresetBlock> _blocks;

    AmperoPreset(List<PresetBlock> blocks) => _blocks = blocks;

    public IReadOnlyList<PresetBlock> Blocks => _blocks;

    /// <summary>Preset number when the preset came from the device (dump prefix), otherwise -1.</summary>
    public int Index { get; set; } = -1;

    public static bool IsTlv(ReadOnlySpan<byte> raw)
    {
        int o = 0;
        while (o + 8 <= raw.Length)
        {
            uint tag = BinaryPrimitives.ReadUInt32LittleEndian(raw.Slice(o));
            uint len = BinaryPrimitives.ReadUInt32LittleEndian(raw.Slice(o + 4));
            if (tag > 0xFF || len > (uint)raw.Length) return false;
            o += 8 + (int)len;
        }
        return o == raw.Length && raw.Length > 0;
    }

    /// <summary>Parses a raw TLV preset.</summary>
    public static AmperoPreset Parse(ReadOnlySpan<byte> raw)
    {
        if (!IsTlv(raw)) throw new InvalidDataException("not an Ampero TLV preset");
        var blocks = new List<PresetBlock>();
        int o = 0;
        while (o < raw.Length)
        {
            uint tag = BinaryPrimitives.ReadUInt32LittleEndian(raw.Slice(o));
            int len = (int)BinaryPrimitives.ReadUInt32LittleEndian(raw.Slice(o + 4));
            blocks.Add(new PresetBlock(tag, raw.Slice(o + 8, len).ToArray()));
            o += 8 + len;
        }
        return new AmperoPreset(blocks);
    }

    /// <summary>Parses the payload of a <see cref="AmperoCommand.PresetDump"/> reply (u32 index + TLV).</summary>
    public static AmperoPreset FromDump(ReadOnlySpan<byte> payload)
    {
        var p = Parse(payload.Slice(4));
        p.Index = (int)BinaryPrimitives.ReadUInt32LittleEndian(payload);
        return p;
    }

    public byte[] ToBytes()
    {
        var ms = new MemoryStream();
        Span<byte> h = stackalloc byte[8];
        foreach (var b in _blocks)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(h, b.Tag);
            BinaryPrimitives.WriteUInt32LittleEndian(h.Slice(4), (uint)b.Data.Length);
            ms.Write(h);
            ms.Write(b.Data);
        }
        return ms.ToArray();
    }

    public byte[]? Block(uint tag) => _blocks.FirstOrDefault(b => b.Tag == tag)?.Data;

    byte[] Required(uint tag, int minLength)
    {
        var d = Block(tag) ?? throw new InvalidDataException($"tag {tag} missing");
        if (d.Length < minLength) throw new InvalidDataException($"tag {tag} too short ({d.Length} B)");
        return d;
    }

    // ------------------------------------------------------------------ info (tag 1)

    public uint Version => BinaryPrimitives.ReadUInt32LittleEndian(Required(TagInfo, 101));

    /// <summary>Preset name, max. 16 ASCII characters.</summary>
    public string Name
    {
        get => ReadString(Required(TagInfo, 101), 4, 17);
        set => WriteString(Required(TagInfo, 101), 4, 17, value);
    }

    /// <summary>Style / category, e.g. "Acoustic" (max. 9 characters).</summary>
    public string Style
    {
        get => ReadString(Required(TagInfo, 101), 91, 10);
        set => WriteString(Required(TagInfo, 101), 91, 10, value);
    }

    // ------------------------------------------------------------------ slots (tag 4)

    /// <summary>Chain position order (12 bytes).</summary>
    public byte[] SlotOrder => Required(TagSlots, 72).AsSpan(0, 12).ToArray();

    public uint GetAlgorithm(int slot) =>
        BinaryPrimitives.ReadUInt32LittleEndian(Required(TagSlots, 72).AsSpan(24 + 4 * CheckSlot(slot)));

    public void SetAlgorithm(int slot, uint code) =>
        BinaryPrimitives.WriteUInt32LittleEndian(Required(TagSlots, 72).AsSpan(24 + 4 * CheckSlot(slot)), code);

    // ------------------------------------------------------------------ scenes (tag 5)

    byte[] Scenes => Required(TagScenes, SceneTempoOffset + 6 + SceneCount * SceneNameLength);

    public string GetSceneName(int scene)
    {
        var d = Scenes;
        return ReadString(d, d.Length - SceneCount * SceneNameLength + SceneNameLength * CheckScene(scene), SceneNameLength);
    }

    public void SetSceneName(int scene, string name)
    {
        var d = Scenes;
        WriteString(d, d.Length - SceneCount * SceneNameLength + SceneNameLength * CheckScene(scene), SceneNameLength, name);
    }

    public int GetSceneVolume(int scene) => BinaryPrimitives.ReadUInt16LittleEndian(Scenes.AsSpan(SceneVolumeOffset + 2 * CheckScene(scene)));
    public void SetSceneVolume(int scene, int v) => BinaryPrimitives.WriteUInt16LittleEndian(Scenes.AsSpan(SceneVolumeOffset + 2 * CheckScene(scene)), (ushort)v);
    public int GetSceneTempo(int scene) => BinaryPrimitives.ReadUInt16LittleEndian(Scenes.AsSpan(SceneTempoOffset + 2 * CheckScene(scene)));
    public void SetSceneTempo(int scene, int v) => BinaryPrimitives.WriteUInt16LittleEndian(Scenes.AsSpan(SceneTempoOffset + 2 * CheckScene(scene)), (ushort)v);

    public bool IsSlotEnabled(int scene, int slot) => Scenes[SceneEnabledOffset + SlotCount * CheckScene(scene) + CheckSlot(slot)] != 0;
    public void SetSlotEnabled(int scene, int slot, bool on) => Scenes[SceneEnabledOffset + SlotCount * CheckScene(scene) + CheckSlot(slot)] = (byte)(on ? 1 : 0);

    /// <summary>Parameter value (display value, e.g. 50 = 50 %). Index = widget ID of the algorithm database.</summary>
    public float GetParameter(int scene, int slot, int index) =>
        BinaryPrimitives.ReadSingleLittleEndian(Scenes.AsSpan(ParamOffset(scene, slot, index)));

    public void SetParameter(int scene, int slot, int index, float value) =>
        BinaryPrimitives.WriteSingleLittleEndian(Scenes.AsSpan(ParamOffset(scene, slot, index)), value);

    static int ParamOffset(int scene, int slot, int index)
    {
        if ((uint)index >= ParamsPerSlot) throw new ArgumentOutOfRangeException(nameof(index));
        return ((CheckScene(scene) * SlotCount + CheckSlot(slot)) * ParamsPerSlot + index) * 4;
    }

    // ------------------------------------------------------------------ footswitches (tag 3)

    /// <summary>Function codes of FS1..FS3 (0x20/0x21 = toggles effect slots; others not yet named).</summary>
    public byte[] FootswitchFunctions => Required(TagFootswitch, 3).AsSpan(0, 3).ToArray();

    public void SetFootswitchFunction(int fs, byte function)
    {
        if ((uint)fs > 2) throw new ArgumentOutOfRangeException(nameof(fs));
        Required(TagFootswitch, 3)[fs] = function;
    }

    // ------------------------------------------------------------------ helpers

    static int CheckScene(int s) => (uint)s < SceneCount ? s : throw new ArgumentOutOfRangeException(nameof(s));
    static int CheckSlot(int s) => (uint)s < SlotCount ? s : throw new ArgumentOutOfRangeException(nameof(s));

    internal static string ReadString(byte[] d, int offset, int length)
    {
        int n = Array.IndexOf(d, (byte)0, offset, length);
        if (n < 0) n = offset + length;
        return Encoding.ASCII.GetString(d, offset, n - offset);
    }

    static void WriteString(byte[] d, int offset, int length, string value)
    {
        var bytes = Encoding.ASCII.GetBytes(value ?? "");
        if (bytes.Length > length - 1) throw new ArgumentException($"max. {length - 1} characters");
        Array.Clear(d, offset, length);
        bytes.CopyTo(d, offset);
    }

    public override string ToString() => Index >= 0 ? $"#{Index:000} {Name}" : Name;
}
