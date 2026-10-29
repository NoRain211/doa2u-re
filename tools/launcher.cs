// SPDX-License-Identifier: GPL-3.0-or-later
// Windows launcher, built twice by tools/package_test.py with the C# compiler
// every Windows ships (.NET Framework 4):
//   Launcher.exe builds the game from your Disc 2 ISO, picks resolution,
//     anti-aliasing, SMAA, widescreen, the intro skip and 5.1 surround,
//     remembers them in private\launcher.json, then plays. An ISO dropped on it
//     opens the build page with that disc chosen.
//   DOA2U.exe (PLAY_ONLY) plays with the saved settings.
// Adapted from committed DOA3 tools/launcher.cs at 6c782d5: its Horizon Bar menu
// over the generated beach (tools/launcher-background.jpg); no retail art is used.
// The build and the game run through tools\build_game.py and tools\run_game.py,
// so the saved settings and the build receipt are checked in one place.
// Keep to C# 5: the in-box compiler knows nothing newer.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;

static class Game
{
    public const string Title = "Dead or Alive 2 Ultimate";
    public const string PythonMessage = "Install Python 3.12 or newer from https://www.python.org/downloads/windows/ and tick Add python.exe to PATH.";
    public static readonly string Root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
    public static readonly string Runner = Path.Combine(Root, @"build\recomp-program-mingw\recomp_program_runner.exe");
    public static string Private(string name) { return Path.Combine(Root, "private", name); }
    static string[] python;
    static bool pythonFound;

    public static Dictionary<string, object> LoadSettings()
    {
        try
        {
            return new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(File.ReadAllText(Private("launcher.json")))
                ?? new Dictionary<string, object>();
        }
        catch (Exception) { return new Dictionary<string, object>(); }
    }

    public static void SaveSettings(Dictionary<string, object> settings)
    {
        Directory.CreateDirectory(Path.Combine(Root, "private"));
        // UTF-8 without a byte order mark keeps a non-ASCII disc path intact.
        File.WriteAllText(Private("launcher.json"), new JavaScriptSerializer().Serialize(settings), new UTF8Encoding(false));
    }

    public static int Int(Dictionary<string, object> s, string key, int fallback)
    {
        object v;
        try { return s.TryGetValue(key, out v) && v != null ? Convert.ToInt32(v, CultureInfo.InvariantCulture) : fallback; }
        catch (Exception) { return fallback; }
    }

    public static bool Bool(Dictionary<string, object> s, string key, bool fallback)
    {
        object v;
        return s.TryGetValue(key, out v) && v is bool ? (bool)v : fallback;
    }

    public static string Str(Dictionary<string, object> s, string key, string fallback)
    {
        object v;
        return s.TryGetValue(key, out v) && v is string ? (string)v : fallback;
    }

    public static bool IsIso(string path)
    {
        return !string.IsNullOrEmpty(path) && path.EndsWith(".iso", StringComparison.OrdinalIgnoreCase) && File.Exists(path);
    }

    // Python 3.12 or newer, through the py launcher or PATH; null if there is none.
    public static string[] Python
    {
        get
        {
            if (pythonFound) return python;
            pythonFound = true;
            foreach (var candidate in new[] { new[] { "py", "-3" }, new[] { "python" } })
            {
                string args = string.Join(" ", candidate.Skip(1)) + " -c \"import sys; sys.exit(sys.version_info < (3, 12))\"";
                try
                {
                    using (var check = Process.Start(new ProcessStartInfo(candidate[0], args) { UseShellExecute = false, CreateNoWindow = true }))
                        if (check.WaitForExit(30000) && check.ExitCode == 0) return python = candidate;
                }
                catch (Win32Exception) { }
            }
            return null;
        }
    }

    // Starts a tool with Python in a hidden console and passes each output line to onLine on a worker thread.
    public static Process StartPython(string script, string arguments, Action<string> onLine)
    {
        var start = new ProcessStartInfo(Python[0], string.Join(" ", Python.Skip(1)) + " -u \"" + Path.Combine(Root, script) + "\" " + arguments)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Root,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        start.EnvironmentVariables["PYTHONUTF8"] = "1";
        var process = new Process { StartInfo = start };
        process.OutputDataReceived += delegate (object sender, DataReceivedEventArgs e) { if (e.Data != null) onLine(e.Data); };
        process.ErrorDataReceived += delegate (object sender, DataReceivedEventArgs e) { if (e.Data != null) onLine(e.Data); };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        return process;
    }

    // Plays with the saved settings and waits for the game to exit; returns why it failed, or null.
    public static string Play()
    {
        if (!File.Exists(Runner)) return "The game is not built yet. Build it in Launcher.exe first.";
        if (Python == null) return PythonMessage;
        string logs = Private("play-logs");
        Directory.CreateDirectory(logs);
        string log = Path.Combine(logs, "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + ".log");
        int code;
        using (var writer = new StreamWriter(log, false, new UTF8Encoding(false)))
        {
            var sync = new object();
            using (Process game = StartPython(@"tools\run_game.py", "--play --saved", line => { lock (sync) writer.WriteLine(line); }))
            {
                game.WaitForExit();
                code = game.ExitCode;
            }
        }
        if (code == 0) return null;
        string[] lines = File.ReadAllLines(log);
        return "The game stopped with exit code " + code + ".\n\n" + string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 20)))
            + "\n\nFull log: " + log;
    }
}

// Xbox controllers through XInput.
static class Pad
{
    [StructLayout(LayoutKind.Sequential)]
    struct State { public uint Packet; public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }
    [DllImport("xinput1_4.dll")] static extern int XInputGetState(int user, out State state);
    // Buttons of every connected controller, with the left stick folded into the D-pad bits.
    public static int Buttons()
    {
        int all = 0;
        for (int i = 0; i < 4; i++)
        {
            State s;
            if (XInputGetState(i, out s) != 0) continue;
            int b = s.Buttons;
            if (s.LY > 16000) b |= 1;
            if (s.LY < -16000) b |= 2;
            if (s.LX < -16000) b |= 4;
            if (s.LX > 16000) b |= 8;
            all |= b;
        }
        return all;
    }
}

class Row
{
    public string Label, Info, Action;
    public string[] Names;
    public int Index, Default;
}

class Page
{
    public string Name, Info, Action;
    public Row[] Rows;
}

class LauncherForm : Form
{
    static readonly int[] Heights = { 480, 720, 1080, 1440, 2160 };
    static readonly int[] Samples = { 1, 2, 4, 8, 16, 32 };
    const int BuildPage = 3, PlayPage = 4;
    // Horizon Bar layout on a 960x540 window: letterbox bars, the settings and page tabs in the bottom one.
    const int W = 960, H = 540, TopH = 44, Side = 30, FadeY = 334;
    const int InfoY = 380, LabelY = 414, ValueY = 434, ValueH = 30, TabY = 494, TabH = 26;

    readonly Row resolution, msaa, smaa, screen, surround, intro, chooseGame;
    readonly Page[] pages;
    // The menu cursor picks a page; inside a page the row cursor picks a setting.
    int menu, row;
    bool inPage;
    // A notice (build progress, a refused action) replaces the information text until the next input.
    string notice;
    string gameSource;
    bool built = File.Exists(Game.Runner);
    Process build;
    DateTime buildStart;
    readonly List<string> buildLines = new List<string>();
    readonly string buildLog = Game.Private("build.log");
    readonly Timer timer = new Timer { Interval = 500 };
    readonly Timer padTimer = new Timer { Interval = 50 };
    int padHeld;
    DateTime padRepeat = DateTime.MaxValue;
    // A held Down that entered a page waits for a fresh press before it changes a value.
    bool downEntered;
    bool downHeld;
    Point? drag;
    RectangleF[] tabs;

    readonly Bitmap background = new Bitmap(W, H);
    readonly Font itemFont = new Font("Arial", 15, FontStyle.Bold | FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font menuFont = new Font("Arial", 19, FontStyle.Bold | FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font tabFont = new Font("Arial", 14, FontStyle.Bold | FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font infoFont = new Font("Arial", 14, FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font smallFont = new Font("Arial", 12, FontStyle.Bold, GraphicsUnit.Pixel);
    readonly Font titleFont = new Font("Arial", 25, FontStyle.Bold | FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font logoFont = new Font("Arial Black", 30, FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font bigFont = new Font("Arial Black", 54, FontStyle.Italic, GraphicsUnit.Pixel);
    readonly Font ultimateFont = new Font("Arial", 21, FontStyle.Bold | FontStyle.Italic, GraphicsUnit.Pixel);
    readonly StringFormat far = new StringFormat { Alignment = StringAlignment.Far, LineAlignment = StringAlignment.Center };
    readonly StringFormat near = new StringFormat { Alignment = StringAlignment.Near, LineAlignment = StringAlignment.Center };
    readonly StringFormat center = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };

    static int Nearest(int h) { return Math.Max(0, Heights.Count(x => x <= h) - 1); }
    static Row Choice(string label, string info, string[] names, bool second, int fallback)
    {
        return new Row { Label = label, Info = info, Names = names, Index = second ? 1 : 0, Default = fallback };
    }

    public LauncherForm(string disc)
    {
        var settings = Game.LoadSettings();
        int screenHeight = Screen.PrimaryScreen.Bounds.Height;
        resolution = new Row { Label = "Resolution", Info = "Sets the render resolution. 480p opens in a window",
            Names = Heights.Select(h => h + "p").ToArray(), Index = Nearest(Game.Int(settings, "height", screenHeight)), Default = Nearest(screenHeight) };
        int savedMsaa = Array.IndexOf(Samples, Game.Int(settings, "msaa", 32));
        msaa = new Row { Label = "MSAA", Info = "Smooths polygon edges; a count the GPU lacks falls back to its highest",
            Names = new[] { "Off", "2x", "4x", "8x", "16x", "32x" }, Index = savedMsaa < 0 ? Samples.Length - 1 : savedMsaa, Default = Samples.Length - 1 };
        smaa = Choice("SMAA", "Smooths edges with SMAA Ultra after MSAA", new[] { "Off", "On" }, Game.Bool(settings, "smaa", true), 1);
        screen = Choice("Screen", "16:9 uses the game's own widescreen camera and menus", new[] { "4:3", "16:9" }, Game.Bool(settings, "widescreen", true), 1);
        surround = Choice("Output", "5.1 plays the game's own surround mix", new[] { "Stereo", "5.1" }, Game.Bool(settings, "surround", false), 0);
        intro = Choice("Intro Movies", "Skip goes straight to the title screen at boot", new[] { "Play", "Skip" }, Game.Bool(settings, "skipIntro", false), 0);
        gameSource = Game.Str(settings, "game_source", null);
        chooseGame = new Row { Label = "Disc 2 ISO", Info = SourceName(), Action = "game" };
        var buildRow = new Row { Label = "Build", Info = "Lifts and compiles the game from your ISO; takes about 15 minutes", Action = "build" };
        pages = new[] {
            new Page { Name = "VIDEO SETTINGS", Info = "Sets resolution, anti-aliasing and screen shape", Rows = PageRows(resolution, msaa, smaa, screen) },
            new Page { Name = "AUDIO SETTINGS", Info = "Chooses stereo or 5.1 surround", Rows = PageRows(surround) },
            new Page { Name = "MOVIES", Info = "Plays or skips the logo and opening movies at boot", Rows = PageRows(intro) },
            new Page { Name = "BUILD GAME", Info = BuildStatus(), Rows = new[] { chooseGame, buildRow, new Row { Label = "Exit", Info = "Returns to the menu", Action = "back" } } },
            new Page { Name = "START GAME", Info = "Saves these settings and starts the game", Rows = new Row[0], Action = "play" },
            new Page { Name = "EXIT", Info = "Closes the launcher", Rows = new Row[0], Action = "exit" },
        };
        menu = built ? PlayPage : BuildPage;

        Text = Game.Title;
        // The exe's own icon, when the build embedded private\launcher-icon.ico, also marks the window and taskbar.
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        BackColor = Color.Black;
        ClientSize = new Size(W, H);
        DoubleBuffered = true;
        AllowDrop = true;
        DrawBackground();
        timer.Tick += delegate { BuildTick(); };
        padTimer.Tick += delegate { PadTick(); };
        // Buttons already down at start (the press that opened the launcher) wait for a release.
        try { padHeld = Pad.Buttons(); padTimer.Start(); } catch (DllNotFoundException) { }
        if (disc != null) UseDisc(disc);
    }

    static Row[] PageRows(params Row[] rows)
    {
        return rows.Concat(new[] {
            new Row { Label = "Default Setting", Info = "Restores the defaults on this page", Action = "default" },
            new Row { Label = "Exit", Info = "Returns to the menu", Action = "back" } }).ToArray();
    }

    string SourceName()
    {
        return Game.IsIso(gameSource) ? "Disc: " + Path.GetFileName(gameSource) : "Choose or drop your Dead or Alive Ultimate Disc 2 ISO";
    }
    string BuildStatus()
    {
        return built ? "Built. Build again after an update" : "Not built yet: choose your Disc 2 ISO, then Build";
    }

    public Dictionary<string, object> Settings()
    {
        return new Dictionary<string, object> {
            { "height", Heights[resolution.Index] }, { "msaa", Samples[msaa.Index] }, { "smaa", smaa.Index == 1 },
            { "widescreen", screen.Index == 1 }, { "skipIntro", intro.Index == 1 }, { "surround", surround.Index == 1 },
            { "game_source", gameSource } };
    }

    static Color Rgb(int r, int g, int b, int a = 255) { return Color.FromArgb(a, r, g, b); }
    int ColW { get { return (W - 2 * Side) / Math.Max(Rows.Length, 1); } }
    static string Verb(string action) { return action == "build" ? "BUILD" : action == "default" ? "RESET" : action == "back" ? "BACK" : "CHOOSE"; }

    // The generated beach (tools/launcher-background.jpg, embedded by package_test.py) between letterbox bars.
    // A private\launcher-background.png or .jpg replaces it; it is read at run time so private art never enters the exe.
    void DrawBackground()
    {
        string own = new[] { "launcher-background.png", "launcher-background.jpg" }.Select(Game.Private).FirstOrDefault(File.Exists);
        using (var bg = Graphics.FromImage(background))
        using (var stream = own != null ? File.OpenRead(own) : typeof(LauncherForm).Assembly.GetManifestResourceStream("launcher-background.jpg"))
        {
            bg.InterpolationMode = InterpolationMode.HighQualityBicubic;
            if (stream != null) using (var image = Image.FromStream(stream)) bg.DrawImage(image, 0, 0, W, H);
            else bg.Clear(Rgb(74, 60, 51));
            bg.FillRectangle(new SolidBrush(Rgb(13, 10, 9)), 0, 0, W, TopH);
            var fade = new LinearGradientBrush(new Rectangle(0, FadeY - 1, W, H - FadeY + 2), Color.Black, Color.Black, 90f);
            fade.InterpolationColors = new ColorBlend { Colors = new[] { Rgb(13, 10, 9, 0), Rgb(13, 10, 9, 230), Rgb(13, 10, 9) }, Positions = new[] { 0f, .26f, 1f } };
            bg.FillRectangle(fade, 0, FadeY, W, H - FadeY);
        }
    }

    // Brushed silver, as on the title logo.
    static Brush Chrome(float y, float h)
    {
        var brush = new LinearGradientBrush(new RectangleF(0, y, 10, h), Color.White, Color.Gray, 90f);
        brush.InterpolationColors = new ColorBlend {
            Colors = new[] { Rgb(251, 251, 249), Rgb(212, 215, 216), Rgb(138, 143, 146), Rgb(201, 204, 204), Rgb(240, 240, 238) },
            Positions = new[] { 0f, .38f, .52f, .7f, 1f } };
        return brush;
    }

    static void Shadowed(Graphics g, string text, Font font, Brush brush, RectangleF rect, StringFormat format, int d)
    {
        g.DrawString(text, font, new SolidBrush(Rgb(30, 22, 18, 220)), new RectangleF(rect.X + d, rect.Y + d, rect.Width, rect.Height), format);
        g.DrawString(text, font, brush, rect, format);
    }

    // A triangle pointing along dx, dy from its base centred on x, y.
    static void Arrow(Graphics g, float x, float y, int dx, int dy, Color color)
    {
        g.FillPolygon(new SolidBrush(color), new[] { new PointF(x + 7 * dx, y + 7 * dy), new PointF(x - 5 * dy, y - 5 * dx), new PointF(x + 5 * dy, y + 5 * dx) });
    }

    static GraphicsPath Pill(float x, float y, float w, float h)
    {
        var path = new GraphicsPath();
        path.AddArc(x, y, h, h, 90, 180);
        path.AddArc(x + w - h, y, h, h, 270, 180);
        path.CloseFigure();
        return path;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.AntiAlias;
        g.DrawImage(background, 0, 0);
        Color sand = Rgb(241, 197, 143), dark = Rgb(42, 29, 21), muted = Rgb(191, 178, 165);

        // Top letterbox bar: title and build state on the left, button prompts on the right.
        g.DrawString("DEAD OR ALIVE 2 ULTIMATE  \u00B7  " + (built ? "READY TO PLAY" : "NOT BUILT YET"),
            smallFont, new SolidBrush(muted), new RectangleF(Side, 0, 500, TopH), near);
        float x = W - Side;
        foreach (var hint in new[] { new[] { "", "CHANGE" }, new[] { "B", "BACK" }, new[] { "A", "SELECT" } })
        {
            float textW = g.MeasureString(hint[1], smallFont).Width;
            x -= textW;
            g.DrawString(hint[1], smallFont, new SolidBrush(muted), new RectangleF(x, 0, textW + 4, TopH), near);
            float w = hint[0] == "" ? 26 : 20;
            x -= w + 6;
            g.FillPath(new SolidBrush(Rgb(242, 227, 207)), Pill(x, TopH / 2 - 10, w, 20));
            if (hint[0] == "") { Arrow(g, x + 9, TopH / 2 + 2, 0, -1, dark); Arrow(g, x + 17, TopH / 2 - 2, 0, 1, dark); }
            else g.DrawString(hint[0], smallFont, new SolidBrush(dark), new RectangleF(x, TopH / 2 - 10, w, 20), center);
            x -= 22;
        }

        // Logo in the sky right of the moon, sized to stay clear of it.
        float right = W - 44, two = g.MeasureString("2", bigFont).Width;
        Shadowed(g, "2", bigFont, Chrome(60, 72), new RectangleF(0, 60, right, 74), far, 3);
        Shadowed(g, "DEAD OR ALIVE", logoFont, Chrome(78, 44), new RectangleF(0, 78, right - two + 4, 46), far, 3);
        Shadowed(g, "ULTIMATE", ultimateFont, Chrome(124, 30), new RectangleF(0, 124, right - 4, 30), far, 2);

        // The chosen page in the bottom bar: its information line, then its settings side by side.
        Page page = pages[menu];
        Row[] rows = page.Rows;
        string info = notice ?? (inPage ? rows[row].Info : page.Info);
        g.DrawString(info, infoFont, new SolidBrush(Rgb(216, 202, 188)), new RectangleF(Side, InfoY, W - 2 * Side, 22), near);
        if (page.Action == "play") Shadowed(g, "PRESS START BUTTON", titleFont, Chrome(ValueY - 8, 40), new RectangleF(Side, ValueY - 8, W - 2 * Side, 40), near, 2);
        var divider = new Pen(Rgb(255, 255, 255, 36));
        int colW = ColW;
        for (int i = 0; i < rows.Length; i++)
        {
            Row r = rows[i];
            int left = Side + i * colW;
            bool selected = inPage && i == row;
            if (selected) g.FillRectangle(new SolidBrush(Rgb(232, 191, 138, 45)), left, LabelY - 6, colW, ValueY + ValueH + 6 - LabelY);
            if (i > 0) g.DrawLine(divider, left, LabelY - 2, left, ValueY + ValueH);
            g.DrawString(r.Label.ToUpperInvariant(), smallFont, new SolidBrush(selected ? Color.White : Rgb(198, 182, 166)), new RectangleF(left + 12, LabelY, colW - 16, 18), near);
            var valueRect = new RectangleF(left + 26, ValueY, colW - 30, ValueH);
            if (r.Names == null) { g.DrawString(Verb(r.Action), itemFont, new SolidBrush(sand), new RectangleF(left + 12, ValueY, colW - 16, ValueH), near); continue; }
            string value = r.Names[r.Index].ToUpperInvariant();
            float cy = ValueY + ValueH / 2f;
            Color arrow = selected ? sand : Rgb(241, 197, 143, 130);
            Arrow(g, left + 19, cy, -1, 0, arrow);
            Arrow(g, left + 30 + g.MeasureString(value, menuFont).Width, cy, 1, 0, arrow);
            g.DrawString(value, menuFont, new SolidBrush(selected ? Color.White : Rgb(232, 222, 212)), valueRect, near);
        }

        // Page tabs along the bottom, the current one underlined in sunset gold.
        g.DrawLine(divider, Side, TabY - 8, W - Side, TabY - 8);
        tabs = new RectangleF[pages.Length];
        float tx = Side;
        for (int m = 0; m < pages.Length; m++)
        {
            float w = g.MeasureString(pages[m].Name, tabFont).Width;
            tabs[m] = new RectangleF(tx, TabY, w, TabH);
            g.DrawString(pages[m].Name, tabFont, new SolidBrush(m == menu ? Rgb(255, 243, 228) : Rgb(169, 156, 144)), tabs[m], near);
            if (m == menu) g.FillRectangle(new LinearGradientBrush(new RectangleF(tx - 1, 0, w + 2, 1), Rgb(247, 220, 181), Rgb(201, 138, 85), 0f), tx, TabY + TabH, w, 3);
            tx += w + 18;
        }
    }

    void StartBuild()
    {
        if (build != null) return;
        if (!Game.IsIso(gameSource)) { notice = "Choose your Disc 2 ISO first"; return; }
        if (Game.Python == null) { MessageBox.Show(Game.PythonMessage, Game.Title); return; }
        Game.SaveSettings(Settings());
        lock (buildLines) buildLines.Clear();
        buildStart = DateTime.Now;
        build = Game.StartPython(@"tools\build_game.py", "\"" + gameSource + "\"", line => { lock (buildLines) buildLines.Add(line); });
        notice = "Starting the build";
        timer.Start();
    }

    void BuildTick()
    {
        string last;
        lock (buildLines) last = buildLines.LastOrDefault();
        notice = "Building " + (DateTime.Now - buildStart).ToString(@"mm\:ss") + (last != null ? "  \u00B7  " + last.Trim() : "");
        if (build.HasExited) { timer.Stop(); FinishBuild(); }
        Invalidate();
    }

    void FinishBuild()
    {
        // Waits for the redirected output to drain.
        build.WaitForExit();
        int code = build.ExitCode;
        build.Dispose();
        build = null;
        string[] lines;
        lock (buildLines) lines = buildLines.ToArray();
        try { File.WriteAllLines(buildLog, lines); } catch (IOException) { }
        built = File.Exists(Game.Runner);
        pages[BuildPage].Info = BuildStatus();
        if (code == 0) { notice = "Build complete. Choose START GAME to play"; menu = PlayPage; inPage = false; return; }
        notice = @"Build failed; see private\build.log";
        MessageBox.Show("The build failed:\n\n" + string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 8))) + "\n\nFull output: " + buildLog, Game.Title);
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        base.OnFormClosing(e);
        if (build == null || build.HasExited) return;
        if (MessageBox.Show("Stop the build?", Game.Title, MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
        timer.Stop();
        using (var kill = Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + build.Id) { UseShellExecute = false, CreateNoWindow = true }))
            kill.WaitForExit();
    }

    static int Clamp(int value, int max) { return Math.Min(Math.Max(value, 0), max); }
    Row[] Rows { get { return pages[menu].Rows; } }

    void MoveCursor(int delta)
    {
        if (inPage) row = Clamp(row + delta, Rows.Length - 1);
        else menu = Clamp(menu + delta, pages.Length - 1);
    }

    void ChangeValue(int delta)
    {
        if (!inPage) return;
        Row r = Rows[row];
        if (r.Names != null) r.Index = Clamp(r.Index + delta, r.Names.Length - 1);
    }

    void Accept()
    {
        Page page = pages[menu];
        if (!inPage)
        {
            if (page.Action == "play" && build != null) notice = "Wait for the build to finish";
            else if (page.Action == "play" && !built) { menu = BuildPage; notice = "Build the game first"; }
            else if (page.Action != null) { Tag = page.Action; Close(); return; }
            else { inPage = true; row = 0; }
        }
        else
        {
            Row r = page.Rows[row];
            if (r.Action == "default") { foreach (Row each in page.Rows) if (each.Names != null) each.Index = each.Default; }
            else if (r.Action == "back") inPage = false;
            else if (r.Action == "game")
            {
                if (build != null) { notice = "Wait for the build to finish"; return; }
                using (var dialog = new OpenFileDialog { Title = "Dead or Alive Ultimate Disc 2 ISO", Filter = "Xbox ISO|*.iso|All files|*.*" })
                    if (dialog.ShowDialog() == DialogResult.OK) { gameSource = dialog.FileName; r.Info = SourceName(); }
            }
            else if (r.Action == "build") StartBuild();
        }
        Invalidate();
    }

    void Back()
    {
        if (inPage) inPage = false;
        else Close();
    }

    void Press(Keys key, bool repeat = false)
    {
        if (build == null) notice = null;
        if (key == Keys.Down && !repeat) downEntered = false;
        switch (key)
        {
            case Keys.Left: MoveCursor(-1); break;
            case Keys.Right: MoveCursor(1); break;
            case Keys.Up: if (inPage) ChangeValue(1); break;
            case Keys.Down:
                if (!inPage) { if (Rows.Length > 0) { Accept(); downEntered = true; } }
                else if (!downEntered) ChangeValue(-1);
                break;
            case Keys.Return: Accept(); break;
            case Keys.Escape: case Keys.Back: Back(); break;
        }
        Invalidate();
    }

    // A form without controls would otherwise spend the arrow keys on focus moves.
    protected override bool ProcessDialogKey(Keys keyData) { return false; }
    // Windows repeats WM_KEYDOWN while a key is held; Down stays held until its WM_KEYUP or focus leaves.
    protected override void OnKeyDown(KeyEventArgs e) { base.OnKeyDown(e); Press(e.KeyCode, e.KeyCode == Keys.Down && downHeld); if (e.KeyCode == Keys.Down) downHeld = true; }
    protected override void OnKeyUp(KeyEventArgs e) { base.OnKeyUp(e); if (e.KeyCode == Keys.Down) downHeld = false; }
    protected override void OnDeactivate(EventArgs e) { base.OnDeactivate(e); downHeld = false; }

    // A dropped ISO becomes the game disc and selects Build.
    void UseDisc(string path)
    {
        if (build != null) { notice = "Wait for the build to finish"; return; }
        gameSource = path;
        chooseGame.Info = SourceName();
        menu = BuildPage; inPage = true; row = 1;
        notice = SourceName() + ". Press Build";
    }

    static string Dropped(DragEventArgs e)
    {
        var files = e.Data.GetData(DataFormats.FileDrop) as string[];
        return files != null && files.Length > 0 && Game.IsIso(files[0]) ? files[0] : null;
    }
    protected override void OnDragEnter(DragEventArgs e) { if (Dropped(e) != null) e.Effect = DragDropEffects.Copy; }
    protected override void OnDragDrop(DragEventArgs e)
    {
        string path = Dropped(e);
        if (path != null) { UseDisc(path); Activate(); Invalidate(); }
    }

    // D-pad or left stick, A or START, B or BACK. Directions repeat while held.
    static readonly int[] PadBits = { 0x1, 0x2, 0x4, 0x8, 0x1000, 0x10, 0x2000, 0x20 };
    static readonly Keys[] PadKeys = { Keys.Up, Keys.Down, Keys.Left, Keys.Right, Keys.Return, Keys.Return, Keys.Escape, Keys.Escape };
    void PadTick()
    {
        // Only while the launcher has focus, so a controller used elsewhere during a build is ignored.
        if (ActiveForm != this) return;
        int buttons = Pad.Buttons();
        DateTime now = DateTime.Now;
        int held = padHeld;
        padHeld = buttons;
        for (int i = 0; i < PadBits.Length; i++)
        {
            int bit = PadBits[i];
            if ((buttons & bit) == 0) continue;
            bool pressed = (held & bit) == 0;
            if (!pressed && (bit > 0x8 || now < padRepeat)) continue;
            padRepeat = now.AddMilliseconds(pressed ? 400 : 120);
            Press(PadKeys[i], !pressed);
            return;
        }
    }

    int HitTab(int x, int y)
    {
        if (tabs == null) return -1;
        for (int m = 0; m < tabs.Length; m++) if (tabs[m].Contains(x, y)) return m;
        return -1;
    }
    int HitColumn(int x, int y)
    {
        if (y < LabelY - 6 || y > ValueY + ValueH || x < Side) return -1;
        int i = (x - Side) / ColW;
        return i < Rows.Length ? i : -1;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        // Borderless, so a drag on the top bar moves the window.
        if (drag.HasValue) { Location = new Point(Left + e.X - drag.Value.X, Top + e.Y - drag.Value.Y); return; }
        int m = HitTab(e.X, e.Y), i = HitColumn(e.X, e.Y);
        if (!inPage && m >= 0 && m != menu) { menu = m; Invalidate(); }
        if (inPage && i >= 0 && i != row) { row = i; Invalidate(); }
    }
    protected override void OnMouseDown(MouseEventArgs e) { if (e.Y < TopH) drag = e.Location; }
    protected override void OnMouseUp(MouseEventArgs e) { drag = null; }

    protected override void OnMouseClick(MouseEventArgs e)
    {
        if (build == null) notice = null;
        int x = e.X, m = HitTab(x, e.Y), i = HitColumn(x, e.Y);
        if (e.Button == MouseButtons.Right) { Back(); Invalidate(); return; }
        if (m >= 0) { menu = m; inPage = false; Accept(); return; }
        if (i < 0) return;
        // From the tabs, the first click only enters the page and selects the setting.
        bool entering = !inPage;
        inPage = true; row = i;
        if (entering) { Invalidate(); return; }
        if (Rows[i].Action != null) Accept();
        else ChangeValue(x < Side + i * ColW + ColW / 2 ? -1 : 1);
        Invalidate();
    }
}

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        try
        {
#if PLAY_ONLY
            if (!File.Exists(Game.Runner)) { Process.Start(Path.Combine(Game.Root, "Launcher.exe")); return; }
#else
            var form = new LauncherForm(args.Length > 0 && Game.IsIso(args[0]) ? args[0] : null);
            Application.Run(form);
            if (form.Tag as string != "play") return;
            Game.SaveSettings(form.Settings());
#endif
            string error = Game.Play();
            if (error != null) MessageBox.Show(error, Game.Title);
        }
        catch (Exception e) { MessageBox.Show(e.Message, Game.Title); }
    }
}
