using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Ampero.Protocol;
using Xunit;

namespace Ampero.Protocol.Tests;

public class ProtocolTests
{
    static string Data(string name) => Path.Combine(AppContext.BaseDirectory, "TestData", name);

    static List<(string Dir, byte[] Frame)> Capture() =>
        File.ReadAllLines(Data("capture_frames.txt"))
            .Where(l => l.Length > 0 && l[0] != '#')
            .Select(l => l.Split(' '))
            .Select(p => (p[0], Convert.FromHexString(p[1])))
            .ToList();

    /// <summary>Reassembles all transfers of the capture, keeping the frames of each.</summary>
    static List<(string Dir, AmperoMessage Msg, List<byte[]> Frames)> Transfers()
    {
        var asm = new Dictionary<string, SysExReassembler> { ["IN"] = new(), ["OUT"] = new() };
        var frames = new Dictionary<string, List<byte[]>> { ["IN"] = new(), ["OUT"] = new() };
        var result = new List<(string, AmperoMessage, List<byte[]>)>();
        foreach (var (dir, f) in Capture())
        {
            if (!AmperoSysEx.TryParseFrame(f, out var fr)) continue;
            if (fr.Offset == 0) frames[dir] = new List<byte[]>();
            frames[dir].Add(f);
            var m = asm[dir].Push(f);
            if (m != null) result.Add((dir, m, frames[dir]));
        }
        return result;
    }

    public static IEnumerable<object[]> Presets() =>
        Directory.GetFiles(Path.Combine(AppContext.BaseDirectory, "TestData"), "*.bin")
            .Select(f => new object[] { Path.GetFileNameWithoutExtension(f) });

    [Theory]
    [MemberData(nameof(Presets))]
    public void Lzo_RecompressionIsByteIdenticalToEditor(string name)
    {
        var bin = File.ReadAllBytes(Data(name + ".bin"));
        var raw = Lzo1x.Decompress(bin, out int consumed);
        Assert.Equal(bin.Length, consumed);
        Assert.Equal(5004, raw.Length);
        Assert.Equal(bin, Lzo1x.Compress(raw));
    }

    [Theory]
    [MemberData(nameof(Presets))]
    public void PresetFile_PrstRoundtripIsByteIdentical(string name)
    {
        var prst = File.ReadAllBytes(Data(name + ".prst"));
        var preset = PresetFile.Decode(prst, out var fmt);
        Assert.Equal(PresetFileFormat.Prst, fmt);
        Assert.Equal(prst, PresetFile.Encode(preset, PresetFileFormat.Prst));
    }

    [Fact]
    public void Lzo_RoundtripOnLargeRandomishData()
    {
        var rnd = new Random(7);
        var data = new byte[200_000];
        for (int i = 0; i < data.Length; i++) data[i] = (byte)(rnd.Next(8) == 0 ? rnd.Next(256) : data[Math.Max(0, i - 1 - rnd.Next(40))]);
        Assert.Equal(data, Lzo1x.Decompress(Lzo1x.Compress(data)));
    }

    [Fact]
    public void Preset_FieldsOfTestExport()
    {
        var p = PresetFile.Load(Data("A75-2-ABCTESTHIJKLMNOP.prst"));
        Assert.Equal("ABCTESTHIJKLMNOP", p.Name);
        Assert.Equal("Acoustic", p.Style);
        Assert.Equal(new[] { "Narf", "TEST", "Blub" }, Enumerable.Range(0, 3).Select(p.GetSceneName));
        Assert.Equal(0x07000000u, p.GetAlgorithm(0));
        Assert.Equal(AmperoPreset.EmptyAlgorithm, p.GetAlgorithm(1));
        Assert.Equal(20f, p.GetParameter(0, 0, 0));
        Assert.Equal(50f, p.GetParameter(2, 0, 1));
        Assert.True(p.IsSlotEnabled(1, 0));
        Assert.Equal(100, p.GetSceneVolume(0));
        Assert.Equal(120, p.GetSceneTempo(2));
    }

    [Fact]
    public void Preset_EditingKeepsEverythingElse()
    {
        var bytes = File.ReadAllBytes(Data("A69-2-Empty.prst"));
        var p = PresetFile.Decode(bytes, out _);
        var before = p.ToBytes();
        p.SetSceneName(1, "Solo");
        Assert.Equal("Solo", PresetFile.Decode(PresetFile.Encode(p, PresetFileFormat.Prst), out _).GetSceneName(1));
        var after = p.ToBytes();
        Assert.Equal(before.Length, after.Length);
        Assert.Equal(6, before.Zip(after).Count(t => t.First != t.Second));   // "Scene 2" -> "Solo\0\0\0"
    }

    [Fact]
    public void SysEx_HostFramesAreReproducedByteIdentical()
    {
        var host = Transfers().Where(t => t.Dir == "OUT").ToList();
        Assert.Equal(50, host.Count);
        foreach (var (_, m, frames) in host)
        {
            Assert.True(m.ChecksumOk);
            Assert.Equal(frames, AmperoSysEx.Encode(m.Command, m.Payload, m.Kind));
        }
    }

    [Fact]
    public void SysEx_DeviceFramesDifferOnlyInChecksum()
    {
        foreach (var (_, m, frames) in Transfers().Where(t => t.Dir == "IN"))
        {
            var mine = AmperoSysEx.Encode(m.Command, m.Payload, m.Kind);
            Assert.Equal(frames.Count, mine.Count);
            for (int i = 0; i < mine.Count; i++)
            {
                mine[i][7] = frames[i][7];
                Assert.Equal(frames[i], mine[i]);
            }
        }
    }

    [Fact]
    public void SysEx_PresetDumpsAndScenesFromCapture()
    {
        var t = Transfers();
        var dumps = t.Where(x => x.Dir == "IN" && x.Msg.Command == AmperoCommand.PresetDump)
                     .Select(x => AmperoPreset.FromDump(x.Msg.Payload)).ToList();
        Assert.Equal(new[] { "#297 Le Chug", "#000 Ampero Strikes!", "#008 Basic Clean", "#006 Juicy Fusion" },
                     dumps.Select(d => d.ToString()));
        Assert.All(dumps, d => Assert.Equal(5068, d.ToBytes().Length));

        var juicy = dumps[3];
        Assert.Equal(new byte[] { 0x0C, 0x29, 0x0D }, juicy.FootswitchFunctions);
        Assert.Equal("Scene 1", juicy.GetSceneName(0));
        Assert.Equal(0x0500_0001u, juicy.GetAlgorithm(1));

        var scenes = t.Where(x => x.Dir == "OUT" && x.Msg.Command == AmperoCommand.CurrentScene && x.Msg.Kind == AmperoKind.Set)
                      .Select(x => (int)x.Msg.Payload[0]);
        Assert.Equal(new[] { 1, 2, 0, 1, 2 }, scenes);

        var list = PresetList.Parse(t.Single(x => x.Msg.Command == AmperoCommand.PresetList && x.Dir == "IN").Msg.Payload);
        Assert.Equal(300, list.Count);
        Assert.Equal("Juicy Fusion", list[6].Name);
        Assert.Equal("Le Chug", list[297].Name);
    }

    [Fact]
    public void SysEx_EncodeSceneMatchesCapture()
    {
        var frame = AmperoSysEx.Encode(AmperoCommand.CurrentScene, new byte[] { 2 }).Single();
        Assert.Contains(Capture(), c => c.Dir == "OUT" && c.Frame.AsSpan().SequenceEqual(frame));
    }
}
