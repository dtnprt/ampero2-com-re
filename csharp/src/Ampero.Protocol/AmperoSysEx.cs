using System;
using System.Buffers.Binary;
using System.Collections.Generic;

namespace Ampero.Protocol;

/// <summary>Message kinds (byte 8 of a frame).</summary>
public static class AmperoKind
{
    public const byte Request = 0x11;
    public const byte Set = 0x12;   // also used for all device replies / notifications
}

/// <summary>Known command codes (u32 at the start of every transfer).</summary>
public static class AmperoCommand
{
    public const uint Connect = 0x05000017;
    public const uint CurrentPreset = 0x00090000;    // u32 preset index (request / select)
    public const uint PresetDump = 0x01000000;       // req u32 index -> u32 index + TLV preset
    public const uint PresetList = 0x01080000;       // u16 order[300], char name[300][17], ...
    public const uint CurrentScene = 0x03000001;     // set: u8 scene; reply/notification: u32
    public const uint Status = 0x03000003;
    public const uint FirmwareVersion = 0x03000005;
    public const uint UserIrNames = 0x04000003;
    public const uint FootswitchBlock = 0x01030000;  // = preset TLV tag 3
    public const uint FootswitchFunctionsSet = 0x01030001;  // u8 fs_func[3]
    public const uint FootswitchSlotsSet = 0x01070001;      // u8 fs, u8 slot_on[12]

    static readonly Dictionary<uint, string> Names = new()
    {
        [Connect] = "Connect",
        [CurrentPreset] = "CurrentPreset",
        [PresetDump] = "PresetDump",
        [PresetList] = "PresetList",
        [CurrentScene] = "CurrentScene",
        [Status] = "Status",
        [FirmwareVersion] = "FirmwareVersion",
        [UserIrNames] = "UserIrNames",
        [FootswitchBlock] = "FootswitchBlock",
        [FootswitchFunctionsSet] = "FootswitchFunctionsSet",
        [FootswitchSlotsSet] = "FootswitchSlotsSet",
    };

    public static string NameOf(uint command) =>
        Names.TryGetValue(command, out var n) ? n : $"0x{command:X8}";
}

/// <summary>One decoded SysEx frame.</summary>
public readonly record struct AmperoFrame(byte Checksum, byte Kind, int Total, int Offset, byte[] Data, bool ChecksumOk);

/// <summary>A complete transfer: command plus decompressed payload.</summary>
public sealed record AmperoMessage(uint Command, byte Kind, byte[] Payload, bool ChecksumOk)
{
    public override string ToString() =>
        $"{AmperoCommand.NameOf(Command)} kind=0x{Kind:X2} payload={Payload.Length} B";

    /// <summary>First 4 payload bytes as little-endian u32 (0 if shorter).</summary>
    public uint PayloadU32 => Payload.Length >= 4 ? BinaryPrimitives.ReadUInt32LittleEndian(Payload) : Payload.Length > 0 ? Payload[0] : 0u;
}

/// <summary>
/// Framing of the Ampero II USB-MIDI protocol:
/// <code>F0 21 25 4D 50 00 00 | cs | kind | totLo totHi | offLo offHi | nibbles ... | F7</code>
/// The concatenated chunk data is <c>u32 cmd, u32 len, LZO1X-1(payload)</c>.
/// </summary>
public static class AmperoSysEx
{
    public static readonly byte[] Header = { 0xF0, 0x21, 0x25, 0x4D, 0x50, 0x00, 0x00 };
    public const int MaxChunk = 185;   // bytes per frame -> 384-byte SysEx

    /// <summary>Builds the complete SysEx frames (including F0 and F7) exactly like the editor.</summary>
    public static IReadOnlyList<byte[]> Encode(uint command, ReadOnlySpan<byte> payload, byte kind = AmperoKind.Set)
    {
        byte[] lzo = payload.IsEmpty ? Array.Empty<byte>() : Lzo1x.Compress(payload);
        var stream = new byte[8 + lzo.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(stream, command);
        BinaryPrimitives.WriteUInt32LittleEndian(stream.AsSpan(4), (uint)lzo.Length);
        lzo.CopyTo(stream, 8);

        var frames = new List<byte[]>();
        for (int off = 0; off < stream.Length; off += MaxChunk)
        {
            int n = Math.Min(MaxChunk, stream.Length - off);
            var f = new byte[13 + 2 * n + 1];
            Header.CopyTo(f, 0);
            int sum = 0;
            for (int i = 0; i < n; i++)
            {
                byte b = stream[off + i];
                f[13 + 2 * i] = (byte)(b >> 4);
                f[14 + 2 * i] = (byte)(b & 0x0F);
                sum += (b >> 4) + (b & 0x0F);
            }
            f[7] = (byte)(sum & 0x7F);
            f[8] = kind;
            f[9] = (byte)(stream.Length & 0x7F);
            f[10] = (byte)(stream.Length >> 7);
            f[11] = (byte)(off & 0x7F);
            f[12] = (byte)(off >> 7);
            f[^1] = 0xF7;
            frames.Add(f);
        }
        return frames;
    }

    /// <summary>Parses a frame; accepts it with or without the leading F0.</summary>
    public static bool TryParseFrame(ReadOnlySpan<byte> frame, out AmperoFrame result)
    {
        result = default;
        if (frame.Length > 0 && frame[0] != 0xF0)
        {
            var withF0 = new byte[frame.Length + 1];
            withF0[0] = 0xF0;
            frame.CopyTo(withF0.AsSpan(1));
            return TryParseFrame(withF0, out result);
        }
        if (frame.Length < 14 || frame[^1] != 0xF7 || !frame.StartsWith(Header)) return false;
        var nib = frame.Slice(13, frame.Length - 14);
        if ((nib.Length & 1) != 0) return false;
        var data = new byte[nib.Length / 2];
        int sum = 0;
        for (int i = 0; i < data.Length; i++)
        {
            byte hi = nib[2 * i], lo = nib[2 * i + 1];
            if (hi > 0x0F || lo > 0x0F) return false;
            data[i] = (byte)((hi << 4) | lo);
            sum += hi + lo;
        }
        result = new AmperoFrame(frame[7], frame[8], frame[9] | (frame[10] << 7), frame[11] | (frame[12] << 7),
                                 data, (sum & 0x7F) == frame[7]);
        return true;
    }
}

/// <summary>
/// Collects frames of one direction into complete <see cref="AmperoMessage"/>s.
/// Note: the checksum of device replies follows an unknown rule; the editor ignores
/// mismatches, so does this class (see <see cref="AmperoMessage.ChecksumOk"/>).
/// </summary>
public sealed class SysExReassembler
{
    byte[]? _buf;
    int _filled, _total;
    byte _kind;
    bool _csOk;

    public void Reset() { _buf = null; _filled = 0; }

    /// <summary>Feeds one SysEx (with or without F0). Returns a message once a transfer is complete.</summary>
    public AmperoMessage? Push(ReadOnlySpan<byte> sysex)
    {
        if (!AmperoSysEx.TryParseFrame(sysex, out var f)) return null;
        if (f.Offset == 0)
        {
            _buf = new byte[f.Total];
            _total = f.Total;
            _filled = 0;
            _kind = f.Kind;
            _csOk = true;
        }
        if (_buf == null || f.Offset != _filled || _filled + f.Data.Length > _total) { Reset(); return null; }
        f.Data.CopyTo(_buf, _filled);
        _filled += f.Data.Length;
        _csOk &= f.ChecksumOk;
        if (_filled < _total) return null;

        var b = _buf;
        Reset();
        if (b.Length < 8) return null;
        uint cmd = BinaryPrimitives.ReadUInt32LittleEndian(b);
        int len = (int)BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(4));
        if (8 + len > b.Length) return null;
        byte[] payload = len == 0 ? Array.Empty<byte>() : Lzo1x.Decompress(b.AsSpan(8, len));
        return new AmperoMessage(cmd, _kind, payload, _csOk);
    }
}
