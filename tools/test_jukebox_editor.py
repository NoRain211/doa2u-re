# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the editor's persistence and archive checks without opening a window."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class EditorTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "win32", "uses the Windows .NET Framework compiler")
    def test_persistence_and_malformed_archive(self):
        compiler = Path(os.environ.get("WINDIR", "C:/Windows")) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
        if not compiler.is_file():
            self.skipTest(".NET Framework compiler unavailable")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness = root / "checks.cs"
            harness.write_text(r'''
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization;
using System.Windows.Forms;
static class EditorChecks
{
    static object Call(string name, params object[] args)
    {
        return typeof(JukeboxEditor).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static).Invoke(null, args);
    }
    static object Run(JukeboxEditor editor, string name, params object[] args)
    {
        return typeof(JukeboxEditor).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(editor, args);
    }
    static object Get(object target, string name)
    {
        return typeof(JukeboxEditor).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).GetValue(target);
    }
    static void Set(object target, string name, object value)
    {
        typeof(JukeboxEditor).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).SetValue(target, value);
    }
    static void Check(bool ok) { if (!ok) throw new Exception("Editor regression failed"); }
    static void Main()
    {
        string path = Path.GetFullPath("assignments.txt");
        Call("AtomicText", path, "one\tDOA3\told.adx\ntwo\tDOA4\tgame.adx\nthree\tDOA3\tclear.adx\n");
        var picks = new Dictionary<string, string> { {"one", "DOA4\tedited.adx"}, {"two", "DOA3\tstale.adx"} };
        var merged = (Dictionary<string, string>)Call("MergeAssignments", path, picks, new[] {"one", "three"});
        Check(merged["one"] == "DOA4\tedited.adx" && merged["two"] == "DOA4\tgame.adx" && !merged.ContainsKey("three"));
        Check((string)Call("RecordText", "one\r\ntwo\tthree = four") == "one  two three = four");

        // Two tracks can share a name: the chosen row keeps its track, not the last matching text.
        var picker = (JukeboxEditor)FormatterServices.GetUninitializedObject(typeof(JukeboxEditor));
        var shared = new Album { Tab = "DOA4", Title = "Dead or Alive 4", Folder = "doa4" };
        shared.Tracks.Add(new Track { File = "a.adx", Name = "Same" });
        shared.Tracks.Add(new Track { File = "b.adx", Name = "Same" });
        Set(picker, "albums", new List<Album> { shared });
        Set(picker, "assigned", new Dictionary<string, string>());
        Set(picker, "editedSlots", new HashSet<string>());
        var slots = new DataGridView();
        slots.Columns.Add(new DataGridViewTextBoxColumn());
        Type choice = typeof(JukeboxEditor).GetNestedType("Choice", BindingFlags.NonPublic);
        slots.Columns.Add(new DataGridViewComboBoxColumn { ValueType = choice });
        Set(picker, "slots", slots);
        Run(picker, "BuildSlots");
        object chosen = null;
        foreach (object item in ((DataGridViewComboBoxColumn)slots.Columns[1]).Items)
        {
            var track = (Track)choice.GetField("Track").GetValue(item);
            if (track != null && track.File == "a.adx") chosen = item;
        }
        Check(chosen != null && Convert.ToString(chosen) == (string)Call("TrackLabel", shared, shared.Tracks[0]));
        slots.Rows[0].Cells[1].Value = chosen;
        Run(picker, "SlotEdited", null, new DataGridViewCellEventArgs(1, 0));
        Check(((Dictionary<string, string>)Get(picker, "assigned"))["bgm_doa2_ks.adx"] == "DOA4\ta.adx");

        using (var locked = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            bool failed = false;
            try { Call("AtomicText", path, "partial"); }
            catch (TargetInvocationException) { failed = true; }
            Check(failed);
        }
        Check(File.ReadAllText(path).Contains("game.adx"));
        Check(Directory.GetFiles(".", "*.tmp.*").Length == 0);
        Call("AtomicText", path, "complete");
        Check(File.ReadAllText(path) == "complete");
        Directory.CreateDirectory("disc/DOA2");
        var editor = (JukeboxEditor)FormatterServices.GetUninitializedObject(typeof(JukeboxEditor));
        typeof(JukeboxEditor).GetField("disc", BindingFlags.NonPublic | BindingFlags.Instance).SetValue(editor, Path.GetFullPath("disc"));
        foreach (uint count in new uint[] {0, 0xffffffff, 4097, 1})
        {
            byte[] bytes = new byte[24];
            Array.Copy(new byte[] {65,70,83,0}, bytes, 4);
            Array.Copy(BitConverter.GetBytes(count), 0, bytes, 4, 4);
            File.WriteAllBytes("disc/DOA2/bgm.afs", bytes);
            var album = (Album)typeof(JukeboxEditor).GetMethod("LoadDoa2u", BindingFlags.NonPublic | BindingFlags.Instance).Invoke(editor, null);
            Check(album.Tracks.Count == 0);
        }
        Console.WriteLine("Editor persistence and archive checks passed");
    }
}
''', encoding="utf-8", newline="\n")
            exe = root / "checks.exe"
            source = Path(__file__).with_name("jukebox_editor.cs")
            compiled = subprocess.run([str(compiler), "/nologo", "/target:exe", "/main:EditorChecks",
                                       f"/out:{exe}", str(source), str(harness)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            checked = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
