using System;
using System.Collections.Generic;

namespace Ampero.Protocol;

public sealed record PresetListEntry(int Index, string Name)
{
    public override string ToString() => $"#{Index:000}  {Name}";
}

/// <summary>
/// Payload of <see cref="AmperoCommand.PresetList"/> (29700 bytes): a u16 table of 300 entries
/// (meaning unclear, contains values &gt; 300), then <c>char name[300][17]</c> indexed by preset
/// number (verified: #6 = "Juicy Fusion", #297 = "Le Chug"), then 24000 unknown bytes.
/// </summary>
public static class PresetList
{
    public const int Count = 300, NameLength = 17, NamesOffset = 2 * Count;

    public static IReadOnlyList<PresetListEntry> Parse(byte[] payload)
    {
        int count = Math.Min(Count, Math.Max(0, payload.Length - NamesOffset) / NameLength);
        var list = new List<PresetListEntry>(count);
        for (int i = 0; i < count; i++)
            list.Add(new PresetListEntry(i, AmperoPreset.ReadString(payload, NamesOffset + NameLength * i, NameLength)));
        return list;
    }
}
