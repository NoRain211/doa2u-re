// SPDX-License-Identifier: GPL-3.0-or-later
// JukeboxEditor: names the Jukebox's tracks and picks which track plays for
// each of the game's music (recomp-runtime/jukebox.c), with a preview player.
//
// It reads every album the game's Jukebox reads: DOA2U's music (DOA2\bgm.afs, which the game
// lists in jukebox\doa2u\names.txt when it starts) and Dead or Alive Ultimate's DOA1 music
// (DOA1\Sound, listed in jukebox\doa1u\names.txt) from the game's disc in
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
    const string Own = "The game's own music";

    readonly string root, jukebox, disc;
    readonly List<Album> albums = new List<Album>();
    readonly List<Track> music = new List<Track>(); // the game's own music: what a row assigns
    readonly HashSet<string> editedSlots = new HashSet<string>();
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
            FlatStyle = FlatStyle.Flat, DisplayStyle = DataGridViewComboBoxDisplayStyle.ComboBox, ValueType = typeof(Choice) };
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
        music.Clear();
        if (disc != null)
        {
            albums.Add(LoadDoa2u());
            albums.Add(LoadDoa1u());
        }
        var known = new[] { "doa2u-movies", "doa3", "doa4", "doaxbv" };
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

    // The game's music is bgm.afs's entries, in the order and with the names jukebox\doa2u\
    // names.txt lists (the game writes it when it starts); before its first start the archive's
    // own order and names stand in. The table's unused slots are a one-sample placeholder.
    Album LoadDoa2u()
    {
        var album = new Album { Tab = "DOA2U", Title = "Dead or Alive 2 Ultimate", Folder = "doa2u" };
        var entries = new List<Track>();
        string path = Path.Combine(disc, "DOA2", "bgm.afs");
        try
        {
            using (var file = File.OpenRead(path))
            using (var reader = new BinaryReader(file))
            {
                byte[] head = reader.ReadBytes(8);
                if (head.Length != 8 || Le32(head, 0) != 0x00534641u) return album;
                uint rawCount = Le32(head, 4);
                if (rawCount == 0 || rawCount > 4096) return album;
                int count = (int)rawCount;
                byte[] table = reader.ReadBytes(8 * (count + 1));
                if (table.Length != 8 * (count + 1)) return album;
                uint namesAt = Le32(table, 8 * count), namesSize = Le32(table, 8 * count + 4);
                if (namesSize < 48u * rawCount || (ulong)namesAt + namesSize > (ulong)file.Length) return album;
                file.Seek(namesAt, SeekOrigin.Begin);
                byte[] names = reader.ReadBytes(48 * count);
                if (names.Length != 48 * count) return album;
                for (int i = 0; i < count; ++i)
                {
                    uint offset = Le32(table, 8 * i), size = Le32(table, 8 * i + 4);
                    if ((ulong)offset + size > (ulong)file.Length) continue;
                    string name = Encoding.ASCII.GetString(names, 48 * i, 32).TrimEnd('\0');
                    var track = new Track { File = name, Path = path, Offset = offset, Size = size };
                    track.Seconds = Seconds(track);
                    entries.Add(track);
                }
            }
        }
        catch (IOException) { entries.Clear(); }
        catch (UnauthorizedAccessException) { entries.Clear(); }
        var byFile = new Dictionary<string, Track>(StringComparer.OrdinalIgnoreCase);
        foreach (Track track in entries) if (!byFile.ContainsKey(track.File)) byFile[track.File] = track;
        var listed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string[] pair in ReadNameLines(Path.Combine(jukebox, "doa2u", "names.txt")))
        {
            Track track;
            if (!byFile.TryGetValue(pair[0], out track) || track.Seconds < 1 || listed.Contains(track.File)) continue;
            track.Name = pair[1];
            listed.Add(track.File);
            album.Tracks.Add(track);
        }
        foreach (Track track in entries)
            if (!listed.Contains(track.File) && track.Seconds >= 1)
            {
                listed.Add(track.File);
                track.Name = StemLabel(track.File);
                album.Tracks.Add(track);
            }
        music.Clear();
        music.AddRange(album.Tracks);
        return album;
    }

    // Dead or Alive Ultimate's DOA1 music: DOA1\Sound, in the order jukebox\doa1u\names.txt lists.
    Album LoadDoa1u()
    {
        var album = new Album { Tab = "DOA1U", Title = "Dead or Alive Ultimate", Folder = "doa1u" };
        var byFile = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            foreach (string path in Directory.GetFiles(Path.Combine(disc, "DOA1", "Sound")))
                byFile[Path.GetFileName(path)] = path;
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        var listed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string[] pair in ReadNameLines(Path.Combine(jukebox, "doa1u", "names.txt")))
        {
            string path;
            if (!byFile.TryGetValue(pair[0], out path) || !listed.Add(pair[0])) continue;
            album.Tracks.Add(Doa1Track(path, pair[1]));
        }
        foreach (var pair in byFile.OrderBy(entry => entry.Key, StringComparer.OrdinalIgnoreCase))
        {
            if (listed.Contains(pair.Key)) continue;
            Track track = Doa1Track(pair.Value, StemLabel(pair.Key));
            if (track.Seconds > 0) album.Tracks.Add(track);
        }
        return album;
    }

    static Track Doa1Track(string path, string name)
    {
        var track = new Track { File = Path.GetFileName(path), Name = name, Path = path };
        track.Seconds = Seconds(track);
        return track;
    }

    // "file = name" lines in the file's order; "album" names the album, not a track.
    static List<string[]> ReadNameLines(string path)
    {
        var lines = new List<string[]>();
        if (!File.Exists(path)) return lines;
        foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
        {
            string line = raw.TrimStart('﻿');
            if (line.StartsWith("#")) continue;
            int equals = line.IndexOf('=');
            if (equals < 0) continue;
            string key = line.Substring(0, equals).Trim(), name = line.Substring(equals + 1).Trim();
            if (key != "" && key != "album" && name != "") lines.Add(new[] { key, name });
        }
        return lines;
    }

    // A file's own words, for a track the game has not named yet.
    static string StemLabel(string file)
    {
        string[] words = Path.GetFileNameWithoutExtension(file).Split(new[] { '_', '-', ' ' },
            StringSplitOptions.RemoveEmptyEntries);
        for (int i = 0; i < words.Length; ++i)
            if (words[i].Length > 0)
                words[i] = char.ToUpperInvariant(words[i][0]) + words[i].Substring(1);
        return string.Join(" ", words);
    }

    // tracks.txt names files in its own folder, as recomp-runtime/jukebox.c's bare_name reads
    // it: a player can drop a shared album beside the game's, so the field is not a path out.
    static bool BareName(string name)
    {
        return name != "" && name != "." && !name.Contains("..") && name.IndexOfAny(new[] { '/', '\\', ':' }) < 0;
    }

    Album LoadFolder(string folder)
    {
        string list = Path.Combine(jukebox, folder, "tracks.txt");
        if (!File.Exists(list)) return null;
        string lower = folder.ToLowerInvariant();
        var album = new Album { Tab = lower == "doa3" ? "DOA3" : lower == "doa4" ? "DOA4" : lower == "doaxbv" ? "DOAXBV" : folder.ToUpperInvariant(), Folder = folder };
        album.Title = album.Tab;
        if (album.Tab.Length > 15) album.Tab = album.Tab.Substring(0, 15);
        foreach (string line in File.ReadAllLines(list, Encoding.UTF8))
        {
            string[] field = line.Split('\t');
            if (field[0] == "#title" && field.Length > 1) { album.Title = field[1]; continue; }
            if (line.StartsWith("#") || field.Length < 4 || !BareName(field[0])) continue;
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

    static Dictionary<string, string> ReadAssignments(string path)
    {
        var result = new Dictionary<string, string>();
        if (!File.Exists(path)) return result;
        foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
        {
            if (line.StartsWith("#")) continue;
            string[] field = line.Split('\t');
            if (field.Length >= 3) result[field[0]] = field[1] + "\t" + field[2];
        }
        return result;
    }

    // The assignments file a save writes: its header, the assignments, then the
    // player's own comments and lines that are not assignments, as the game's save keeps them.
    static string AssignmentText(string path, Dictionary<string, string> latest)
    {
        const string header = "# The Jukebox's assignments: the game's music<TAB>album<TAB>track file. Set in game.";
        var lines = new StringBuilder(header + "\n");
        foreach (var pair in latest.OrderBy(pair => pair.Key)) lines.Append(pair.Key + "\t" + pair.Value + "\n");
        if (File.Exists(path))
            foreach (string line in File.ReadAllLines(path, Encoding.UTF8))
                if (line != header && line.Trim().Length != 0 && (line.StartsWith("#") || line.Split('\t').Length < 3))
                    lines.Append(line + "\n");
        return lines.ToString();
    }

    static Dictionary<string, string> MergeAssignments(string path, Dictionary<string, string> picks, IEnumerable<string> edited)
    {
        var latest = ReadAssignments(path);
        foreach (string key in edited)
        {
            string pick;
            if (picks.TryGetValue(key, out pick)) latest[key] = pick;
            else latest.Remove(key);
        }
        return latest;
    }

    void LoadAssignments()
    {
        assigned.Clear();
        foreach (var pair in ReadAssignments(Path.Combine(jukebox, "assignments.txt"))) assigned[pair.Key] = pair.Value;
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

    static string TrackLabel(Album album, Track track) { return album.Tab + "  ·  " + track.Name; }

    // A row's choice holds the track itself: two tracks can share a display name, and the
    // combo's text must not decide which track a row is assigned.
    class Choice
    {
        public readonly Album Album;
        public readonly Track Track; // null: the game's own music
        public Choice(Album album, Track track) { Album = album; Track = track; }
        public override string ToString() { return Track == null ? Own : TrackLabel(Album, Track); }
    }

    void BuildSlots()
    {
        loading = true;
        var choice = (DataGridViewComboBoxColumn)slots.Columns[1];
        var items = new List<Choice> { new Choice(null, null) };
        foreach (Album album in albums) foreach (Track track in album.Tracks) items.Add(new Choice(album, track));
        int keep = slots.FirstDisplayedScrollingRowIndex;
        slots.Rows.Clear();
        choice.Items.Clear();
        choice.Items.AddRange(items.ToArray());
        foreach (Track track in music)
        {
            string pick;
            Choice value = items[0];
            if (assigned.TryGetValue(track.File, out pick))
            {
                string[] part = pick.Split('\t');
                value = items.FirstOrDefault(c => c.Track != null && c.Album.Tab == part[0] && c.Track.File == part[1]) ?? items[0];
            }
            int row = slots.Rows.Add(track.Name, value);
            slots.Rows[row].Tag = track.File;
        }
        if (keep >= 0 && keep < slots.Rows.Count) slots.FirstDisplayedScrollingRowIndex = keep;
        loading = false;
    }

    void SlotEdited(object sender, DataGridViewCellEventArgs e)
    {
        if (loading || e.RowIndex < 0 || e.ColumnIndex != 1) return;
        string music = (string)slots.Rows[e.RowIndex].Tag;
        var choice = slots.Rows[e.RowIndex].Cells[1].Value as Choice;
        editedSlots.Add(music);
        if (choice == null || choice.Track == null) assigned.Remove(music);
        else assigned[music] = choice.Album.Tab + "\t" + choice.Track.File;
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

    static void AtomicText(string path, string text)
    {
        string temporary = path + ".tmp." + Guid.NewGuid().ToString("N");
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                byte[] bytes = new UTF8Encoding(false).GetBytes(text);
                file.Write(bytes, 0, bytes.Length);
                file.Flush(true);
            }
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    // A name or title becomes a names.txt record's text. import_soundtrack.py's record_text
    // also spaces out tabs and line breaks: a break would start or split a record, while a
    // later "=" stays in the value because readers split at the first one.
    static string RecordText(string text)
    {
        return (text ?? "").Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');
    }

    // A record's key: the text before its first "=", or null where it is not one.
    static string RecordKey(string line)
    {
        int equals = line.IndexOf('=');
        return equals < 0 ? null : line.Substring(0, equals).Trim();
    }

    // A save keeps every line the player has, as the game's apply_names does: the records it
    // knows are rewritten and the ones the file lacks are added, so comments, keys for files
    // it does not list and the album's own title all stay.
    static void MergeNames(string path, Album album)
    {
        bool exists = File.Exists(path);
        var lines = new List<string>();
        bool titled = false;
        var listed = new HashSet<string>();
        if (exists)
            foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = raw.TrimStart('﻿');
                string key = line.StartsWith("#") ? null : RecordKey(line);
                if (key == "album")
                {
                    lines.Add("album = " + RecordText(album.Title));
                    titled = true;
                    continue;
                }
                Track track = key == null ? null : album.Tracks.Find(t => t.File == key);
                if (track == null) { lines.Add(line); continue; }
                lines.Add(track.File + " = " + RecordText(track.Name));
                listed.Add(track.File);
            }
        if (!titled) lines.Add("album = " + RecordText(album.Title));
        foreach (Track track in album.Tracks)
            if (listed.Add(track.File)) lines.Add(track.File + " = " + RecordText(track.Name));
        var text = new StringBuilder();
        if (!exists)
        {
            text.Append("# The Jukebox's names for the " + album.Tab + " album's tracks.\n");
            text.Append("# Change the text after \"=\" and save; the game reads this file when it starts.\n");
            text.Append("# Leave the file name before \"=\" as it is.\n\n");
        }
        foreach (string line in lines) text.Append(line + "\n");
        AtomicText(path, text.ToString());
    }

    bool Save()
    {
        if (grid.IsCurrentCellInEditMode) grid.EndEdit();
        try
        {
            Directory.CreateDirectory(jukebox);
            if (slots.IsCurrentCellInEditMode) slots.EndEdit();
            string assignmentPath = Path.Combine(jukebox, "assignments.txt");
            var latest = MergeAssignments(assignmentPath, assigned, editedSlots);
            foreach (Album album in albums)
            {
                string folder = Path.Combine(jukebox, album.Folder);
                Directory.CreateDirectory(folder);
                MergeNames(Path.Combine(folder, "names.txt"), album);
            }
            AtomicText(assignmentPath, AssignmentText(assignmentPath, latest));
            assigned.Clear();
            foreach (var pair in latest) assigned[pair.Key] = pair.Value;
            editedSlots.Clear();
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
