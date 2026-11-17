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
static class EditorChecks
{
    static object Call(string name, params object[] args)
    {
        return typeof(JukeboxEditor).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static).Invoke(null, args);
    }
    static void Check(bool ok) { if (!ok) throw new Exception("Editor regression failed"); }
    static void Main()
    {
        string path = Path.GetFullPath("assignments.txt");
        Call("AtomicText", path, "one\tDOA3\told.adx\ntwo\tDOA4\tgame.adx\nthree\tDOA3\tclear.adx\n");
        var picks = new Dictionary<string, string> { {"one", "DOA4\tedited.adx"}, {"two", "DOA3\tstale.adx"} };
        var merged = (Dictionary<string, string>)Call("MergeAssignments", path, picks, new[] {"one", "three"});
        Check(merged["one"] == "DOA4\tedited.adx" && merged["two"] == "DOA4\tgame.adx" && !merged.ContainsKey("three"));
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
