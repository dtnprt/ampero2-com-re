namespace Ampero.Gui.ViewModels;

public sealed class SceneViewModel : ObservableObject
{
    string _name = "";
    bool _isActive;
    string _details = "";

    public SceneViewModel(int index) => Index = index;

    public int Index { get; }
    public string Label => $"Szene {Index + 1}";
    public string Name { get => _name; set => Set(ref _name, value); }
    public string Details { get => _details; set => Set(ref _details, value); }
    public bool IsActive { get => _isActive; set => Set(ref _isActive, value); }
}

public sealed record SlotRow(int Slot, string Module, string Effect, bool Enabled, string Parameters);

public sealed record FootswitchRow(string Footswitch, string Function);
