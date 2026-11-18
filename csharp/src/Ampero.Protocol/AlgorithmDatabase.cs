using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace Ampero.Protocol;

public sealed record ParameterInfo(int Id, string Name, double Min, double Max, double Default);

public sealed record AlgorithmInfo(uint Code, string Module, string Name, IReadOnlyList<ParameterInfo> Parameters);

/// <summary>
/// Effect names and parameters from the editor's <c>v1.0.9_alg_data.json</c>
/// (<c>data/flutter_assets/assets/data</c>). Codes: high byte = module type.
/// </summary>
public sealed class AlgorithmDatabase
{
    readonly Dictionary<uint, AlgorithmInfo> _algs;

    AlgorithmDatabase(Dictionary<uint, AlgorithmInfo> algs) => _algs = algs;

    public static AlgorithmDatabase Empty { get; } = new(new());

    public int Count => _algs.Count;

    public static AlgorithmDatabase Load(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        var algs = new Dictionary<uint, AlgorithmInfo>();
        var catalog = doc.RootElement.GetProperty("PluginProperties").GetProperty("Modules").GetProperty("Catalog");
        foreach (var module in catalog.EnumerateArray())
        {
            string moduleName = module.GetProperty("Name").GetString() ?? "";
            foreach (var a in module.GetProperty("Alg").EnumerateArray())
            {
                uint code = uint.Parse(a.GetProperty("Code").GetString()!, CultureInfo.InvariantCulture);
                var pars = new List<ParameterInfo>();
                if (a.TryGetProperty("Widget", out var w))
                {
                    var items = w.ValueKind == JsonValueKind.Array ? w.EnumerateArray().ToList() : new List<JsonElement> { w };
                    foreach (var p in items.Where(p => p.ValueKind == JsonValueKind.Object))
                        pars.Add(new ParameterInfo(Int(p, "ID"), Str(p, "Name"), Num(p, "DisplayMin"), Num(p, "DisplayMax"), Num(p, "DefaultValue")));
                }
                algs[code] = new AlgorithmInfo(code, moduleName, a.GetProperty("Name").GetString() ?? "", pars.OrderBy(p => p.Id).ToList());
            }
        }
        return new AlgorithmDatabase(algs);
    }

    /// <summary>Tries the usual locations relative to the working directory and the executable.</summary>
    public static AlgorithmDatabase LoadDefault()
    {
        const string rel = @"windows software\data\flutter_assets\assets\data\v1.0.9_alg_data.json";
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "v1.0.9_alg_data.json"),
            Path.Combine(Environment.CurrentDirectory, "v1.0.9_alg_data.json"),
            Path.Combine(@"C:\temp\ampero\windows", rel),
        };
        var path = candidates.FirstOrDefault(File.Exists);
        return path == null ? Empty : Load(path);
    }

    public AlgorithmInfo? Get(uint code) => _algs.TryGetValue(code, out var a) ? a : null;

    public string Describe(uint code) =>
        code == AmperoPreset.EmptyAlgorithm ? "–" : Get(code) is { } a ? $"{a.Module} {a.Name}" : $"0x{code:X8}";

    static string Str(JsonElement e, string n) => e.TryGetProperty(n, out var v) ? v.GetString() ?? "" : "";
    static int Int(JsonElement e, string n) => int.TryParse(Str(e, n), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : 0;
    static double Num(JsonElement e, string n) => double.TryParse(Str(e, n), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0;
}
