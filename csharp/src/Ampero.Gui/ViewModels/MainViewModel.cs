using System;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using Ampero.Midi;
using Ampero.Protocol;
using Microsoft.Win32;

namespace Ampero.Gui.ViewModels;

public sealed class MainViewModel : ObservableObject, IDisposable
{
    readonly Dispatcher _ui = Application.Current.Dispatcher;
    readonly DispatcherTimer _poll;
    AlgorithmDatabase _algs;
    AmperoDevice? _device;
    AmperoPreset? _preset;
    bool _busy;

    string? _selectedInput, _selectedOutput, _firmware, _presetFilter;
    string _status = "Nicht verbunden";
    string _presetName = "–", _presetNumber = "", _presetStyle = "";
    int _activeScene = -1;
    bool _pollDevice = true;
    PresetListEntry? _selectedPreset;

    public MainViewModel()
    {
        _algs = AlgorithmDatabase.LoadDefault();
        for (int i = 0; i < AmperoPreset.SceneCount; i++) Scenes.Add(new SceneViewModel(i));

        RefreshPortsCommand = new RelayCommand(_ => RefreshPorts());
        ConnectCommand = new RelayCommand(_ => ToggleConnectionAsync(), _ => SelectedInput != null && SelectedOutput != null);
        SelectSceneCommand = new RelayCommand(p => SelectSceneAsync(Convert.ToInt32(p, CultureInfo.InvariantCulture)), _ => _preset != null);
        LoadSelectedPresetCommand = new RelayCommand(_ => SelectPresetOnDeviceAsync(), _ => IsConnected && SelectedPreset != null);
        ReloadCommand = new RelayCommand(_ => ReloadCurrentAsync(), _ => IsConnected);
        OpenFileCommand = new RelayCommand(_ => OpenFile());
        SaveFileCommand = new RelayCommand(_ => SaveFile(), _ => _preset != null);
        LoadAlgDbCommand = new RelayCommand(_ => LoadAlgDb());
        ClearLogCommand = new RelayCommand(_ => Log.Clear());

        _poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _poll.Tick += async (_, _) => await PollAsync();

        RefreshPorts();
        AddLog(_algs.Count > 0 ? $"Algorithmus-DB: {_algs.Count} Effekte" : "Algorithmus-DB nicht gefunden (Datei → Algorithmus-DB laden…)");
    }

    // ------------------------------------------------------------------ bindable state

    public ObservableCollection<string> Inputs { get; } = new();
    public ObservableCollection<string> Outputs { get; } = new();
    public ObservableCollection<PresetListEntry> PresetList { get; } = new();
    public ObservableCollection<PresetListEntry> FilteredPresets { get; } = new();
    public ObservableCollection<SceneViewModel> Scenes { get; } = new();
    public ObservableCollection<SlotRow> Slots { get; } = new();
    public ObservableCollection<FootswitchRow> Footswitches { get; } = new();
    public ObservableCollection<string> Log { get; } = new();

    public RelayCommand RefreshPortsCommand { get; }
    public RelayCommand ConnectCommand { get; }
    public RelayCommand SelectSceneCommand { get; }
    public RelayCommand LoadSelectedPresetCommand { get; }
    public RelayCommand ReloadCommand { get; }
    public RelayCommand OpenFileCommand { get; }
    public RelayCommand SaveFileCommand { get; }
    public RelayCommand LoadAlgDbCommand { get; }
    public RelayCommand ClearLogCommand { get; }

    public string? SelectedInput { get => _selectedInput; set => Set(ref _selectedInput, value); }
    public string? SelectedOutput { get => _selectedOutput; set => Set(ref _selectedOutput, value); }
    public string Status { get => _status; set => Set(ref _status, value); }
    public string? Firmware { get => _firmware; set => Set(ref _firmware, value); }
    public string PresetName { get => _presetName; set => Set(ref _presetName, value); }
    public string PresetNumber { get => _presetNumber; set => Set(ref _presetNumber, value); }
    public string PresetStyle { get => _presetStyle; set => Set(ref _presetStyle, value); }
    public bool PollDevice { get => _pollDevice; set => Set(ref _pollDevice, value); }
    public bool IsConnected => _device != null;
    public string ConnectLabel => IsConnected ? "Trennen" : "Verbinden";
    public PresetListEntry? SelectedPreset { get => _selectedPreset; set => Set(ref _selectedPreset, value); }

    public string? PresetFilter
    {
        get => _presetFilter;
        set { if (Set(ref _presetFilter, value)) ApplyFilter(); }
    }

    public int ActiveScene
    {
        get => _activeScene;
        private set
        {
            if (!Set(ref _activeScene, value)) return;
            foreach (var s in Scenes) s.IsActive = s.Index == value;
            UpdateSlots();
        }
    }

    // ------------------------------------------------------------------ connection

    void RefreshPorts()
    {
        Inputs.Clear();
        Outputs.Clear();
        try
        {
            foreach (var n in AmperoDevice.InputNames()) Inputs.Add(n);
            foreach (var n in AmperoDevice.OutputNames()) Outputs.Add(n);
            var (i, o) = AmperoDevice.FindPorts();
            SelectedInput = i ?? Inputs.FirstOrDefault();
            SelectedOutput = o ?? Outputs.FirstOrDefault();
        }
        catch (Exception ex)
        {
            AddLog("MIDI-Ports: " + ex.Message);
        }
    }

    async Task ToggleConnectionAsync()
    {
        if (_device != null)
        {
            Disconnect("Getrennt");
            return;
        }
        try
        {
            _device = AmperoDevice.Open(SelectedInput!, SelectedOutput!);
        }
        catch (Exception ex)
        {
            _device = null;
            Status = "Fehler beim Öffnen";
            AddLog($"Öffnen fehlgeschlagen: {ex.Message} (läuft noch der Hotone-Editor?)");
            return;
        }
        _device.FrameTransferred += (sent, f) => _ui.InvokeAsync(() => AddLog((sent ? "→ " : "← ") + Describe(f)));
        _device.SceneChanged += s => _ui.InvokeAsync(() => { AddLog($"Gerät: Szene {s + 1}"); ActiveScene = s; });
        _device.PresetChanged += p => _ui.InvokeAsync(() => LoadPresetAsync(p));
        _device.Error += ex => _ui.InvokeAsync(() => AddLog("Fehler: " + ex.Message));
        OnPropertyChanged(nameof(IsConnected));
        OnPropertyChanged(nameof(ConnectLabel));

        try
        {
            Status = "Verbinde…";
            _device.Connect();
            await Task.Delay(100);
            Firmware = await _device.GetFirmwareVersionAsync();
            var list = await _device.GetPresetListAsync();
            PresetList.Clear();
            foreach (var e in list) PresetList.Add(e);
            ApplyFilter();
            await ReloadCurrentAsync();
            Status = $"Verbunden ({_device.InputName})";
            _poll.Start();
        }
        catch (Exception ex)
        {
            AddLog("Verbindung: " + ex.Message);
            Disconnect("Keine Antwort vom Gerät");
        }
    }

    void Disconnect(string status)
    {
        _poll.Stop();
        _device?.Dispose();
        _device = null;
        Status = status;
        OnPropertyChanged(nameof(IsConnected));
        OnPropertyChanged(nameof(ConnectLabel));
    }

    // ------------------------------------------------------------------ preset / scene

    async Task ReloadCurrentAsync()
    {
        if (_device == null) return;
        int index = await _device.GetCurrentPresetAsync();
        await LoadPresetAsync(index);
    }

    async Task LoadPresetAsync(int index)
    {
        if (_device == null || _busy) return;
        _busy = true;
        try
        {
            var preset = await _device.GetPresetAsync(index);
            ShowPreset(preset);
            ActiveScene = await _device.GetCurrentSceneAsync();
            var fs = await _device.GetFootswitchBlockAsync();
            ShowFootswitches(fs);
            SelectedPreset = PresetList.FirstOrDefault(p => p.Index == index);
        }
        catch (Exception ex)
        {
            AddLog($"Preset {index}: {ex.Message}");
        }
        finally
        {
            _busy = false;
        }
    }

    async Task SelectPresetOnDeviceAsync()
    {
        if (_device == null || SelectedPreset == null) return;
        _device.SelectPreset(SelectedPreset.Index);
        await Task.Delay(150);
        await LoadPresetAsync(SelectedPreset.Index);
    }

    async Task SelectSceneAsync(int scene)
    {
        if (_device != null) _device.SelectScene(scene);
        ActiveScene = scene;
        await Task.CompletedTask;
    }

    async Task PollAsync()
    {
        if (_device == null || _busy || !PollDevice) return;
        _busy = true;
        try
        {
            int index = await _device.GetCurrentPresetAsync();
            int scene = await _device.GetCurrentSceneAsync();
            _busy = false;
            if (_preset == null || index != _preset.Index) await LoadPresetAsync(index);
            else ActiveScene = scene;
        }
        catch (TimeoutException)
        {
            AddLog("Gerät antwortet nicht");
        }
        catch (Exception ex)
        {
            AddLog("Abfrage: " + ex.Message);
        }
        finally
        {
            _busy = false;
        }
    }

    void ShowPreset(AmperoPreset preset)
    {
        _preset = preset;
        PresetName = preset.Name;
        PresetStyle = preset.Style;
        PresetNumber = preset.Index >= 0 ? $"Preset #{preset.Index:000}" : "Datei";
        foreach (var s in Scenes)
        {
            s.Name = preset.GetSceneName(s.Index);
            s.Details = $"Vol {preset.GetSceneVolume(s.Index)} · {preset.GetSceneTempo(s.Index)} BPM";
        }
        UpdateSlots();
        CommandManager_Invalidate();
    }

    void ShowFootswitches(byte[] block)
    {
        Footswitches.Clear();
        for (int i = 0; i < 3 && i < block.Length; i++)
            Footswitches.Add(new FootswitchRow($"FS{i + 1}", $"0x{block[i]:X2}" + (block[i] is 0x1F or 0x20 or 0x21 ? "  (Effekt-Schalter)" : "")));
    }

    void UpdateSlots()
    {
        Slots.Clear();
        if (_preset == null) return;
        int scene = Math.Clamp(ActiveScene, 0, AmperoPreset.SceneCount - 1);
        for (int slot = 0; slot < AmperoPreset.SlotCount; slot++)
        {
            uint code = _preset.GetAlgorithm(slot);
            if (code == AmperoPreset.EmptyAlgorithm) continue;
            var alg = _algs.Get(code);
            string pars = alg == null
                ? string.Join(" ", Enumerable.Range(0, 8).Select(i => Fmt(_preset.GetParameter(scene, slot, i))))
                : string.Join("   ", alg.Parameters.Where(p => p.Id < AmperoPreset.ParamsPerSlot)
                                                  .Select(p => $"{p.Name} {Fmt(_preset.GetParameter(scene, slot, p.Id))}"));
            Slots.Add(new SlotRow(slot + 1, alg?.Module ?? $"0x{code >> 24:X2}", alg?.Name ?? $"0x{code:X8}",
                                  _preset.IsSlotEnabled(scene, slot), pars));
        }
    }

    static string Fmt(float v) => float.IsFinite(v) ? v.ToString("0.##", CultureInfo.InvariantCulture) : "–";

    void ApplyFilter()
    {
        FilteredPresets.Clear();
        foreach (var p in PresetList.Where(p => string.IsNullOrWhiteSpace(PresetFilter) ||
                                                p.Name.Contains(PresetFilter, StringComparison.OrdinalIgnoreCase) ||
                                                p.Index.ToString(CultureInfo.InvariantCulture) == PresetFilter.Trim()))
            FilteredPresets.Add(p);
    }

    // ------------------------------------------------------------------ files

    void OpenFile()
    {
        var dlg = new OpenFileDialog { Filter = "Ampero-Presets (*.prst;*.arr;*.bin;*.raw)|*.prst;*.arr;*.bin;*.raw|Alle Dateien|*.*" };
        if (dlg.ShowDialog() != true) return;
        try
        {
            var preset = PresetFile.Load(dlg.FileName);
            ShowPreset(preset);
            Footswitches.Clear();
            ShowFootswitches(preset.FootswitchFunctions);
            ActiveScene = 0;
            AddLog($"Datei geladen: {dlg.FileName} ({preset.ToBytes().Length} Byte)");
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "Preset öffnen", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    void SaveFile()
    {
        if (_preset == null) return;
        var dlg = new SaveFileDialog
        {
            Filter = "Rohdaten TLV (*.raw)|*.raw|Editor-Format (*.prst)|*.prst",
            FileName = string.Concat(_preset.Name.Split(System.IO.Path.GetInvalidFileNameChars())),
        };
        if (dlg.ShowDialog() != true) return;
        PresetFile.Save(_preset, dlg.FileName);
        AddLog($"Gespeichert: {dlg.FileName}");
    }

    void LoadAlgDb()
    {
        var dlg = new OpenFileDialog { Filter = "alg_data JSON (*.json)|*.json", FileName = "v1.0.9_alg_data.json" };
        if (dlg.ShowDialog() != true) return;
        try
        {
            _algs = AlgorithmDatabase.Load(dlg.FileName);
            AddLog($"Algorithmus-DB: {_algs.Count} Effekte");
            UpdateSlots();
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "Algorithmus-DB", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    // ------------------------------------------------------------------ log

    static string Describe(byte[] frame)
    {
        if (!AmperoSysEx.TryParseFrame(frame, out var f)) return $"SysEx {frame.Length} B";
        string head = f.Offset == 0 && f.Data.Length >= 4
            ? AmperoCommand.NameOf(BitConverter.ToUInt32(f.Data, 0))
            : $"Fortsetzung @{f.Offset}";
        return $"{head}  kind 0x{f.Kind:X2}  {f.Offset + f.Data.Length}/{f.Total} B";
    }

    void AddLog(string line)
    {
        Log.Add($"{DateTime.Now:HH:mm:ss.fff}  {line}");
        while (Log.Count > 500) Log.RemoveAt(0);
    }

    static void CommandManager_Invalidate() => System.Windows.Input.CommandManager.InvalidateRequerySuggested();

    public void Dispose() => Disconnect("Getrennt");
}
