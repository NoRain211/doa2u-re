// SPDX-License-Identifier: GPL-3.0-or-later
// macOS launcher: pick resolution, anti-aliasing, widescreen, the intro skip
// and 5.1 surround, remember them in private/launcher.json, then exit 0 so launcher.sh
// starts the game. Mirrors tools/launcher.cs, with MSAA up to SDL_GPU's 8x.
// Exits 1 if the window is closed.
// Adapted from the DOA3 launcher (NoRain211/doa3-re tools/launcher_macos.swift).
import AppKit

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1
    ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath)
let settingsURL = root.appendingPathComponent("private/launcher.json")

let heights = [480, 720, 1080, 1440, 2160]
let samples = [1, 2, 4, 8]

var settings: [String: Any] = [:]
if let data = try? Data(contentsOf: settingsURL),
   let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
    settings = json
}
func intSetting(_ key: String, _ fallback: Int) -> Int {
    (settings[key] as? NSNumber)?.intValue ?? fallback
}
func boolSetting(_ key: String, _ fallback: Bool) -> Bool {
    (settings[key] as? NSNumber)?.boolValue ?? fallback
}
let screenHeight: Int = {
    guard let screen = NSScreen.screens.first ?? NSScreen.main else { return 1080 }
    return Int(screen.frame.height * screen.backingScaleFactor)
}()

final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 280, height: 248),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
    let resolution = NSPopUpButton()
    let msaa = NSPopUpButton()
    let smaa = NSButton(checkboxWithTitle: "SMAA Ultra", target: nil, action: nil)
    let widescreen = NSButton(checkboxWithTitle: "Widescreen (16:9)", target: nil, action: nil)
    let skipIntro = NSButton(checkboxWithTitle: "Skip intro movies", target: nil, action: nil)
    let surround = NSButton(checkboxWithTitle: "Surround 5.1", target: nil, action: nil)
    var launched = false

    func addRow(_ title: String, _ popup: NSPopUpButton, _ items: [String], _ index: Int, _ y: CGFloat) {
        let label = NSTextField(labelWithString: title)
        label.frame = NSRect(x: 16, y: y + 3, width: 84, height: 20)
        popup.frame = NSRect(x: 104, y: y, width: 160, height: 26)
        popup.addItems(withTitles: items)
        popup.selectItem(at: max(0, index))
        window.contentView!.addSubview(label)
        window.contentView!.addSubview(popup)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        window.title = "Dead or Alive 2 Ultimate"
        window.delegate = self
        // launcher.cs's defaults: the screen height, the most MSAA, widescreen on.
        let savedHeight = intSetting("height", screenHeight)
        let nearest = heights.lastIndex(where: { $0 <= savedHeight }) ?? 0
        addRow("Resolution", resolution,
               heights.map { $0 == 480 ? "480p (windowed)" : $0 == 2160 ? "2160p (4K)" : "\($0)p" },
               nearest, 206)
        let savedMsaa = min(intSetting("msaa", 8), 8)
        addRow("MSAA", msaa, samples.map { $0 == 1 ? "Off" : "\($0)x" },
               samples.lastIndex(where: { $0 <= savedMsaa }) ?? 0, 174)
        smaa.frame = NSRect(x: 104, y: 146, width: 170, height: 20)
        smaa.state = boolSetting("smaa", true) ? .on : .off
        widescreen.frame = NSRect(x: 104, y: 120, width: 170, height: 20)
        widescreen.state = boolSetting("widescreen", true) ? .on : .off
        skipIntro.frame = NSRect(x: 104, y: 94, width: 170, height: 20)
        skipIntro.state = boolSetting("skipIntro", false) ? .on : .off
        surround.frame = NSRect(x: 104, y: 68, width: 170, height: 20)
        surround.state = boolSetting("surround", false) ? .on : .off
        let note = NSTextField(labelWithString: "Unsupported MSAA falls back to GPU support.")
        note.font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 16, y: 44, width: 256, height: 16)

        let play = NSButton(title: "Play", target: self, action: #selector(play))
        play.frame = NSRect(x: 180, y: 8, width: 88, height: 30)
        play.keyEquivalent = "\r"
        for view in [smaa, widescreen, skipIntro, surround, note, play] { window.contentView!.addSubview(view) }

        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func play() {
        let chosen: [String: Any] = [
            "height": heights[resolution.indexOfSelectedItem],
            "msaa": samples[msaa.indexOfSelectedItem],
            "smaa": smaa.state == .on,
            "widescreen": widescreen.state == .on,
            "skipIntro": skipIntro.state == .on,
            "surround": surround.state == .on,
        ]
        do {
            try FileManager.default.createDirectory(
                at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try JSONSerialization.data(
                withJSONObject: chosen, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: settingsURL)
        } catch {
            NSAlert(error: error).runModal()
            return
        }
        launched = true
        NSApp.terminate(nil)
    }

    func windowWillClose(_ notification: Notification) {
        if !launched { exit(1) }
    }

    // AppKit exits 0 after terminate, so a Dock or Cmd-Q quit would start the game.
    func applicationWillTerminate(_ notification: Notification) {
        exit(launched ? 0 : 1)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let launcher = Launcher()
app.delegate = launcher
app.run()
exit(launcher.launched ? 0 : 1)
