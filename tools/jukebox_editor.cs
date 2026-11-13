// SPDX-License-Identifier: GPL-3.0-or-later
// JukeboxEditor: names the Jukebox's tracks and picks which track plays for
// each of the game's music (recomp-runtime/jukebox.c), with a preview player.
//
// It reads every album the game's Jukebox reads: DOA2U's music (DOA2\bgm.afs)
// and Dead or Alive Ultimate's DOA1 music (DOA1\Sound) from the game's disc in
// private\play-disc (or private\imported-disc\disc), and each album that
// tools\import_soundtrack.py put in jukebox\<album> (tracks.txt). Names go to
// jukebox\<album>\names.txt ("file = name", jukebox\doa2u and jukebox\doa1u
// for the disc's albums) and assignments to jukebox\assignments.txt, the files
// the game reads when it starts.
//
// Built with the C# compiler every Windows ships (C# 5):
//   csc /target:winexe /out:JukeboxEditor.exe tools\jukebox_editor.cs
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Media;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class JukeboxEditorProgram
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new JukeboxEditor());
    }
}

class Track
{
    public string File, Name, Path;
    public long Offset, Size; // in Path: an AFS entry, or the whole file (Size 0)
    public double Seconds;
}

class Album
{
    public string Tab, Title, Folder; // Folder: jukebox\<Folder> holds names.txt
    public List<Track> Tracks = new List<Track>();
}

// CRI ADX (type 3, 4-bit, unencrypted), as recomp-runtime/adx_decoder.c decodes it.
static class Adx
{
    static int Be16(byte[] b, int at) { return b[at] << 8 | b[at + 1]; }
    static long Be32(byte[] b, int at) { return (long)(uint)(b[at] << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3]); }

    public static bool Header(byte[] b, out int channels, out int rate, out long samples)
    {
        channels = 0; rate = 0; samples = 0;
        if (b.Length < 0x14 || b[0] != 0x80 || b[1] != 0) return false;
        channels = b[7]; rate = (int)Be32(b, 8); samples = Be32(b, 12);
        return channels > 0 && rate > 0;
    }

    // 16-bit PCM WAV of the whole track.
    public static byte[] ToWav(byte[] b)
    {
        int channels, rate; long samples;
        if (!Header(b, out channels, out rate, out samples)) throw new InvalidDataException("not an ADX file");
        int data = Be16(b, 2) + 4, cutoff = Be16(b, 0x10);
        if (b[4] != 3 || b[5] != 18 || b[6] != 4 || b[0x13] != 0 || cutoff == 0 || channels > 8)
            throw new InvalidDataException("an ADX kind the preview cannot play");
        float z = (float)Math.Cos(6.28318530718 * cutoff / rate);
        float a = 1.41421353816986f - z, bb = 0.414213538169861f;
        float c = (a - (float)Math.Sqrt((a + bb) * (a - bb))) / bb;
        int coef1 = (short)(c * 8192.0f), coef2 = (short)(c * c * -4096.0f);
        var out16 = new MemoryStream();
        var writer = new BinaryWriter(out16);
        writer.Write(new byte[44]);
        var history = new int[channels, 2];
        long decoded = 0;
        var frame = new short[32 * channels];
        for (int at = data; decoded < samples && at + 18 * channels <= b.Length; at += 18 * channels)
        {
            if (b[at] == 0x80 && b[at + 1] == 0x01) break;
            for (int ch = 0; ch < channels; ++ch)
            {
                int f = at + ch * 18;
                int scale = (short)Be16(b, f) + 1;
                for (int i = 0; i < 32; ++i)
                {
                    int value = b[f + 2 + i / 2];
                    int nibble = (((i & 1) != 0 ? value & 0x0f : value >> 4) ^ 8) - 8;
                    int sample = nibble * scale + ((coef1 * history[ch, 0] + coef2 * history[ch, 1]) >> 12);
                    sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
                    history[ch, 1] = history[ch, 0];
                    history[ch, 0] = sample;
                    frame[i * channels + ch] = (short)sample;
                }
            }
            long count = Math.Min(32, samples - decoded);
            for (int i = 0; i < count * channels; ++i) writer.Write(frame[i]);
            decoded += count;
        }
        writer.Flush();
        byte[] wav = out16.ToArray();
        int bytes = wav.Length - 44;
        using (var head = new BinaryWriter(new MemoryStream(wav)))
        {
            head.Write(Encoding.ASCII.GetBytes("RIFF")); head.Write(36 + bytes);
            head.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); head.Write(16); head.Write((short)1);
            head.Write((short)channels); head.Write(rate); head.Write(rate * channels * 2);
            head.Write((short)(channels * 2)); head.Write((short)16);
            head.Write(Encoding.ASCII.GetBytes("data")); head.Write(bytes);
        }
        return wav;
    }
}

class JukeboxEditor : Form
{
    // DOA2U's music table (as jukebox.c's DOA2U_NAMES): the game's music the Jukebox can replace.
    static readonly string[,] Doa2u = {
        {"bgm_doa2_ks.adx", "Kasumi"}, {"bgm_doa2_ln.adx", "Leon"}, {"bgm_doa2_gn.adx", "Gen Fu"},
        {"bgm_doa2_zk.adx", "Zack"}, {"bgm_doa2_tg.adx", "Tengu"}, {"bgm_doa2_le.adx", "Leifang"},
        {"bgm_doa2_jn.adx", "Jann Lee"}, {"bgm_doa2_tn.adx", "Tina"}, {"bgm_doa2_ba.adx", "Bass"},
        {"bgm_doa2_ry.adx", "Ryu Hayabusa"}, {"bgm_doa2_ay.adx", "Ayane"}, {"bgm_doa2_he.adx", "Helena"},
        {"bgm_doa2_ei.adx", "Ein"}, {"bgm_doa2_by.adx", "Bayman"}, {"bgm_doa2_ko.adx", "Hitomi"},
        {"bgm_doa2_sel.adx", "Character Select"}, {"bgm_doa2_staffroll.adx", "Staff Roll"},
        {"bgm_doa2_name.adx", "Name Entry"}, {"bgm_doa2_trainning.adx", "Sparring"},
        {"bgm_doa2_zk_end.adx", "Zack's Ending"}, {"bgm_doa2_he_end.adx", "Helena's Ending"},
        {"bgm_doa2_jn_end.adx", "Jann Lee's Ending"}, {"bgm_doa2_tn_end.adx", "Tina's Ending"},
        {"bgm_doa2_he_con.adx", "Helena (Story)"}, {"bgm_doa2_ei_con.adx", "Ein (Story)"},
        {"bgm_doa2_ba_con.adx", "Bass (Story)"}, {"bgm_doa2_rplyend.adx", "Replay End"},
        {"bgm_doa2_tagend.adx", "Tag Battle End"}, {"bgm_doa2_gn2.adx", "Gen Fu (DOA1)"},
        {"bgm_doa2_le2.adx", "Leifang (DOA1)"}, {"doa12_e3_bgm.adx", "E3 Demo"},
        {"rmx_ks.adx", "Kasumi (Remix)"}, {"rmx_ln.adx", "Leon (Remix)"}, {"rmx_gn.adx", "Gen Fu (Remix)"},
        {"rmx_zk.adx", "Zack (Remix)"}, {"rmx_tg.adx", "Tengu (Remix)"}, {"rmx_le.adx", "Leifang (Remix)"},
        {"rmx_jn.adx", "Jann Lee (Remix)"}, {"rmx_tn.adx", "Tina (Remix)"}, {"rmx_ba.adx", "Bass (Remix)"},
        {"rmx_ry.adx", "Ryu Hayabusa (Remix)"}, {"rmx_ay.adx", "Ayane (Remix)"}, {"rmx_he.adx", "Helena (Remix)"},
        {"rmx_ei.adx", "Ein (Remix)"}, {"rmx_by.adx", "Bayman (Remix)"}, {"rmx_ko.adx", "Hitomi (Remix)"},
        {"bgm_doa2_online.adx", "Online"}, {"dreamon.adx", "Dream On"}, {"ups.adx", "UPS"},
        {"tgs2004.adx", "TGS 2004"},
    };
    // Dead or Alive Ultimate's DOA1 music (as jukebox.c's DOA1U_NAMES).
    static readonly string[,] Doa1u = {
        {"ks_sng.adx", "Kasumi"}, {"lu_sng.adx", "Jann Lee"}, {"le_sng.adx", "Leifang"},
        {"km_sng.adx", "Ryu Hayabusa"}, {"gy_sng.adx", "Bayman"}, {"fa_sng.adx", "Gen Fu"},
        {"kl_sng.adx", "Zack"}, {"tn_sng.adx", "Tina"}, {"rd_sng.adx", "Raidou"},
        {"sel_sng.adx", "Character Select"}, {"name_sng.adx", "Name Entry"}, {"cnt_sng.adx", "Continue"},
        {"clr_sng.adx", "Clear"}, {"trng_sng.adx", "Training"},
        {"kl_lose.adx", "Zack (Lose)"}, {"le_lose.adx", "Leifang (Lose)"}, {"km_lose.adx", "Ryu Hayabusa (Lose)"},
        {"lu_lose.adx", "Jann Lee (Lose)"}, {"tn_lose.adx", "Tina (Lose)"}, {"gy_lose.adx", "Bayman (Lose)"},
        {"fa_lose.adx", "Gen Fu (Lose)"}, {"ks_lose.adx", "Kasumi (Lose)"}, {"ks_lose2.adx", "Kasumi (Lose 2)"},
        {"rd_lose.adx", "Raidou (Lose)"}, {"opmov_sng.adx", "Opening Movie"}, {"edmov_sng.adx", "Ending Movie"},
        {"online_sng.adx", "Online"},
    };
    const string Own = "The game's own music";

    readonly string root, jukebox, disc;
    readonly List<Album> albums = new List<Album>();
    readonly Dictionary<string, string> assigned = new Dictionary<string, string>(); // music file -> "TAB\tfile"
    readonly ListBox albumList = new ListBox();
    readonly TextBox title = new TextBox();
    readonly DataGridView grid = new DataGridView(), slots = new DataGridView();
    readonly Label status = new Label();
    readonly Button play = new Button(), stop = new Button(), save = new Button();
    SoundPlayer player;
    int playId;
    bool dirty, loading;

    public JukeboxEditor()
    {
        root = AppDomain.CurrentDomain.BaseDirectory;
        if (!File.Exists(Path.Combine(root, "Launcher.exe")) && Directory.Exists(Path.Combine(root, "..", "private")))
            root = Path.GetFullPath(Path.Combine(root, ".."));
        jukebox = Path.Combine(root, "jukebox");
        string old = Path.Combine(root, "private", "jukebox");
        try { if (!Directory.Exists(jukebox) && Directory.Exists(old)) Directory.Move(old, jukebox); } catch (Exception) { }
        disc = new[] { Path.Combine(root, "private", "play-disc"), Path.Combine(root, "private", "imported-disc", "disc") }
            .FirstOrDefault(d => File.Exists(Path.Combine(d, "DOA2", "bgm.afs")));

        Text = "Jukebox Editor - Dead or Alive 2 Ultimate";
        Size = new Size(1000, 680);
        MinimumSize = new Size(760, 480);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9.5f);

        var tabs = new TabControl { Dock = DockStyle.Fill };
        var tracksPage = new TabPage("Tracks");
        var slotsPage = new TabPage("Assignments");
        tabs.TabPages.Add(tracksPage);
        tabs.TabPages.Add(slotsPage);

        albumList.Dock = DockStyle.Left;
        albumList.Width = 300;
        albumList.IntegralHeight = false;
        albumList.FormattingEnabled = true;
        albumList.SelectedIndexChanged += delegate { if (!loading) ShowAlbum(); };

        var titleRow = new Panel { Dock = DockStyle.Top, Height = 34, Padding = new Padding(6) };
        var titleLabel = new Label { Text = "Album name:", AutoSize = true, Dock = DockStyle.Left, Padding = new Padding(0, 4, 6, 0) };
        title.Dock = DockStyle.Fill;
        title.TextChanged += delegate
        {
            Album album = albumList.SelectedItem as Album;
            if (album == null || loading || album.Title == title.Text) return;
            album.Title = title.Text;
            dirty = true;
        };
        title.Leave += delegate
        {
            int at = albumList.SelectedIndex;
            if (at < 0) return;
            loading = true;
            albumList.Items[at] = albumList.Items[at];
            albumList.SelectedIndex = at;
            loading = false;
        };
        titleRow.Controls.Add(title);
        titleRow.Controls.Add(titleLabel);

        SetUpGrid(grid);
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "#", Width = 44, ReadOnly = true });
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "Name (click to edit)", AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill });
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "Length", Width = 70, ReadOnly = true });
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "File", Width = 200, ReadOnly = true });
        grid.CellValueChanged += GridEdited;
        grid.CellDoubleClick += delegate(object s, DataGridViewCellEventArgs e) { if (e.RowIndex >= 0 && e.ColumnIndex != 1) Play(); };
        grid.KeyDown += delegate(object s, KeyEventArgs e) { if (e.KeyCode == Keys.Space && !grid.IsCurrentCellInEditMode) { Play(); e.Handled = true; } };

        var trackSide = new Panel { Dock = DockStyle.Fill };
        trackSide.Controls.Add(grid);
        trackSide.Controls.Add(titleRow);
        tracksPage.Controls.Add(trackSide);
        tracksPage.Controls.Add(new Splitter { Dock = DockStyle.Left });
        tracksPage.Controls.Add(albumList);

        SetUpGrid(slots);
        slots.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "The game's music", Width = 220, ReadOnly = true });
        var choice = new DataGridViewComboBoxColumn { HeaderText = "Plays", AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill,
            FlatStyle = FlatStyle.Flat, DisplayStyle = DataGridViewComboBoxDisplayStyle.ComboBox };
        slots.Columns.Add(choice);
        slots.CellValueChanged += SlotEdited;
        slots.CurrentCellDirtyStateChanged += delegate { if (slots.IsCurrentCellDirty) slots.CommitEdit(DataGridViewDataErrorContexts.Commit); };
        slots.DataError += delegate(object s, DataGridViewDataErrorEventArgs e) { e.ThrowException = false; };
        slotsPage.Controls.Add(slots);
        var slotHelp = new Label { Dock = DockStyle.Top, Height = 40, Padding = new Padding(6),
            Text = "Choose the track each of the game's music plays. Preview a track on the Tracks page. Assignments are offline only: online the game plays its own music." };
        slotsPage.Controls.Add(slotHelp);

        var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 44, Padding = new Padding(6), FlowDirection = FlowDirection.LeftToRight };
        play.Text = "▶ Play"; play.Width = 90; play.Click += delegate { Play(); };
        stop.Text = "■ Stop"; stop.Width = 90; stop.Click += delegate { Stop(); };
        save.Text = "Save"; save.Width = 90; save.Click += delegate { Save(); };
        status.AutoSize = true;
        status.Padding = new Padding(10, 7, 0, 0);
        bottom.Controls.AddRange(new Control[] { play, stop, save, status });

        Controls.Add(tabs);
        Controls.Add(bottom);
        FormClosing += AskToSave;

        LoadAlbums();
        BuildSlots();
        if (albumList.Items.Count > 0) albumList.SelectedIndex = 0;
        status.Text = disc == null ? "The game's disc was not found in private\\play-disc: build the game first. Imported albums still show."
            : "Double-click a track or press Space to hear it. Edit a name, then Save. The game reads the changes when it starts.";
    }

    static void SetUpGrid(DataGridView g)
    {
        g.Dock = DockStyle.Fill;
        g.AllowUserToAddRows = false;
        g.AllowUserToDeleteRows = false;
        g.AllowUserToResizeRows = false;
        g.RowHeadersVisible = false;
        g.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
        g.MultiSelect = false;
        g.BackgroundColor = SystemColors.Window;
        g.BorderStyle = BorderStyle.None;
        g.EditMode = DataGridViewEditMode.EditOnKeystrokeOrF2;
        g.CellClick += delegate(object s, DataGridViewCellEventArgs e)
        {
            if (e.RowIndex >= 0 && !g.Columns[e.ColumnIndex].ReadOnly) g.BeginEdit(true);
        };
    }

    // ---- Reading ----------------------------------------------------------------

    void LoadAlbums()
    {
        albums.Clear();
        if (disc != null)
        {
            albums.Add(LoadDoa2u());
            albums.Add(LoadDoa1u());
        }
        var known = new[] { "doa2u-movies", "doa3", "doa4" };
        var folders = Directory.Exists(jukebox) ? Directory.GetDirectories(jukebox).Select(Path.GetFileName).ToList() : new List<string>();
        foreach (string k in known) if (!folders.Contains(k, StringComparer.OrdinalIgnoreCase)) folders.Insert(0, k);
        var order = known.Concat(folders.Where(f => !known.Contains(f.ToLowerInvariant())).OrderBy(f => f, StringComparer.OrdinalIgnoreCase));
        foreach (string folder in order)
        {
            string lower = folder.ToLowerInvariant();
            if (lower == "doa2u" || lower == "doa1u") continue;
            Album album = LoadFolder(folder);
            if (album != null) albums.Add(album);
        }
        foreach (Album album in albums) ApplyNames(album);
        albumList.Items.Clear();
        foreach (Album album in albums) albumList.Items.Add(album);
        albumList.Format += delegate(object s, ListControlConvertEventArgs e)
        {
            Album a = (Album)e.ListItem;
            e.Value = a.Tab + "  -  " + a.Title + "  (" + a.Tracks.Count + ")";
        };
        LoadAssignments();
    }

    static uint Le32(byte[] b, int at) { return BitConverter.ToUInt32(b, at); }

    Album LoadDoa2u()
    {
        var album = new Album { Tab = "DOA2U", Title = "Dead or Alive 2 Ultimate", Folder = "doa2u" };
        string path = Path.Combine(disc, "DOA2", "bgm.afs");
        using (var file = File.OpenRead(path))
        {
            var head = new byte[8];
            file.Read(head, 0, 8);
            int count = (int)Le32(head, 4);
            var table = new byte[8 * (count + 1)];
            file.Read(table, 0, table.Length);
            uint namesAt = Le32(table, 8 * count), namesSize = Le32(table, 8 * count + 4);
            var names = new byte[namesSize];
            file.Seek(namesAt, SeekOrigin.Begin);
            file.Read(names, 0, names.Length);
            for (int n = 0; n < Doa2u.GetLength(0); ++n)
                for (int i = 0; i < count; ++i)
                {
                    string entry = Encoding.ASCII.GetString(names, 48 * i, 32).TrimEnd('\0').ToLowerInvariant();
                    if (entry != Doa2u[n, 0]) continue;
                    var track = new Track { File = Doa2u[n, 0], Name = Doa2u[n, 1], Path = path, Offset = Le32(table, 8 * i), Size = Le32(table, 8 * i + 4) };
                    track.Seconds = Seconds(track);
                    album.Tracks.Add(track);
                    break;
                }
        }
        return album;
    }

    Album LoadDoa1u()
    {
        var album = new Album { Tab = "DOA1U", Title = "Dead or Alive Ultimate", Folder = "doa1u" };
        for (int n = 0; n < Doa1u.GetLength(0); ++n)
        {
            string path = Path.Combine(disc, "DOA1", "Sound", Doa1u[n, 0]);
            if (!File.Exists(path)) continue;
            var track = new Track { File = Doa1u[n, 0], Name = Doa1u[n, 1], Path = path };
            track.Seconds = Seconds(track);
            album.Tracks.Add(track);
        }
        return album;
    }

    Album LoadFolder(string folder)
    {
        string list = Path.Combine(jukebox, folder, "tracks.txt");
        if (!File.Exists(list)) return null;
        string lower = folder.ToLowerInvariant();
        var album = new Album { Tab = lower == "doa3" ? "DOA3" : lower == "doa4" ? "DOA4" : folder.ToUpperInvariant(), Folder = folder };
        album.Title = album.Tab;
        if (album.Tab.Length > 15) album.Tab = album.Tab.Substring(0, 15);
        foreach (string line in File.ReadAllLines(list, Encoding.UTF8))
        {
            string[] field = line.Split('\t');
            if (field[0] == "#title" && field.Length > 1) { album.Title = field[1]; continue; }
            if (line.StartsWith("#") || field.Length < 4 || field[0] == "") continue;
            var track = new Track { File = field[0], Name = field[3], Path = Path.Combine(jukebox, folder, field[0]) };
            track.Seconds = Seconds(track);
            album.Tracks.Add(track);
        }
        return album;
    }

    void ApplyNames(Album album)
    {
        string path = Path.Combine(jukebox, album.Folder, "names.txt");
        if (!File.Exists(path)) return;
        foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
        {
            string line = raw.TrimStart('﻿');
            if (line.StartsWith("#")) continue;
            int equals = line.IndexOf('=');
            if (equals < 0) continue;
            string key = line.Substring(0, equals).Trim(), name = line.Substring(equals + 1).Trim();
            if (name == "") continue;
            if (key == "album") { album.Title = name; continue; }
            foreach (Track track in album.Tracks) if (track.File == key) track.Name = name;
        }
    }

    void LoadAssignments()
    {
        assigned.Clear();
        string path = Path.Combine(jukebox, "assignments.txt");
        if (!File.Exists(path)) return;
        foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
        {
            if (line.StartsWith("#")) continue;
            string[] field = line.Split('\t');
            if (field.Length >= 3) assigned[field[0]] = field[1] + "\t" + field[2];
        }
    }

    static byte[] Bytes(Track track)
    {
        using (var file = File.OpenRead(track.Path))
        {
            long size = track.Size != 0 ? track.Size : file.Length;
            var bytes = new byte[size];
            file.Seek(track.Offset, SeekOrigin.Begin);
            int read = 0;
            while (read < size) { int got = file.Read(bytes, read, (int)(size - read)); if (got <= 0) break; read += got; }
            return bytes;
        }
    }

    static double Seconds(Track track)
    {
        try
        {
            using (var file = File.OpenRead(track.Path))
            {
                var head = new byte[0x20];
                file.Seek(track.Offset, SeekOrigin.Begin);
                file.Read(head, 0, head.Length);
                int channels, rate; long samples;
                return Adx.Header(head, out channels, out rate, out samples) ? (double)samples / rate : 0;
            }
        }
        catch (Exception) { return 0; }
    }

    // ---- The pages ------------------------------------------------------------

    void ShowAlbum()
    {
        Album album = albumList.SelectedItem as Album;
        loading = true;
        grid.Rows.Clear();
        title.Text = album != null ? album.Title : "";
        if (album != null)
            for (int i = 0; i < album.Tracks.Count; ++i)
            {
                Track t = album.Tracks[i];
                grid.Rows.Add((i + 1).ToString(), t.Name, t.Seconds > 0 ? TimeSpan.FromSeconds(Math.Round(t.Seconds)).ToString(@"m\:ss") : "?", t.File);
                grid.Rows[i].Tag = t;
            }
        loading = false;
    }

    void GridEdited(object sender, DataGridViewCellEventArgs e)
    {
        if (loading || e.RowIndex < 0 || e.ColumnIndex != 1) return;
        var track = (Track)grid.Rows[e.RowIndex].Tag;
        string name = (Convert.ToString(grid.Rows[e.RowIndex].Cells[1].Value) ?? "").Trim();
        if (name == "") { grid.Rows[e.RowIndex].Cells[1].Value = track.Name; return; }
        if (name == track.Name) return;
        track.Name = name;
        dirty = true;
        albumList.Refresh();
        BuildSlots();
    }

    string TrackLabel(Album album, Track track) { return album.Tab + "  ·  " + track.Name; }

    void BuildSlots()
    {
        loading = true;
        var choice = (DataGridViewComboBoxColumn)slots.Columns[1];
        var items = new List<string> { Own };
        foreach (Album album in albums) foreach (Track track in album.Tracks) items.Add(TrackLabel(album, track));
        int keep = slots.FirstDisplayedScrollingRowIndex;
        slots.Rows.Clear();
        choice.Items.Clear();
        choice.Items.AddRange(items.ToArray());
        for (int n = 0; n < Doa2u.GetLength(0); ++n)
        {
            string value = Own, pick;
            if (assigned.TryGetValue(Doa2u[n, 0], out pick))
            {
                string[] part = pick.Split('\t');
                Album album = albums.FirstOrDefault(a => a.Tab == part[0]);
                Track track = album != null ? album.Tracks.FirstOrDefault(t => t.File == part[1]) : null;
                if (track != null) value = TrackLabel(album, track);
            }
            int row = slots.Rows.Add(Doa2u[n, 1], value);
            slots.Rows[row].Tag = Doa2u[n, 0];
        }
        if (keep >= 0 && keep < slots.Rows.Count) slots.FirstDisplayedScrollingRowIndex = keep;
        loading = false;
    }

    void SlotEdited(object sender, DataGridViewCellEventArgs e)
    {
        if (loading || e.RowIndex < 0 || e.ColumnIndex != 1) return;
        string music = (string)slots.Rows[e.RowIndex].Tag, value = Convert.ToString(slots.Rows[e.RowIndex].Cells[1].Value);
        assigned.Remove(music);
        foreach (Album album in albums)
            foreach (Track track in album.Tracks)
                if (TrackLabel(album, track) == value) assigned[music] = album.Tab + "\t" + track.File;
        dirty = true;
    }

    // ---- Preview --------------------------------------------------------------

    void Play()
    {
        if (grid.CurrentRow == null) return;
        var track = (Track)grid.CurrentRow.Tag;
        Stop();
        int id = ++playId;
        status.Text = "Loading " + track.Name + "...";
        var thread = new Thread(() =>
        {
            byte[] wav = null;
            string error = null;
            try { wav = Adx.ToWav(Bytes(track)); }
            catch (Exception ex) { error = ex.Message; }
            BeginInvoke((Action)(() =>
            {
                if (id != playId) return;
                if (wav == null) { status.Text = "Cannot play " + track.Name + ": " + error; return; }
                player = new SoundPlayer(new MemoryStream(wav));
                player.Play();
                status.Text = "Playing " + track.Name + "  (" + track.File + ")";
            }));
        });
        thread.IsBackground = true;
        thread.Start();
    }

    void Stop()
    {
        ++playId;
        if (player != null) { player.Stop(); player.Dispose(); player = null; }
        status.Text = "Stopped";
    }

    // ---- Writing --------------------------------------------------------------

    bool Save()
    {
        if (grid.IsCurrentCellInEditMode) grid.EndEdit();
        try
        {
            Directory.CreateDirectory(jukebox);
            foreach (Album album in albums)
            {
                string folder = Path.Combine(jukebox, album.Folder);
                Directory.CreateDirectory(folder);
                var text = new StringBuilder();
                text.Append("# The Jukebox's names for the " + album.Tab + " album's tracks.\n");
                text.Append("# Change the text after \"=\" and save; the game reads this file when it starts.\n");
                text.Append("# Leave the file name before \"=\" as it is.\n\n");
                text.Append("album = " + album.Title + "\n\n");
                foreach (Track track in album.Tracks) text.Append(track.File + " = " + track.Name + "\n");
                File.WriteAllText(Path.Combine(folder, "names.txt"), text.ToString(), new UTF8Encoding(false));
            }
            var lines = new StringBuilder("# The Jukebox's assignments: the game's music<TAB>album<TAB>track file. Set in game.\n");
            for (int n = 0; n < Doa2u.GetLength(0); ++n)
            {
                string pick;
                if (assigned.TryGetValue(Doa2u[n, 0], out pick)) lines.Append(Doa2u[n, 0] + "\t" + pick + "\n");
            }
            File.WriteAllText(Path.Combine(jukebox, "assignments.txt"), lines.ToString(), new UTF8Encoding(false));
            dirty = false;
            status.Text = "Saved. Restart the game if it is running to hear the changes.";
            return true;
        }
        catch (Exception ex)
        {
            MessageBox.Show("Could not save: " + ex.Message, Text);
            return false;
        }
    }

    void AskToSave(object sender, FormClosingEventArgs e)
    {
        Stop();
        if (!dirty) return;
        var answer = MessageBox.Show("Save your changes?", Text, MessageBoxButtons.YesNoCancel);
        if (answer == DialogResult.Cancel || (answer == DialogResult.Yes && !Save())) e.Cancel = true;
    }
}
