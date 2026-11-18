using System.Collections.Specialized;
using System.Windows;
using System.Windows.Input;
using Ampero.Gui.ViewModels;

namespace Ampero.Gui;

public partial class MainWindow : Window
{
    readonly MainViewModel _vm;

    public MainWindow()
    {
        InitializeComponent();
        _vm = new MainViewModel();
        DataContext = _vm;
        ((INotifyCollectionChanged)_vm.Log).CollectionChanged += (_, _) =>
        {
            if (LogList.Items.Count > 0) LogList.ScrollIntoView(LogList.Items[^1]);
        };
        Closed += (_, _) => _vm.Dispose();
    }

    void PresetList_DoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (_vm.LoadSelectedPresetCommand.CanExecute(null)) _vm.LoadSelectedPresetCommand.Execute(null);
    }
}
