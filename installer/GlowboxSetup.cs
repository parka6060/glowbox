using System;
using System.IO;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Windows.Forms;
using System.Drawing;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

class GlowboxSetup {
    static readonly string[] LoadingImages = { "loading.png", "loading-failed.png", "spinner.png" };
    [STAThread]
    static int Main(string[] args) {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        try {
            using (var form = new SetupWindow()) {
                Application.Run(form);
                return form.Installed ? 0 : 1;
            }
        } catch (Exception error) {
            MessageBox.Show(error.Message,"Glowbox Compositor Setup",MessageBoxButtons.OK,MessageBoxIcon.Error);
            return 1;
        }
    }

    internal static string NormalizeFolder(string folder) {
        string path = Path.GetFullPath(folder.Trim().Trim('"'));
        var expanded = new StringBuilder(32768);
        if (GetLongPathName(path, expanded, (uint)expanded.Capacity) > 0) path = expanded.ToString();
        string drive = Path.GetPathRoot(path);
        return path.Length == drive.Length ? path : path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }
    internal static bool IsObsFolder(string folder) {
        try { return IsX64Executable(Path.Combine(NormalizeFolder(folder), "bin", "64bit", "obs64.exe")); }
        catch (Exception error) {
            if (error is IOException || error is UnauthorizedAccessException || error is ArgumentException || error is NotSupportedException) return false;
            throw;
        }
    }
    static bool IsX64Executable(string path) {
        try {
            using (var reader = new BinaryReader(File.OpenRead(path))) {
                if (reader.BaseStream.Length < 64 || reader.ReadUInt16() != 0x5a4d) return false;
                reader.BaseStream.Position = 0x3c; int pe = reader.ReadInt32();
                if (pe < 64 || pe > reader.BaseStream.Length - 26) return false;
                reader.BaseStream.Position = pe;
                if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664) return false;
                reader.BaseStream.Position = pe + 24;
                return reader.ReadUInt16() == 0x20b;
            }
        } catch (IOException) { return false; } catch (UnauthorizedAccessException) { return false; }
    }
    internal static List<string> FindObsFolders(IEnumerable<string> candidates) {
        var found = new List<string>();
        foreach (string candidate in candidates) {
            if (String.IsNullOrWhiteSpace(candidate) || !IsObsFolder(candidate)) continue;
            string path = NormalizeFolder(candidate);
            if (!found.Exists(delegate(string existing) { return String.Equals(existing, path, StringComparison.OrdinalIgnoreCase); }))
                found.Add(path);
        }
        return found;
    }
    static List<string> DiscoverObs() {
        var candidates = new List<string>();
        candidates.Add(AppDomain.CurrentDomain.BaseDirectory);
        string besideInstaller = NormalizeFolder(AppDomain.CurrentDomain.BaseDirectory);
        if (String.Equals(Path.GetFileName(besideInstaller), "plugins", StringComparison.OrdinalIgnoreCase))
            candidates.Add(Path.GetDirectoryName(besideInstaller));
        foreach (string key in new[] { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" }) {
            string folder = Environment.GetEnvironmentVariable(key);
            if (!String.IsNullOrEmpty(folder)) candidates.Add(Path.Combine(folder, "obs-studio"));
        }

        foreach (var process in Process.GetProcessesByName("obs64")) {
            using (process) {
                try {
                    string exe = process.MainModule.FileName;
                    candidates.Add(Path.GetFullPath(Path.Combine(Path.GetDirectoryName(exe), "..", "..")));
                } catch (System.ComponentModel.Win32Exception) { }
                  catch (InvalidOperationException) { }
            }
        }
        return FindObsFolders(candidates);
    }
    // Windows Common Item Dialog, in folder mode.

    [ComImport, Guid("42F85136-DB7E-439C-85F1-E4075D135FC8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IFolderDialog {
        [PreserveSig] int Show(IntPtr owner);
        void SetFileTypes(uint count, IntPtr filters);
        void SetFileTypeIndex(uint index);
        void GetFileTypeIndex(out uint index);
        void Advise(IntPtr events, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint options);
        void GetOptions(out uint options);
        void SetDefaultFolder(IShellItem item);
        void SetFolder(IShellItem item);
        void GetFolder(out IShellItem item);
        void GetCurrentSelection(out IShellItem item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName(out IntPtr name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void GetResult(out IShellItem item);
        void AddPlace(IShellItem item, uint alignment);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string extension);
        void Close(int result);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr filter);
    }
    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IShellItem {
        void BindToHandler(IntPtr context, ref Guid handler, ref Guid iid, out IntPtr result);
        void GetParent(out IShellItem parent);
        void GetDisplayName(uint nameType, out IntPtr name);
        void GetAttributes(uint mask, out uint attributes);
        void Compare(IShellItem other, uint hint, out int order);
    }
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr context, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IShellItem item);
    internal static IFolderDialog CreateFolderDialog(string current) {
        var dialog=(IFolderDialog)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7")));
        try {
            uint options; dialog.GetOptions(out options);
            // FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT
            dialog.SetOptions(options | 0x20u | 0x40u | 0x800u | 0x02000000u);
            dialog.SetTitle("Choose your OBS Studio folder");
            dialog.SetOkButtonLabel("Select folder");
            if (!String.IsNullOrWhiteSpace(current)) {
                string path=NormalizeFolder(current);
                if (Directory.Exists(path)) {
                    IShellItem initial=null;
                    try {
                        Guid iid=typeof(IShellItem).GUID;
                        SHCreateItemFromParsingName(path,IntPtr.Zero,ref iid,out initial);
                        dialog.SetFolder(initial);
                    } finally { if (initial!=null) Marshal.ReleaseComObject(initial); }
                }
            }
            return dialog;
        } catch { Marshal.ReleaseComObject(dialog); throw; }
    }
    static string PickObsFolder(IWin32Window owner, string current) {
        // An invalid pasted path should not stop Browse from opening.
        string initial=null;
        try { if (Directory.Exists(NormalizeFolder(current))) initial=NormalizeFolder(current); }
        catch (ArgumentException) { } catch (NotSupportedException) { } catch (IOException) { }
        var dialog=CreateFolderDialog(initial);
        IShellItem result=null;
        IntPtr name=IntPtr.Zero;
        try {
            int hr=dialog.Show(owner.Handle);
            if (hr==unchecked((int)0x800704C7)) return null; // User cancelled, keep the existing path.
            Marshal.ThrowExceptionForHR(hr);
            dialog.GetResult(out result);
            result.GetDisplayName(0x80058000u,out name); // SIGDN_FILESYSPATH
            return Marshal.PtrToStringUni(name);
        } finally {
            if (name!=IntPtr.Zero) Marshal.FreeCoTaskMem(name);
            if (result!=null) Marshal.ReleaseComObject(result);
            Marshal.ReleaseComObject(dialog);
        }
    }
    sealed class SetupWindow : Form {
        static readonly Color Orange = Color.FromArgb(255, 139, 52);
        static readonly Color Ink = Color.FromArgb(37, 34, 31);
        static readonly Color Muted = Color.FromArgb(91, 89, 86);
        readonly ComboBox folder = new ComboBox();
        readonly Label title = new Label();
        readonly Label intro = new Label();
        readonly Label help = new Label();
        readonly Label status = new Label();
        readonly Label locationLabel = new Label();
        readonly Button browse = new Button();
        readonly Button install = new Button();
        readonly Button cancel = new Button();
        readonly ToolTip pathTip = new ToolTip();
        readonly CheckBox portable = new CheckBox();
        bool busy;
        public bool Installed { get; private set; }
        void PutLabel(Label label, string text, int x, int y, int w, int h, bool heading) {
            label.Text=text; label.SetBounds(x,y,w,h); label.ForeColor=Ink;
            label.Font=heading ? new Font(SystemFonts.MessageBoxFont.FontFamily,12,FontStyle.Regular) : SystemFonts.MessageBoxFont;
            Controls.Add(label);
        }
        void PutButton(Button button, string text, int x, int y, int w) {
            button.Text=text; button.SetBounds(x,y,w,30);
            button.UseVisualStyleBackColor=true;
            Controls.Add(button);
        }
        public SetupWindow() {
            SuspendLayout();
            AutoScaleDimensions=new SizeF(96,96); AutoScaleMode=AutoScaleMode.Dpi;
            Text="Glowbox Setup";
            ClientSize=new Size(660,350); FormBorderStyle=FormBorderStyle.FixedDialog;
            MaximizeBox=false; MinimizeBox=false; StartPosition=FormStartPosition.CenterScreen;
            BackColor=Color.White; ForeColor=Ink; Font=SystemFonts.MessageBoxFont;
            var art=new Panel { BackColor=Orange, Bounds=new Rectangle(0,0,132,350), AccessibleName="Orange artwork placeholder" };
            Controls.Add(art);
            PutLabel(title,"Install Glowbox",156,22,480,28,true);
            PutLabel(intro,"Choose where to install Glowbox.",156,60,480,26,false);
            PutLabel(locationLabel,"OBS Studio folder",156,100,480,22,false);
            folder.SetBounds(156,126,376,26); folder.DropDownStyle=ComboBoxStyle.DropDown;
            folder.AccessibleName="OBS Studio installation folder"; folder.DropDownWidth=650; folder.TabIndex=0;
            Controls.Add(folder);
            PutButton(browse,"&BrowseÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¾ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â¦ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦",544,122,92); browse.TabIndex=1;
            browse.Click+=delegate {
                try {
                    string selected=PickObsFolder(this,folder.Text);
                    if (selected!=null) folder.Text=selected;
                } catch (Exception error) {
                    MessageBox.Show(this,"Couldn't open the folder picker. Paste your OBS folder path instead.\n\n" + error.Message,
                        "Choose OBS folder",MessageBoxButtons.OK,MessageBoxIcon.Error);
                }
            };
            portable.Text="Portable installation"; portable.SetBounds(156,160,480,24); portable.TabIndex=2; Controls.Add(portable);
            portable.CheckedChanged+=delegate { UpdateTarget(); };
            PutLabel(help,"",156,188,480,28,false); help.ForeColor=Muted;
            PutLabel(status,"",156,222,480,68,false); status.AccessibleName="Installation status";
            PutButton(cancel,"&Cancel",432,298,96); cancel.TabIndex=3;
            PutButton(install,"&Install",540,298,96); install.TabIndex=4;
            cancel.Click+=delegate { Close(); };
            install.Click+=delegate { Install(); };
            CancelButton=cancel; AcceptButton=install;
            var found=DiscoverObs();
            foreach (string path in found) folder.Items.Add(path);
            help.Text=found.Count>1 ? "More than one OBS copy found. Choose the one you use." :
                "Use Browse if your OBS folder isn't listed.";
            folder.TextChanged+=delegate { UpdateTarget(); };
            if (found.Count==1) folder.SelectedIndex=0;
            else UpdateTarget();
            Shown+=delegate { FitToScreen(); };
            FormClosing+=delegate(object sender, FormClosingEventArgs e) { if (busy) e.Cancel=true; };
            ResumeLayout(false);
            PerformLayout();
        }
        internal void FitToScreen() {
            // Fit the actual control bounds after initial DPI scaling.
            ClientSize=new Size(Math.Max(ClientSize.Width,Math.Max(browse.Right,install.Right)+24),
                Math.Max(ClientSize.Height,install.Bottom+22));
            // Keep the actions reachable on small screens and at high display scaling.
            Rectangle area=Screen.FromControl(this).WorkingArea;
            if (Width>area.Width || Height>area.Height) {
                AutoScroll=true;
                AutoScrollMinSize=ClientSize;
                Size=new Size(Math.Min(Width,area.Width),Math.Min(Height,area.Height));
                Location=new Point(area.Left+(area.Width-Width)/2,area.Top+(area.Height-Height)/2);
            }
        }
        void UpdateTarget() {
            try {
                string root=NormalizeFolder(folder.Text);
                bool marked=HasPortableMarker(root);
                if(marked && !portable.Checked) portable.Checked=true;
                portable.Enabled=!marked && !busy && !Installed;
                var target=CreateTarget(root,portable.Checked);
                install.Enabled=!busy; pathTip.SetToolTip(folder,root);
                status.ForeColor=Ink;
                status.Text="OBS Studio " + target.ObsVersion + ". Close OBS before installing.\n" +
                    (target.Portable ? "Installs inside this portable OBS folder." : "Updates the shared Glowbox plugin for OBS " + MinimumObsVersion() + " and newer.") +
                    "\nReplaces an existing installation here, if present.";
            } catch (Exception error) {
                install.Enabled=false;
                status.ForeColor=String.IsNullOrWhiteSpace(folder.Text)?Muted:Color.FromArgb(159,48,32);
                status.Text=String.IsNullOrWhiteSpace(folder.Text)?"Choose your OBS folder.":error.Message;
            }
        }
        void Install() {
            if (!IsObsFolder(folder.Text)) { UpdateTarget(); return; }
            if (Process.GetProcessesByName("obs64").Length>0) {
                status.ForeColor=Color.FromArgb(159,48,32);
                status.Text="Close OBS, then click Install.";
                return;
            }
            busy=true; install.Enabled=false; browse.Enabled=false; folder.Enabled=false; portable.Enabled=false; cancel.Enabled=false;
            install.Text="InstallingÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¾ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¾Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â¦ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¦"; UseWaitCursor=true;
            try {
                InstallPayloads(CreateTarget(folder.Text,portable.Checked),ReadResource("glowbox.dll"),ReadResource("compositor.effect"));
                Installed=true;
                title.Text="Installation complete";
                intro.Text="Glowbox is installed.";
                help.Text="Open this copy of OBS. Right-click your scene and choose Filters.";
                status.ForeColor=Ink;
                status.Text="Open OBS to use Glowbox Compositor.";
                install.Visible=false; browse.Visible=false; portable.Enabled=false; cancel.Text="&Finish";
                AcceptButton=cancel;
            } catch (Exception error) {
                status.ForeColor=Color.FromArgb(159,48,32); status.Text=error.Message;
                pathTip.SetToolTip(status,error.ToString());
                MessageBox.Show(this,error.Message,"Couldn't install Glowbox",MessageBoxButtons.OK,MessageBoxIcon.Error);
                install.Enabled=true; browse.Enabled=true; folder.Enabled=true;
            } finally {
                busy=false; cancel.Enabled=true; UseWaitCursor=false; install.Text="&Install";
                if(!Installed)portable.Enabled=!HasPortableMarker(NormalizeFolder(folder.Text));
            }
        }
        protected override void Dispose(bool disposing) {
            if (disposing) pathTip.Dispose();
            base.Dispose(disposing);
        }
    }
    static byte[] ReadResource(string name) {
        using (var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream(name)) {
            if (resource == null) throw new Exception("The installer is missing a file. Download it again.\nMissing file: " + name);
            using (var buffer = new MemoryStream()) { resource.CopyTo(buffer); return buffer.ToArray(); }
        }
    }


    internal sealed class InstallTarget {
        internal readonly string ObsRoot, CommonRoot, Mode, BundleRoot;
        internal readonly Version ObsVersion;
        internal bool Portable { get { return Mode != "normal"; } }
        internal InstallTarget(string root, string common, string mode, Version version) {
            ObsRoot = root; CommonRoot = common; Mode = mode; ObsVersion = version;
            BundleRoot = mode == "normal" ? Path.Combine(common, "obs-studio", "plugins", "glowbox") :
                Path.Combine(root, "plugins", "glowbox");
        }
    }
    internal static bool HasPortableMarker(string folder) {
        foreach (string marker in new[] { "portable_mode", "obs_portable_mode", "portable_mode.txt", "obs_portable_mode.txt" })
            if (File.Exists(Path.Combine(folder, marker)) || Directory.Exists(Path.Combine(folder, marker))) return true;
        return false;
    }
    internal static Version MinimumObsVersion() {
        var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("obs-api.txt");
        if (stream == null) {
            throw new IOException("The installer is missing its OBS version information.");
        }
        using (stream) using (var reader = new StreamReader(stream)) {
            Version version;
            string text = reader.ReadToEnd().Trim();
            if (!Version.TryParse(text, out version) || version.Build >= 0)
                throw new IOException("The installer's OBS version information is invalid.");
            return version;
        }
    }
    static Version ReadObsVersion(string folder) {
        var exe = FileVersionInfo.GetVersionInfo(Path.Combine(folder, "bin", "64bit", "obs64.exe"));
        var dll = FileVersionInfo.GetVersionInfo(Path.Combine(folder, "bin", "64bit", "obs.dll"));
        if (exe.ProductName != "OBS Studio" || dll.ProductName != "OBS Studio" ||
            exe.FileMajorPart != dll.FileMajorPart || exe.FileMinorPart != dll.FileMinorPart || exe.FileMajorPart <= 0)
            throw new IOException("Choose a complete OBS Studio installation.");
        return new Version(exe.FileMajorPart, exe.FileMinorPart, Math.Max(0, exe.FileBuildPart));
    }
    internal static InstallTarget CreateTarget(string folder, bool portable, string common, Version minimum) {
        string root = NormalizeFolder(folder);
        CheckPath(root);
        if (!IsObsFolder(root) || !IsX64Executable(Path.Combine(root, "bin", "64bit", "obs.dll")))
            throw new IOException("Choose an x64 OBS Studio installation.");
        Version version = ReadObsVersion(root);
        if (version == null || version.Major < minimum.Major ||
            (version.Major == minimum.Major && version.Minor < minimum.Minor))
            throw new IOException("This build requires OBS Studio " + minimum + " or newer (x64).");
        portable = portable || HasPortableMarker(root);
        string commonRoot = portable ? null : NormalizeFolder(common);
        string mode = !portable ? "normal" : version.Major >= 33 ? "portable33" : "portable32";
        var target = new InstallTarget(root, commonRoot, mode, version);
        if (!portable) {
            CheckPath(commonRoot);
            if (Within(target.BundleRoot, root) || Within(root, target.BundleRoot))
                throw new IOException("The shared plugin folder and OBS folder must be separate.");
        }
        ValidateFiles(target, Files(target, mode, Guid.Empty.ToString("N"), null, null));
        return target;
    }
    internal static InstallTarget CreateTarget(string folder, bool portable) {
        return CreateTarget(folder, portable, Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            MinimumObsVersion());
    }
    static bool AnyObsRunning() {
        foreach (string name in new[] { "obs64", "obs32" }) {
            var processes = Process.GetProcessesByName(name);
            bool running = processes.Length != 0;
            foreach (var process in processes) process.Dispose();
            if (running) return true;
        }
        return false;
    }
    static void RequireObsClosed(bool running) {
        if (running) throw new IOException("Close OBS before installing Glowbox.");
    }
    static bool Within(string path, string root) {
        return String.Equals(path, root, StringComparison.OrdinalIgnoreCase) ||
            path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetLongPathName(string path, StringBuilder result, uint capacity);
    [StructLayout(LayoutKind.Sequential)]
    struct FileIdentity {
        internal uint Attributes;
        internal System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write;
        internal uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(Microsoft.Win32.SafeHandles.SafeFileHandle file, out FileIdentity info);
    // Check every existing ancestor, including the destination itself. Hard-linked
    // destination files are rejected so no second path can alias a transaction file.
    static void CheckPath(string path) {
        path = Path.GetFullPath(path);
        if (path.StartsWith(@"\\", StringComparison.Ordinal))
            throw new IOException("Choose a local folder without linked paths.");
        string remainder = path.Substring(Path.GetPathRoot(path).Length);
        foreach (string part in remainder.Split(Path.DirectorySeparatorChar))
            if (part.EndsWith(".", StringComparison.Ordinal) || part.EndsWith(" ", StringComparison.Ordinal) || part.Contains(":"))
                throw new IOException("Ambiguous paths are not supported.");
        for (string current = path; !String.IsNullOrEmpty(current); current = Path.GetDirectoryName(current)) {
            FileAttributes attributes;
            try { attributes = File.GetAttributes(current); }
            catch (FileNotFoundException) { continue; }
            catch (DirectoryNotFoundException) { continue; }
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Linked installation paths are not supported: " + current);
            if (File.Exists(current)) using (var file = File.OpenRead(current)) {
                FileIdentity identity;
                if (!GetFileInformationByHandle(file.SafeFileHandle, out identity) || identity.Links != 1)
                    throw new IOException("Aliased installation files are not supported: " + current);
            }
        }
    }
    sealed class PendingFile {
        internal readonly string Key, Destination, Staged, Backup;
        internal readonly byte[] Payload;
        internal bool Existed;
        internal string PreviousHash = "-";
        internal PendingFile(string key, string destination, byte[] payload, string token) {
            Key = key; Destination = Path.GetFullPath(destination); Payload = payload;
            Staged = Destination + "." + token + ".new";
            Backup = Destination + "." + token + ".bak";
        }
    }
    static string[] OldPluginBundles(InstallTarget target) {
        string local = Path.Combine(target.ObsRoot, "plugins", "shoebox");
        return target.Portable ? new[] { local } : new[] {
            local, Path.Combine(target.CommonRoot, "obs-studio", "plugins", "shoebox")
        };
    }
    static PendingFile[] Files(InstallTarget target, string mode, string token, byte[] dll, byte[] effect, byte[][] images = null) {
        string legacyDll = Path.Combine(target.ObsRoot, "obs-plugins", "64bit", "glowbox.dll");
        string legacyData = Path.Combine(target.ObsRoot, "data", "obs-plugins", "glowbox", "compositor.effect");
        string bundle = mode == "normal" ? Path.Combine(target.CommonRoot, "obs-studio", "plugins", "glowbox") :
            Path.Combine(target.ObsRoot, "plugins", "glowbox");
        var modern = new[] {
            new PendingFile("modern-dll", Path.Combine(bundle, "glowbox.dll"), mode == "portable32" ? null : dll, token),
            new PendingFile("compat-dll", Path.Combine(bundle, "bin", "64bit", "glowbox.dll"), mode == "portable32" ? null : dll, token),
            new PendingFile("bundle-data", Path.Combine(bundle, "data", "compositor.effect"), mode == "portable32" ? null : effect, token)
        };
        var legacy = new[] {
            new PendingFile("legacy-dll", legacyDll, mode == "portable32" ? dll : null, token),
            new PendingFile("legacy-data", legacyData, mode == "portable32" ? effect : null, token)
        };
        var files = new List<PendingFile>(mode == "portable32" ?
            new[] { legacy[0], legacy[1], modern[0], modern[1], modern[2] } :
            new[] { modern[0], modern[1], modern[2], legacy[0], legacy[1] });
        files.Add(new PendingFile("old-legacy-dll", Path.Combine(target.ObsRoot, "obs-plugins", "64bit", "shoebox.dll"), null, token));
        files.Add(new PendingFile("old-legacy-data", Path.Combine(target.ObsRoot, "data", "obs-plugins", "shoebox", "compositor.effect"), null, token));
        int index = 0;
        foreach (string oldBundle in OldPluginBundles(target)) {
            string key = "old-bundle-" + index++;
            files.Add(new PendingFile(key + "-dll", Path.Combine(oldBundle, "shoebox.dll"), null, token));
            files.Add(new PendingFile(key + "-compat", Path.Combine(oldBundle, "bin", "64bit", "shoebox.dll"), null, token));
            files.Add(new PendingFile(key + "-data", Path.Combine(oldBundle, "data", "compositor.effect"), null, token));
        }
        for (int i = 0; i < LoadingImages.Length; ++i) {
            byte[] image = images == null ? null : images[i];
            files.Add(new PendingFile("bundle-image-" + i, Path.Combine(bundle, "data", LoadingImages[i]), mode == "portable32" ? null : image, token));
            files.Add(new PendingFile("legacy-image-" + i, Path.Combine(target.ObsRoot, "data", "obs-plugins", "glowbox", LoadingImages[i]), mode == "portable32" ? image : null, token));
        }
        return files.ToArray();
    }
    static void ValidateFiles(InstallTarget target, PendingFile[] files) {
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in files) {
            bool local = Within(file.Destination, target.ObsRoot);
            bool shared = !target.Portable && Within(file.Destination, target.BundleRoot);
            if (!target.Portable)
                foreach (string oldBundle in OldPluginBundles(target))
                    shared |= Within(file.Destination, oldBundle);
            if ((!local && !shared) || !paths.Add(file.Destination))
                throw new IOException("Installation paths overlap or escape their destination.");
            foreach (string path in new[] { file.Destination, file.Staged, file.Backup, file.Backup + ".restore" }) CheckPath(path);
            if (Directory.Exists(file.Destination)) throw new IOException("An installation file is a folder: " + file.Destination);
            if (File.Exists(file.Destination) && (File.GetAttributes(file.Destination) & FileAttributes.ReadOnly) != 0)
                throw new IOException("An installation file is read-only: " + file.Destination);
        }
    }
    static string Journal(InstallTarget target) { return Path.Combine(target.ObsRoot, ".glowbox-install.pending"); }
    static string Owner(InstallTarget target) { return Path.Combine(target.BundleRoot, ".glowbox-install.owner"); }
    static string Hash(byte[] value) {
        using (var hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(value)).Replace("-", "");
    }
    static string PathHash(string value) { return Hash(Encoding.UTF8.GetBytes(value.ToUpperInvariant())); }
    static void DurableWrite(string path, byte[] payload) {
        CheckPath(path);
        using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
            stream.Write(payload, 0, payload.Length); stream.Flush(true);
        }
    }
    static void Verify(string path, byte[] payload) {
        CheckPath(path);
        if (Hash(File.ReadAllBytes(path)) != Hash(payload)) throw new IOException("File verification failed: " + path);
    }
    static void DeleteChecked(string path) { CheckPath(path); if (File.Exists(path)) File.Delete(path); }
    static void Cleanup(PendingFile[] files) {
        foreach (var file in files) foreach (string path in new[] { file.Staged, file.Backup, file.Backup + ".restore" }) TryDelete(path);
    }
    static void TryDelete(string path) {
        try { DeleteChecked(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
    static void CommitFile(PendingFile file) {
        CheckPath(file.Destination); CheckPath(file.Staged);
        if (File.Exists(file.Destination)) File.Replace(file.Staged, file.Destination, null);
        else File.Move(file.Staged, file.Destination);
    }
    static string Record(InstallTarget target, string mode, string token, string state, PendingFile[] files) {
        var text = new StringBuilder("GLOWBOX-INSTALL-3\n" + mode + "\n" + token + "\n" +
            PathHash(target.ObsRoot) + "\n" + (mode == "normal" ? PathHash(target.CommonRoot) : "-") + "\n" + state + "\n");
        foreach (var file in files) text.Append(file.Key + " " + (file.Existed ? "1" : "0") + " " + file.PreviousHash + "\n");
        return text.ToString();
    }
    static void PublishJournal(InstallTarget target, string text, string token) {
        string staged = Journal(target) + "." + token + ".new";
        DurableWrite(staged, Encoding.UTF8.GetBytes(text));
        CheckPath(Journal(target));
        if (File.Exists(Journal(target))) File.Replace(staged, Journal(target), null);
        else File.Move(staged, Journal(target));
    }
    static string[] ReadOwner(InstallTarget target) {
        string path = Owner(target); CheckPath(path);
        if (!File.Exists(path)) return null;
        string[] lines = File.ReadAllLines(path); Guid token;
        if (lines.Length != 4 || lines[0] != "GLOWBOX-OWNER-3" || !Guid.TryParseExact(lines[1], "N", out token) ||
            lines[2] != PathHash(target.ObsRoot) || lines[3] != PathHash(target.CommonRoot))
            throw new IOException("An interrupted update belongs to another OBS folder, or its record is damaged. Select the original OBS folder to recover it.");
        return lines;
    }
    static void Restore(PendingFile[] files) {
        // Verify every backup before modifying any destination. Keep all backups
        // until the journal is removed so an interrupted rollback can be repeated.
        foreach (var file in files) if (file.Existed) {
            CheckPath(file.Backup);
            if (!File.Exists(file.Backup) || Hash(File.ReadAllBytes(file.Backup)) != file.PreviousHash)
                throw new IOException("An installation backup is missing or damaged: " + file.Backup);
        }
        string errors = "";
        foreach (var file in files) try {
            if (file.Existed) {
                string restore = file.Backup + ".restore"; TryDelete(restore);
                byte[] bytes = File.ReadAllBytes(file.Backup); DurableWrite(restore, bytes); Verify(restore, bytes);
                CheckPath(file.Destination);
                if (File.Exists(file.Destination)) File.Replace(restore, file.Destination, null);
                else File.Move(restore, file.Destination);
            } else DeleteChecked(file.Destination);
        } catch (Exception error) { errors += "\n" + file.Destination + ": " + error.Message; }
        if (errors.Length != 0) throw new IOException("Some files couldn't be restored. Keep OBS closed and run the installer again. Backups were retained." + errors);
    }
    static void Recover(InstallTarget target) {
        string path = Journal(target); CheckPath(path);
        string[] owner = target.Portable ? null : ReadOwner(target);
        if (!File.Exists(path)) {
            if (owner != null) { Cleanup(Files(target, "normal", owner[1], null, null)); DeleteChecked(Owner(target)); }
            return;
        }
        string[] lines = File.ReadAllLines(path); Guid token;
        if (lines.Length == 3 && Guid.TryParseExact(lines[0], "N", out token) &&
            (lines[1] == "0" || lines[1] == "1") && (lines[2] == "0" || lines[2] == "1")) {
            var old = new[] {
                new PendingFile("legacy-dll", Path.Combine(target.ObsRoot,"obs-plugins","64bit","shoebox.dll"),null,lines[0]),
                new PendingFile("legacy-data",Path.Combine(target.ObsRoot,"data","obs-plugins","shoebox","compositor.effect"),null,lines[0])
            };
            ValidateFiles(target, old);
            for (int i = 0; i < old.Length; ++i) {
                old[i].Existed = lines[i + 1] == "1";
                if (old[i].Existed) { CheckPath(old[i].Backup); old[i].PreviousHash = Hash(File.ReadAllBytes(old[i].Backup)); }
            }
            Restore(old); DeleteChecked(path); Cleanup(old); return;
        }
        if (lines.Length < 6 || lines[0] != "GLOWBOX-INSTALL-3" ||
            (lines[1] != "normal" && lines[1] != "portable32" && lines[1] != "portable33") ||
            !Guid.TryParseExact(lines[2], "N", out token) || lines[3] != PathHash(target.ObsRoot) ||
            (lines[5] != "prepared" && lines[5] != "committing"))
            throw new IOException("The installation record is damaged. No files were changed.");
        string mode = lines[1];
        if (mode == "normal" && target.Portable)
            throw new IOException("Recover the previous normal installation before switching to portable mode.");
        if (lines[4] != (mode == "normal" ? PathHash(target.CommonRoot) : "-"))
            throw new IOException("The installation record belongs to a different shared folder.");
        var files = Files(target, mode, lines[2], null, null);
        // Earlier records predate the six loading-image destinations.
        if (lines.Length == 6 + files.Length - 6)
            Array.Resize(ref files, files.Length - 6);
        if (lines.Length != 6 + files.Length)
            throw new IOException("The installation record is damaged. No files were changed.");
        ValidateFiles(target, files);
        for (int i = 0; i < files.Length; ++i) {
            string[] parts = lines[i + 6].Split(' ');
            if (parts.Length != 3 || parts[0] != files[i].Key || (parts[1] != "0" && parts[1] != "1") ||
                (parts[1] == "0" ? parts[2] != "-" : !System.Text.RegularExpressions.Regex.IsMatch(parts[2], "^[0-9A-F]{64}$")))
                throw new IOException("The installation record is damaged. No files were changed.");
            files[i].Existed = parts[1] == "1"; files[i].PreviousHash = parts[2];
        }
        if (mode == "normal" && lines[5] == "committing" && (owner == null || owner[1] != lines[2]))
            throw new IOException("The shared installation record is missing or damaged.");
        if (lines[5] == "committing") Restore(files);
        DeleteChecked(path);
        if (mode == "normal" && owner != null && owner[1] == lines[2]) DeleteChecked(Owner(target));
        Cleanup(files);
        TryDelete(Journal(target) + "." + lines[2] + ".new");
    }
    internal static void InstallPayloads(InstallTarget target, byte[] dll, byte[] effect) {
        if (target == null || dll == null || effect == null || dll.Length == 0 || effect.Length == 0)
            throw new ArgumentException("Missing installation payload.");
        RequireObsClosed(AnyObsRunning());
        CheckPath(target.ObsRoot);
        var images = new byte[LoadingImages.Length][];
        for (int i = 0; i < images.Length; ++i) images[i] = ReadResource(LoadingImages[i]);
        var initial = Files(target, target.Mode, Guid.Empty.ToString("N"), dll, effect, images); ValidateFiles(target, initial);
        FileStream sharedLock = null;
        var oldLocks = new List<FileStream>();
        try {
            if (!target.Portable) {
                CheckPath(target.BundleRoot); Directory.CreateDirectory(target.BundleRoot); CheckPath(target.BundleRoot);
                string sharedPath = Path.Combine(target.BundleRoot, ".glowbox-install.lock"); CheckPath(sharedPath);
                sharedLock = new FileStream(sharedPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose);
            }
            foreach (string oldBundle in OldPluginBundles(target)) {
                CheckPath(oldBundle);
                if (!Directory.Exists(oldBundle)) continue;
                string oldLock = Path.Combine(oldBundle, ".glowbox-install.lock"); CheckPath(oldLock);
                oldLocks.Add(new FileStream(oldLock, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose));
                string oldOwner = Path.Combine(oldBundle, ".glowbox-install.owner"); CheckPath(oldOwner);
                if (File.Exists(oldOwner))
                    throw new IOException("Finish the previous plugin installation before installing Glowbox.");
            }
            string lockPath = Path.Combine(target.ObsRoot, ".glowbox-install.lock"); CheckPath(lockPath);
            using (var rootLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose)) {
                RequireObsClosed(AnyObsRunning());
                Recover(target);
                InstallLocked(target, dll, effect, images);
            }
        } finally {
            foreach (var oldLock in oldLocks) oldLock.Dispose();
            if (sharedLock != null) sharedLock.Dispose();
        }
    }
    static void InstallLocked(InstallTarget target, byte[] dll, byte[] effect, byte[][] images) {
        string token = Guid.NewGuid().ToString("N");
        var files = Files(target, target.Mode, token, dll, effect, images); ValidateFiles(target, files);
        bool journalPublished = false;
        try {
            // Prepared recovery only removes this token's staging files. Publish
            // before writing payloads so a crash cannot orphan staged DLLs/backups.
            PublishJournal(target, Record(target, target.Mode, token, "prepared", files), token);
            journalPublished = true;
            foreach (var file in files) {
                if (file.Payload != null) {
                    CheckPath(Path.GetDirectoryName(file.Destination)); Directory.CreateDirectory(Path.GetDirectoryName(file.Destination));
                    DurableWrite(file.Staged, file.Payload); Verify(file.Staged, file.Payload);
                }
                file.Existed = File.Exists(file.Destination);
                if (file.Existed) {
                    byte[] previous = File.ReadAllBytes(file.Destination); file.PreviousHash = Hash(previous);
                    DurableWrite(file.Backup, previous); Verify(file.Backup, previous);
                }

            }
            PublishJournal(target, Record(target, target.Mode, token, "prepared", files), token);
            if (!target.Portable)
                DurableWrite(Owner(target), Encoding.UTF8.GetBytes("GLOWBOX-OWNER-3\n" + token + "\n" + PathHash(target.ObsRoot) + "\n" + PathHash(target.CommonRoot)));
            PublishJournal(target, Record(target, target.Mode, token, "committing", files), token);
            // Commit all new files before removing obsolete paths.
            var commitOrder = new List<PendingFile>();
            foreach (var pending in files) if (pending.Payload != null) commitOrder.Add(pending);
            foreach (var pending in files) if (pending.Payload == null) commitOrder.Add(pending);
            for (int i = 0; i < commitOrder.Count; ++i) {
                RequireObsClosed(AnyObsRunning());
                var file = commitOrder[i];
                if (file.Payload != null) { CommitFile(file); Verify(file.Destination, file.Payload); }
                else {
                    foreach (var installed in files) if (installed.Payload != null) Verify(installed.Destination, installed.Payload);
                    DeleteChecked(file.Destination);
                }
            }
            DeleteChecked(Journal(target)); journalPublished = false;
            // The payload transaction is committed once its journal is gone.
            // A locked cleanup record must not report a successful update as rollback.
            if (!target.Portable) TryDelete(Owner(target));
        } catch (Exception error) {
            if (journalPublished) {
                try { Recover(target); journalPublished = false; }
                catch (Exception rollback) { throw new IOException("Installation failed. " + rollback.Message + "\n" + error.Message, error); }
            }
            throw new IOException("Couldn't install Glowbox. Previous files were retained or restored.\n" + error.Message, error);
        } finally {
            TryDelete(Journal(target) + "." + token + ".new");
            if (!journalPublished) Cleanup(files);
        }
    }

}
