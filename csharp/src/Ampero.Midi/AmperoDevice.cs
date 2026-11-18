using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Ampero.Protocol;
using Melanchall.DryWetMidi.Core;
using Melanchall.DryWetMidi.Multimedia;

namespace Ampero.Midi;

/// <summary>
/// Talks to an Ampero II (Stomp) over USB-MIDI using DryWetMIDI.
/// Windows MIDI ports are exclusive: close the Hotone editor before opening the device.
/// </summary>
public sealed class AmperoDevice : IDisposable
{
    readonly InputDevice _input;
    readonly OutputDevice _output;
    readonly SysExReassembler _reassembler = new();
    readonly object _sync = new();
    readonly Dictionary<uint, TaskCompletionSource<AmperoMessage>> _pending = new();
    readonly SemaphoreSlim _requestGate = new(1, 1);

    AmperoDevice(InputDevice input, OutputDevice output)
    {
        _input = input;
        _output = output;
        _input.WaitForCompleteSysExEvent = true;
        _input.EventReceived += OnEventReceived;
        _input.StartEventsListening();
    }

    public string InputName => _input.Name;
    public string OutputName => _output.Name;

    /// <summary>Timeout for a single request/response.</summary>
    public TimeSpan Timeout { get; set; } = TimeSpan.FromSeconds(3);

    /// <summary>Every complete message from the device (raised on the MIDI thread).</summary>
    public event Action<AmperoMessage>? MessageReceived;
    /// <summary>Raw SysEx frames (including F0/F7) in both directions, for logging.</summary>
    public event Action<bool, byte[]>? FrameTransferred;   // true = sent
    /// <summary>Scene changed on the device (notification that was not an answer to a request).</summary>
    public event Action<int>? SceneChanged;
    /// <summary>Preset changed on the device (notification that was not an answer to a request).</summary>
    public event Action<int>? PresetChanged;
    public event Action<Exception>? Error;

    // ------------------------------------------------------------------ discovery

    public static IReadOnlyList<string> InputNames() => InputDevice.GetAll().Select(Dispose).ToList();
    public static IReadOnlyList<string> OutputNames() => OutputDevice.GetAll().Select(Dispose).ToList();

    static string Dispose(MidiDevice d) { var n = d.Name; d.Dispose(); return n; }

    /// <summary>Port names that look like an Ampero (contain "Ampero" or "MP-300"), or null.</summary>
    public static (string? Input, string? Output) FindPorts()
    {
        static bool Match(string n) => n.Contains("Ampero", StringComparison.OrdinalIgnoreCase) || n.Contains("MP-300", StringComparison.OrdinalIgnoreCase);
        return (InputNames().FirstOrDefault(Match), OutputNames().FirstOrDefault(Match));
    }

    public static AmperoDevice Open(string inputName, string outputName)
    {
        var input = InputDevice.GetByName(inputName);
        try
        {
            return new AmperoDevice(input, OutputDevice.GetByName(outputName));
        }
        catch
        {
            input.Dispose();
            throw;
        }
    }

    // ------------------------------------------------------------------ low level

    /// <summary>Sends one transfer (no answer expected).</summary>
    public void Send(uint command, ReadOnlySpan<byte> payload, byte kind = AmperoKind.Set)
    {
        foreach (var frame in AmperoSysEx.Encode(command, payload, kind))
        {
            _output.SendEvent(new NormalSysExEvent(frame.AsSpan(1).ToArray()));   // DryWetMIDI: data without F0
            FrameTransferred?.Invoke(true, frame);
        }
    }

    /// <summary>Sends a request and waits for the device's answer with the same command code.</summary>
    public async Task<AmperoMessage> RequestAsync(uint command, byte[]? payload = null, CancellationToken ct = default)
    {
        await _requestGate.WaitAsync(ct).ConfigureAwait(false);
        var tcs = new TaskCompletionSource<AmperoMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
        try
        {
            lock (_sync) _pending[command] = tcs;
            Send(command, payload ?? Array.Empty<byte>(), AmperoKind.Request);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(Timeout);
            using (timeout.Token.Register(() => tcs.TrySetCanceled()))
            {
                try
                {
                    return await tcs.Task.ConfigureAwait(false);
                }
                catch (TaskCanceledException) when (!ct.IsCancellationRequested)
                {
                    throw new TimeoutException($"no answer to {AmperoCommand.NameOf(command)}");
                }
            }
        }
        finally
        {
            lock (_sync) _pending.Remove(command);
            _requestGate.Release();
        }
    }

    void OnEventReceived(object? sender, MidiEventReceivedEventArgs e)
    {
        try
        {
            if (e.Event is not NormalSysExEvent sx) return;
            var frame = new byte[sx.Data.Length + 1];
            frame[0] = 0xF0;
            sx.Data.CopyTo(frame, 1);
            FrameTransferred?.Invoke(false, frame);

            var msg = _reassembler.Push(frame);
            if (msg == null) return;
            MessageReceived?.Invoke(msg);

            TaskCompletionSource<AmperoMessage>? tcs;
            lock (_sync) _pending.TryGetValue(msg.Command, out tcs);
            if (tcs != null && tcs.TrySetResult(msg)) return;

            if (msg.Command == AmperoCommand.CurrentScene) SceneChanged?.Invoke((int)msg.PayloadU32);
            else if (msg.Command == AmperoCommand.CurrentPreset) PresetChanged?.Invoke((int)msg.PayloadU32);
        }
        catch (Exception ex)
        {
            Error?.Invoke(ex);
        }
    }

    static byte[] U32(int v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(b, v);
        return b;
    }

    // ------------------------------------------------------------------ commands

    /// <summary>Announces the host like the editor does (no answer).</summary>
    public void Connect() => Send(AmperoCommand.Connect, ReadOnlySpan<byte>.Empty, AmperoKind.Request);

    public async Task<int> GetCurrentPresetAsync(CancellationToken ct = default) =>
        (int)(await RequestAsync(AmperoCommand.CurrentPreset, null, ct).ConfigureAwait(false)).PayloadU32;

    public void SelectPreset(int index) => Send(AmperoCommand.CurrentPreset, U32(index));

    public async Task<AmperoPreset> GetPresetAsync(int index, CancellationToken ct = default) =>
        AmperoPreset.FromDump((await RequestAsync(AmperoCommand.PresetDump, U32(index), ct).ConfigureAwait(false)).Payload);

    public async Task<int> GetCurrentSceneAsync(CancellationToken ct = default) =>
        (int)(await RequestAsync(AmperoCommand.CurrentScene, null, ct).ConfigureAwait(false)).PayloadU32;

    /// <summary>Selects scene 0..2 of the current preset.</summary>
    public void SelectScene(int scene) => Send(AmperoCommand.CurrentScene, new[] { (byte)scene });

    public async Task<IReadOnlyList<PresetListEntry>> GetPresetListAsync(CancellationToken ct = default) =>
        PresetList.Parse((await RequestAsync(AmperoCommand.PresetList, null, ct).ConfigureAwait(false)).Payload);

    public async Task<string> GetFirmwareVersionAsync(CancellationToken ct = default)
    {
        var p = (await RequestAsync(AmperoCommand.FirmwareVersion, null, ct).ConfigureAwait(false)).Payload;
        int n = Array.IndexOf(p, (byte)0);
        return System.Text.Encoding.ASCII.GetString(p, 0, n < 0 ? p.Length : n);
    }

    /// <summary>Footswitch block of the current preset (= TLV tag 3; bytes 0..2 = FS1..FS3 functions).</summary>
    public async Task<byte[]> GetFootswitchBlockAsync(CancellationToken ct = default) =>
        (await RequestAsync(AmperoCommand.FootswitchBlock, null, ct).ConfigureAwait(false)).Payload;

    /// <summary>Sets the function codes of FS1..FS3 (as the editor does; not saved until the preset is saved).</summary>
    public void SetFootswitchFunctions(byte fs1, byte fs2, byte fs3) =>
        Send(AmperoCommand.FootswitchFunctionsSet, new[] { fs1, fs2, fs3 });

    /// <summary>Assigns effect slots to a footswitch (fs 0..2, 12 flags).</summary>
    public void SetFootswitchSlots(int fs, IReadOnlyList<bool> slots)
    {
        if (slots.Count != AmperoPreset.SlotCount) throw new ArgumentException("12 slot flags expected");
        var p = new byte[13];
        p[0] = (byte)fs;
        for (int i = 0; i < 12; i++) p[1 + i] = (byte)(slots[i] ? 1 : 0);
        Send(AmperoCommand.FootswitchSlotsSet, p);
    }

    public void Dispose()
    {
        _input.EventReceived -= OnEventReceived;
        try { _input.StopEventsListening(); } catch { /* device may be gone */ }
        _input.Dispose();
        _output.Dispose();
        lock (_sync)
            foreach (var t in _pending.Values) t.TrySetCanceled();
    }
}
