// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceEditor: picks each fighter's hit grunts and KO cries for the game's
// hit voices (recomp-runtime/hit_voice.c) and tunes how often they play.
//
// It works on private\doa4-voice-all\<prefix>\selections beside this exe: WAV
// files of any rate, channel count and bit depth are converted to the 48 kHz
// mono 16-bit the game reads and named <fighter>_HIT_LV1_NN.wav (light),
// _HIT_LV2_ (medium), _HIT_LV3_ (heavy) or <fighter>_KO_NN.wav. Removed clips
// move to selections\removed. The settings go to selections\voice.ini. The
// game picks up every change within a second, so it can run alongside.
// Save mapping writes the picks to tools\doa4_voice_selections.json (through
// tools\import_doa4_voices.py --save-selections, which needs Python, ffmpeg and
// your DOA4 snd.afs), so other installs build the same selections from their
// own disc. The mapping holds no audio.
//
// Built with the C# compiler every Windows ships (C# 5):
//   csc /target:winexe /out:VoiceEditor.exe tools\voice_editor.cs
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Media;
using System.Text;
using System.Diagnostics;
using System.Threading;
using System.Windows.Forms;

static class VoiceEditorProgram
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new VoiceEditor());
    }
}

class Fighter
{
    public string Prefix, Name, Stem;
    public Fighter(string prefix, string name, string stem) { Prefix = prefix; Name = name; Stem = stem; }
    public override string ToString() { return Name; }
}

class Pool
{
    public string Title, Tag; // Tag: HIT_LV1, HIT_LV2, HIT_LV3 or KO
    public ListBox List;
    public Pool(string title, string tag) { Title = title; Tag = tag; }
}

class VoiceEditor : Form
{
    static readonly Fighter[] Fighters = {
        new Fighter("aya", "Ayane", "ayane"), new Fighter("bas", "Bass", "bass"),
        new Fighter("bay", "Bayman", "bayman"), new Fighter("ein", "Ein", "ein"),
        new Fighter("gen", "Gen Fu", "genfu"), new Fighter("hel", "Helena", "helena"),
        new Fighter("kor", "Hitomi", "hitomi"), new Fighter("jan", "Jann Lee", "jannlee"),
        new Fighter("kas", "Kasumi", "kasumi"), new Fighter("lei", "Leifang", "leifang"),
        new Fighter("leo", "Leon", "leon"), new Fighter("ryu", "Ryu Hayabusa", "hayabusa"),
        new Fighter("tng", "Tengu", "tengu"), new Fighter("tin", "Tina", "tina"),
        new Fighter("zac", "Zack", "zack"),
    };

    readonly Pool[] pools = {
        new Pool("Light hits", "HIT_LV1"), new Pool("Medium hits", "HIT_LV2"),
        new Pool("Heavy hits", "HIT_LV3"), new Pool("KO cries", "KO"),
    };

    string root;
    readonly ListBox fighters = new ListBox();
    readonly NumericUpDown mediumFrom = Number(1, 255, 0, 1);
    readonly NumericUpDown heavyFrom = Number(1, 255, 0, 1);
    readonly NumericUpDown meterLow = Number(1, 500, 0, 1);
    readonly NumericUpDown meterHigh = Number(1, 500, 0, 1);
    readonly NumericUpDown gap = Number(0.1m, 10m, 1, 0.1m);
    readonly NumericUpDown volume = Number(-30m, 10m, 1, 0.5m);
    readonly CheckBox heavyAlways = new CheckBox();
    readonly Label status = new Label();
    readonly SoundPlayer player = new SoundPlayer();
    bool loading;

    static NumericUpDown Number(decimal min, decimal max, int places, decimal step)
    {
        return new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = places, Increment = step, Width = 70 };
    }

    public VoiceEditor()
    {
        Text = "DOA2U Voice Editor";
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(980, 640);
        MinimumSize = new Size(860, 560);
        StartPosition = FormStartPosition.CenterScreen;

        root = FindRoot();

        var left = new Panel { Dock = DockStyle.Left, Width = 170, Padding = new Padding(8) };
        var fightersLabel = new Label { Text = "Fighter", Dock = DockStyle.Top, Height = 20 };
        fighters.Dock = DockStyle.Fill;
        fighters.IntegralHeight = false;
        foreach (var f in Fighters) fighters.Items.Add(f);
        fighters.SelectedIndexChanged += delegate { LoadFighter(); };
        left.Controls.Add(fighters);
        left.Controls.Add(fightersLabel);
        var save = Button("Save mapping...", delegate { SaveMapping(); });
        save.AutoSize = false;
        save.Dock = DockStyle.Bottom;
        save.Height = 30;
        new ToolTip().SetToolTip(save, "Write every fighter's picks and settings to tools\\doa4_voice_selections.json,\n" +
            "so other installs build the same selections from their own DOA4 disc.");
        left.Controls.Add(save);

        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 2, Padding = new Padding(4) };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        grid.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        grid.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        for (int i = 0; i < pools.Length; ++i) grid.Controls.Add(PoolBox(pools[i]), i % 2, i / 2);

        var settings = SettingsBox();
        status.Dock = DockStyle.Bottom;
        status.Height = 22;
        status.Padding = new Padding(8, 3, 8, 0);

        Controls.Add(grid);
        Controls.Add(settings);
        Controls.Add(left);
        Controls.Add(status);

        if (fighters.Items.Count > 0) fighters.SelectedIndex = 0;
        if (root == null) Say("No doa4-voice-all folder chosen; restart the editor to choose one.");
    }

    // private\doa4-voice-all beside the exe, or one the player picks.
    static string FindRoot()
    {
        var here = AppDomain.CurrentDomain.BaseDirectory;
        var candidate = Path.Combine(Path.Combine(here, "private"), "doa4-voice-all");
        if (Directory.Exists(candidate)) return candidate;
        using (var dialog = new FolderBrowserDialog())
        {
            dialog.Description = "Choose the doa4-voice-all folder (made by tools\\import_doa4_voices.py, in private).";
            dialog.SelectedPath = here;
            if (dialog.ShowDialog() == DialogResult.OK) return dialog.SelectedPath;
        }
        return null;
    }

    // The repository: private\doa4-voice-all's grandparent.
    string RepoRoot
    {
        get
        {
            if (root == null) return null;
            var up = Directory.GetParent(root);
            up = up == null ? null : up.Parent;
            return up == null ? null : up.FullName;
        }
    }

    void SaveMapping()
    {
        var repo = RepoRoot;
        var script = repo == null ? null : Path.Combine(Path.Combine(repo, "tools"), "import_doa4_voices.py");
        if (script == null || !File.Exists(script))
        {
            MessageBox.Show(this, "tools\\import_doa4_voices.py was not found beside the private folder.", Text);
            return;
        }
        var remembered = Path.Combine(root, "snd-afs.txt");
        string afs = File.Exists(remembered) ? File.ReadAllText(remembered).Trim() : null;
        if (afs == null || !File.Exists(afs))
        {
            using (var dialog = new OpenFileDialog())
            {
                dialog.Title = "Choose snd.afs from your extracted DOA4 disc";
                dialog.Filter = "snd.afs|snd.afs|AFS archives (*.afs)|*.afs";
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                afs = dialog.FileName;
                File.WriteAllText(remembered, afs);
            }
        }
        Say("Saving the mapping: decoding DOA4's voice banks to match your clips (a minute or two)...");
        UseWaitCursor = true;
        Enabled = false;
        var args = "\"" + script + "\" \"" + afs + "\" --save-selections --out \"" + root + "\"";
        var worker = new Thread(delegate()
        {
            string output;
            int code = -1;
            try { output = RunPython(repo, args, out code); }
            catch (Exception e) { output = e.Message; }
            BeginInvoke((MethodInvoker)delegate
            {
                Enabled = true;
                UseWaitCursor = false;
                Say(code == 0 ? "Mapping saved to tools\\doa4_voice_selections.json." : "Saving the mapping failed.");
                MessageBox.Show(this, output.Trim(), code == 0 ? "Mapping saved" : "Saving the mapping failed");
            });
        });
        worker.IsBackground = true;
        worker.Start();
    }

    // python, else the py launcher; stdout and stderr together.
    static string RunPython(string directory, string args, out int code)
    {
        foreach (var exe in new[] { "python", "py" })
        {
            var info = new ProcessStartInfo(exe, (exe == "py" ? "-3 " : "") + args)
            {
                WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            Process process;
            try { process = Process.Start(info); }
            catch (System.ComponentModel.Win32Exception) { continue; }
            var errors = new StringBuilder();
            process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e)
            {
                if (e.Data != null && e.Data.IndexOf("Invalid data", StringComparison.Ordinal) < 0 &&
                    e.Data.IndexOf("would have to skip", StringComparison.Ordinal) < 0) errors.AppendLine(e.Data);
            };
            process.BeginErrorReadLine();
            var text = process.StandardOutput.ReadToEnd();
            process.WaitForExit();
            code = process.ExitCode;
            return text + errors;
        }
        code = -1;
        return "Python was not found. Install Python 3 and make sure it is on PATH.";
    }

    Fighter Current { get { return fighters.SelectedItem as Fighter; } }

    string Selections
    {
        get { return root == null || Current == null ? null : Path.Combine(Path.Combine(root, Current.Prefix), "selections"); }
    }

    Control PoolBox(Pool pool)
    {
        var box = new GroupBox { Text = pool.Title, Dock = DockStyle.Fill, Padding = new Padding(6) };
        pool.List = new ListBox { Dock = DockStyle.Fill, IntegralHeight = false, SelectionMode = SelectionMode.MultiExtended };
        pool.List.DoubleClick += delegate { Play(pool); };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Play", null, delegate { Play(pool); });
        foreach (var other in pools)
        {
            if (other == pool) continue;
            var target = other;
            menu.Items.Add("Move to " + target.Title, null, delegate { MoveClips(pool, target); });
        }
        menu.Items.Add("Remove", null, delegate { Remove(pool); });
        pool.List.ContextMenuStrip = menu;

        var buttons = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 34, FlowDirection = FlowDirection.LeftToRight };
        buttons.Controls.Add(Button("Add...", delegate { Add(pool); }));
        buttons.Controls.Add(Button("Play", delegate { Play(pool); }));
        buttons.Controls.Add(Button("Remove", delegate { Remove(pool); }));
        box.Controls.Add(pool.List);
        box.Controls.Add(buttons);
        return box;
    }

    static Button Button(string text, EventHandler click)
    {
        var button = new Button { Text = text, AutoSize = true };
        button.Click += click;
        return button;
    }

    Control SettingsBox()
    {
        var box = new GroupBox { Text = "How this fighter's voice plays", Dock = DockStyle.Bottom, Height = 150, Padding = new Padding(8) };
        var table = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 4, RowCount = 4, AutoSize = false };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 230));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 170));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 230));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        var tips = new ToolTip { AutoPopDelay = 20000 };

        Row(table, 0, 0, "Medium hit from base damage", mediumFrom, tips,
            "A move whose damage before combo scaling is at least this uses a medium grunt.");
        Row(table, 1, 0, "Heavy hit from base damage", heavyFrom, tips,
            "A move whose damage before combo scaling is at least this uses a heavy grunt.");
        heavyAlways.Text = "Heavy hits always grunt";
        heavyAlways.AutoSize = true;
        tips.SetToolTip(heavyAlways, "When on, a heavy hit grunts whenever the gap below allows, whatever the meter says.");
        table.Controls.Add(heavyAlways, 0, 2);
        table.SetColumnSpan(heavyAlways, 2);

        var meter = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0) };
        meter.Controls.Add(meterLow);
        meter.Controls.Add(new Label { Text = "to", AutoSize = true, Padding = new Padding(0, 6, 0, 0) });
        meter.Controls.Add(meterHigh);
        Row(table, 0, 2, "Damage taken between grunts", meter, tips,
            "Each hit adds its base damage to a meter; at a random point in this range the fighter grunts and the meter empties. Lower numbers: more grunts.");
        Row(table, 1, 2, "Least time between grunts (seconds)", gap, tips,
            "No two grunts from this fighter come closer together than this.");
        Row(table, 2, 2, "Volume (dB, on top of PC Settings)", volume, tips,
            "Makes this fighter's clips louder or quieter than the Hit Voices row's level.");

        var help = new Label
        {
            Text = "Changes save at once; a running game picks them up within a second. Right-click a clip to move it between lists.",
            AutoSize = true, ForeColor = SystemColors.GrayText, Padding = new Padding(0, 6, 0, 0),
        };
        table.Controls.Add(help, 0, 3);
        table.SetColumnSpan(help, 4);
        box.Controls.Add(table);

        EventHandler save = delegate { if (!loading) SaveSettings(); };
        foreach (var number in new[] { mediumFrom, heavyFrom, meterLow, meterHigh, gap, volume }) number.ValueChanged += save;
        heavyAlways.CheckedChanged += save;
        return box;
    }

    static void Row(TableLayoutPanel table, int row, int column, string text, Control control, ToolTip tips, string tip)
    {
        var label = new Label { Text = text, AutoSize = true, Padding = new Padding(0, 6, 0, 0) };
        tips.SetToolTip(label, tip);
        tips.SetToolTip(control, tip);
        table.Controls.Add(label, column, row);
        table.Controls.Add(control, column + 1, row);
    }

    void Say(string text) { status.Text = text; }

    // ---- Clips --------------------------------------------------------

    // The pool a file's name puts it in, as the game reads it.
    static string KindOf(string name)
    {
        var upper = name.ToUpperInvariant();
        if (!upper.EndsWith(".WAV")) return null;
        int at = upper.IndexOf("HIT_LV");
        if (at >= 0) return at + 6 < upper.Length && upper[at + 6] >= '1' && upper[at + 6] <= '3' ? "HIT_LV" + upper[at + 6] : null;
        for (at = upper.IndexOf("KO"); at >= 0; at = upper.IndexOf("KO", at + 1))
        {
            bool before = at == 0 || "_- ".IndexOf(upper[at - 1]) >= 0;
            bool after = at + 2 < upper.Length && "_-. ".IndexOf(upper[at + 2]) >= 0;
            if (before && after) return "KO";
        }
        return null;
    }

    void LoadFighter()
    {
        loading = true;
        try
        {
            foreach (var pool in pools) pool.List.Items.Clear();
            var folder = Selections;
            if (folder != null && Directory.Exists(folder))
            {
                var files = Directory.GetFiles(folder, "*.wav");
                Array.Sort(files, StringComparer.OrdinalIgnoreCase);
                foreach (var file in files)
                {
                    var kind = KindOf(Path.GetFileName(file));
                    foreach (var pool in pools) if (pool.Tag == kind) pool.List.Items.Add(Path.GetFileName(file));
                }
            }
            LoadSettings();
            int count = 0;
            foreach (var pool in pools) count += pool.List.Items.Count;
            Say(Current == null ? "" : Current.Name + ": " + count + " clips" +
                (count == 0 ? " (silent in game until you add some)" : "") + (folder != null ? "  -  " + folder : ""));
        }
        finally { loading = false; }
    }

    void Add(Pool pool)
    {
        if (Selections == null) return;
        using (var dialog = new OpenFileDialog())
        {
            dialog.Title = "Add " + pool.Title.ToLowerInvariant() + " for " + Current.Name;
            dialog.Filter = "WAV files (*.wav)|*.wav";
            dialog.Multiselect = true;
            var decoded = Path.Combine(root, Current.Prefix);
            if (Directory.Exists(decoded)) dialog.InitialDirectory = decoded;
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            Directory.CreateDirectory(Selections);
            int added = 0;
            var failed = new List<string>();
            foreach (var source in dialog.FileNames)
            {
                try
                {
                    var pcm = Wave.ReadMono48k(source);
                    Wave.Write(NextName(pool), pcm);
                    ++added;
                }
                catch (Exception e) { failed.Add(Path.GetFileName(source) + ": " + e.Message); }
            }
            LoadFighter();
            if (failed.Count > 0) MessageBox.Show(this, "Not added:\n" + string.Join("\n", failed.ToArray()), Text);
            Say("Added " + added + " clip" + (added == 1 ? "" : "s") + " to " + Current.Name + "'s " + pool.Title.ToLowerInvariant() + ".");
        }
    }

    // <stem>_<tag>_NN.wav with the first free NN.
    string NextName(Pool pool)
    {
        for (int n = 1; ; ++n)
        {
            var path = Path.Combine(Selections, Current.Stem + "_" + pool.Tag + "_" + n.ToString("00") + ".wav");
            if (!File.Exists(path)) return path;
        }
    }

    List<string> Chosen(Pool pool)
    {
        var chosen = new List<string>();
        foreach (var item in pool.List.SelectedItems) chosen.Add(Path.Combine(Selections, (string)item));
        return chosen;
    }

    void Play(Pool pool)
    {
        var chosen = Chosen(pool);
        if (chosen.Count == 0 && pool.List.Items.Count > 0) chosen.Add(Path.Combine(Selections, (string)pool.List.Items[0]));
        if (chosen.Count == 0) return;
        try
        {
            player.SoundLocation = chosen[0];
            player.Load();
            player.Play();
        }
        catch (Exception e) { Say("Can't play " + Path.GetFileName(chosen[0]) + ": " + e.Message); }
    }

    void Remove(Pool pool)
    {
        var chosen = Chosen(pool);
        if (chosen.Count == 0) return;
        player.Stop();
        var removed = Path.Combine(Selections, "removed");
        Directory.CreateDirectory(removed);
        foreach (var file in chosen)
        {
            var target = Path.Combine(removed, Path.GetFileName(file));
            for (int n = 2; File.Exists(target); ++n)
                target = Path.Combine(removed, Path.GetFileNameWithoutExtension(file) + " (" + n + ").wav");
            File.Move(file, target);
        }
        LoadFighter();
        Say("Moved " + chosen.Count + " clip" + (chosen.Count == 1 ? "" : "s") + " to " + removed + ".");
    }

    void MoveClips(Pool from, Pool to)
    {
        var chosen = Chosen(from);
        if (chosen.Count == 0) return;
        player.Stop();
        foreach (var file in chosen) File.Move(file, NextName(to));
        LoadFighter();
        Say("Moved " + chosen.Count + " clip" + (chosen.Count == 1 ? "" : "s") + " to " + to.Title.ToLowerInvariant() + ".");
    }

    // ---- Settings: selections\voice.ini, as hit_voice.c reads it --------

    string IniPath { get { return Selections == null ? null : Path.Combine(Selections, "voice.ini"); } }

    void LoadSettings()
    {
        var values = new Dictionary<string, decimal>();
        var path = IniPath;
        if (path != null && File.Exists(path))
        {
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Trim();
                int eq = line.IndexOf('=');
                if (line.StartsWith("#") || eq <= 0) continue;
                decimal value;
                if (decimal.TryParse(line.Substring(eq + 1).Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out value))
                    values[line.Substring(0, eq).Trim().ToLowerInvariant()] = value;
            }
        }
        Set(mediumFrom, values, "medium_from", 22m, 1m);
        Set(heavyFrom, values, "heavy_from", 35m, 1m);
        Set(meterLow, values, "meter_low", 28m, 1m);
        Set(meterHigh, values, "meter_high", 46m, 1m);
        Set(gap, values, "gap_ms", 700m, 1000m);
        Set(volume, values, "volume_db", 0m, 1m);
        decimal always;
        heavyAlways.Checked = !values.TryGetValue("heavy_always", out always) || always != 0m;
    }

    static void Set(NumericUpDown box, Dictionary<string, decimal> values, string key, decimal fallback, decimal scale)
    {
        decimal value;
        value = (values.TryGetValue(key, out value) ? value : fallback) / scale;
        box.Value = Math.Max(box.Minimum, Math.Min(box.Maximum, value));
    }

    void SaveSettings()
    {
        if (Selections == null) return;
        if (meterHigh.Value < meterLow.Value) meterHigh.Value = meterLow.Value;
        if (heavyFrom.Value < mediumFrom.Value) heavyFrom.Value = mediumFrom.Value;
        Directory.CreateDirectory(Selections);
        var c = CultureInfo.InvariantCulture;
        var text = new StringBuilder();
        text.AppendLine("# " + Current.Name + "'s hit voices; written by VoiceEditor, read by the game.");
        text.AppendLine("medium_from=" + mediumFrom.Value.ToString("0", c));
        text.AppendLine("heavy_from=" + heavyFrom.Value.ToString("0", c));
        text.AppendLine("heavy_always=" + (heavyAlways.Checked ? "1" : "0"));
        text.AppendLine("meter_low=" + meterLow.Value.ToString("0", c));
        text.AppendLine("meter_high=" + meterHigh.Value.ToString("0", c));
        text.AppendLine("gap_ms=" + (gap.Value * 1000m).ToString("0", c));
        text.AppendLine("volume_db=" + volume.Value.ToString("0.0", c));
        File.WriteAllText(IniPath, text.ToString());
        Say("Saved " + Current.Name + "'s settings.");
    }
}

// WAV in any common PCM or float format to 48 kHz mono 16-bit.
static class Wave
{
    public static short[] ReadMono48k(string path)
    {
        var data = File.ReadAllBytes(path);
        if (data.Length < 12 || Encoding.ASCII.GetString(data, 0, 4) != "RIFF" || Encoding.ASCII.GetString(data, 8, 4) != "WAVE")
            throw new InvalidDataException("not a WAV file");
        int format = 0, channels = 0, rate = 0, bits = 0, start = -1, length = 0;
        for (int at = 12; at + 8 <= data.Length;)
        {
            var id = Encoding.ASCII.GetString(data, at, 4);
            int size = BitConverter.ToInt32(data, at + 4);
            if (size < 0 || at + 8 + (long)size > data.Length) size = data.Length - at - 8;
            if (id == "fmt " && size >= 16)
            {
                format = BitConverter.ToUInt16(data, at + 8);
                channels = BitConverter.ToUInt16(data, at + 10);
                rate = BitConverter.ToInt32(data, at + 12);
                bits = BitConverter.ToUInt16(data, at + 22);
                if (format == 0xFFFE && size >= 26) format = BitConverter.ToUInt16(data, at + 32); // extensible: subformat
            }
            else if (id == "data")
            {
                start = at + 8;
                length = size;
                break;
            }
            at += 8 + size + (size & 1);
        }
        if (start < 0 || channels < 1 || rate < 1000) throw new InvalidDataException("no audio data");
        if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(format == 3 && bits == 32))
            throw new InvalidDataException("unsupported format (" + format + ", " + bits + "-bit); save it as PCM WAV");
        int step = bits / 8 * channels;
        int frames = length / step;
        var mono = new float[frames];
        for (int i = 0; i < frames; ++i)
        {
            float sum = 0f;
            for (int c = 0; c < channels; ++c) sum += Sample(data, start + i * step + c * bits / 8, format, bits);
            mono[i] = sum / channels;
        }
        // Linear resampling to 48 kHz is plenty for voice clips.
        int outFrames = (int)((long)frames * 48000 / rate);
        var pcm = new short[outFrames];
        for (int i = 0; i < outFrames; ++i)
        {
            double position = (double)i * rate / 48000.0;
            int a = (int)position;
            double t = position - a;
            float x = a < frames ? mono[a] : 0f, y = a + 1 < frames ? mono[a + 1] : x;
            double v = (x + (y - x) * t) * 32767.0;
            pcm[i] = (short)Math.Max(-32768.0, Math.Min(32767.0, Math.Round(v)));
        }
        return pcm;
    }

    static float Sample(byte[] data, int at, int format, int bits)
    {
        if (format == 3) return BitConverter.ToSingle(data, at);
        switch (bits)
        {
            case 8: return (data[at] - 128) / 128f;
            case 16: return BitConverter.ToInt16(data, at) / 32768f;
            case 24: return ((data[at] << 8 | data[at + 1] << 16 | data[at + 2] << 24) >> 8) / 8388608f;
            default: return BitConverter.ToInt32(data, at) / 2147483648f;
        }
    }

    public static void Write(string path, short[] pcm)
    {
        using (var stream = new FileStream(path, FileMode.CreateNew))
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write(Encoding.ASCII.GetBytes("RIFF"));
            writer.Write(36 + pcm.Length * 2);
            writer.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
            writer.Write(16);
            writer.Write((short)1);
            writer.Write((short)1);
            writer.Write(48000);
            writer.Write(96000);
            writer.Write((short)2);
            writer.Write((short)16);
            writer.Write(Encoding.ASCII.GetBytes("data"));
            writer.Write(pcm.Length * 2);
            foreach (var s in pcm) writer.Write(s);
        }
    }
}
