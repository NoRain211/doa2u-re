// SPDX-License-Identifier: GPL-3.0-or-later
// macOS launcher: pick resolution, anti-aliasing, widescreen, 5.1 surround and
// the intro skip, remember them in private/launcher.json, then exit 0 so
// launcher.sh starts the game. Mirrors tools/launcher.cs without the build page
// (./build_game.sh builds) and with MSAA up to SDL_GPU's 8x: the backdrop
// between letterbox bars, the page tabs and the chosen page's settings in the
// bottom one. Adapted from committed DOA3 tools/launcher_macos.swift at ce393e4.
// Keyboard and mouse. Exits 1 if the window is closed.
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

func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: Int = 255) -> NSColor {
    NSColor(srgbRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: CGFloat(a) / 255)
}
func font(_ name: String, _ size: CGFloat) -> NSFont { NSFont(name: name, size: size) ?? .boldSystemFont(ofSize: size) }

// A setting with its choices.
final class Row {
    let label: String
    var info: String
    var names: [String]
    var index: Int
    // Set on a row that does something when chosen, instead of changing a value.
    var action: (() -> Void)?
    init(_ label: String, _ info: String, _ names: [String], _ index: Int) {
        self.label = label; self.info = info; self.names = names; self.index = max(0, index)
    }
}

struct Page { let name: String; let info: String; let rows: [Row] }

// Runs tools/update.py (check or install) off the main thread; done gets whether it succeeded and its last output line.
func runUpdate(_ command: String, done: @escaping (Bool, String) -> Void) {
    let process = Process()
    let output = Pipe()
    // The interpreter run_game.sh and build_game.sh prefer, else python3 on PATH.
    let script = root.appendingPathComponent("tools/update.py").path
    let venv = root.appendingPathComponent("private/venv/bin/python3").path
    if FileManager.default.isExecutableFile(atPath: venv) {
        process.executableURL = URL(fileURLWithPath: venv)
        process.arguments = [script, command]
    } else {
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", script, command]
    }
    process.standardOutput = output
    process.standardError = output
    process.terminationHandler = { finished in
        let text = String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let status = finished.terminationStatus
        let last = text.split(separator: "\n").last.map(String.init) ?? (status == 0 ? "" : "Update failed (exit \(status))")
        DispatchQueue.main.async { done(status == 0, last) }
    }
    do { try process.run() } catch { DispatchQueue.main.async { done(false, "Cannot run python3: \(error.localizedDescription)") } }
}

final class LauncherView: NSView {
    // Horizon Bar layout, as in tools/launcher.cs: letterbox bars, the settings and page tabs in the bottom one.
    static let topH: CGFloat = 44, side: CGFloat = 30, fadeY: CGFloat = 334
    static let infoY: CGFloat = 380, labelY: CGFloat = 414, valueY: CGFloat = 434, valueH: CGFloat = 30
    static let tabY: CGFloat = 494, tabH: CGFloat = 26
    static let playPage = 4, updatePage = 5, exitPage = 6

    // launcher.cs's defaults: the screen height, the most MSAA, SMAA and widescreen on.
    let resolution = Row("Resolution", "Sets the render resolution. 480p opens in a window",
                         heights.map { "\($0)p" }, heights.lastIndex(where: { $0 <= intSetting("height", screenHeight) }) ?? 0)
    // Windows saves up to 32x; SDL_GPU stops at 8x.
    let msaa = Row("MSAA", "Smooths polygon edges; a count the GPU lacks falls back to its highest",
                   samples.map { $0 == 1 ? "Off" : "\($0)x" }, samples.lastIndex(where: { $0 <= intSetting("msaa", 8) }) ?? 0)
    let smaa = Row("SMAA", "Smooths edges with SMAA after MSAA", ["Off", "On"], boolSetting("smaa", true) ? 1 : 0)
    let screen = Row("Screen", "16:9 uses the game's own widescreen camera and menus", ["4:3", "16:9"],
                     boolSetting("widescreen", true) ? 1 : 0)
    let surround = Row("Output", "5.1 plays the game's own surround mix", ["Stereo", "5.1"], boolSetting("surround", false) ? 1 : 0)
    let intro = Row("Intro Movies", "Skip goes straight to the title screen at boot", ["Play", "Skip"],
                    boolSetting("skipIntro", false) ? 1 : 0)
    let updateRow = Row("Update", "Choose Check to look for a newer release", ["Check"], 0)
    let checkAtStart = Row("Check At Start", "Looks for a newer release when the launcher opens", ["Off", "On"],
                           boolSetting("checkUpdates", true) ? 1 : 0)
    let unlock = Row("Unlocks", "Earn offers only the default characters and costumes; a profile saved with Everything keeps them",
                     ["Everything", "Earn"], boolSetting("unlockAll", true) ? 0 : 1)
    lazy var pages = [
        Page(name: "VIDEO SETTINGS", info: "Sets resolution, anti-aliasing and screen shape", rows: [resolution, msaa, smaa, screen]),
        Page(name: "AUDIO SETTINGS", info: "Chooses stereo or the game's own 5.1 surround mix", rows: [surround]),
        Page(name: "MOVIES", info: "Plays or skips the logo and opening movies at boot", rows: [intro]),
        Page(name: "GAMEPLAY", info: "Offers every character and costume, or leaves them to earn through play", rows: [unlock]),
        Page(name: "START GAME", info: "Saves these settings and starts the game", rows: []),
        Page(name: "UPDATE", info: "Checks GitHub for a newer release and installs it", rows: [updateRow, checkAtStart]),
        Page(name: "EXIT", info: "Closes the launcher", rows: []),
    ]
    // The tab cursor picks a page; inside a page the row cursor picks a setting column.
    var tab = LauncherView.playPage, row = 0, inPage = false
    // A held Down that entered a page waits for release before it changes a value.
    var downEntered = false
    // The newer version a check found, and whether a check or install is running.
    // Play and quit wait for an install, which replaces the tools.
    var pending: String?
    var updating = false, installing = false
    var tabs: [NSRect] = []
    let built = FileManager.default.fileExists(atPath: root.appendingPathComponent("build/recomp-program/recomp_program_runner").path)
    var onPlay: () -> Void = {}
    // A private/launcher-background.png or .jpg replaces the generated backdrop.
    let background = ["private/launcher-background.png", "private/launcher-background.jpg", "tools/launcher-background.jpg"]
        .lazy.compactMap { NSImage(contentsOf: root.appendingPathComponent($0)) }.first

    let menuFont = font("HelveticaNeue-BoldItalic", 19)
    let tabFont = font("HelveticaNeue-BoldItalic", 14), infoFont = font("HelveticaNeue-Italic", 14), smallFont = font("HelveticaNeue-Bold", 12)
    let titleFont = font("HelveticaNeue-BoldItalic", 25), ultimateFont = font("HelveticaNeue-BoldItalic", 21)
    let logoFont = font("Arial-Black", 30), bigFont = font("Arial-Black", 54)

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        updateRow.action = { [unowned self] in self.update(quiet: false) }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil))
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    var rows: [Row] { pages[tab].rows }
    var columnWidth: CGFloat { (bounds.width - 2 * LauncherView.side) / CGFloat(max(rows.count, 1)) }

    var chosen: [String: Any] {
        [
            "height": heights[resolution.index],
            "msaa": samples[msaa.index],
            "smaa": smaa.index == 1,
            "widescreen": screen.index == 1,
            "skipIntro": intro.index == 1,
            "surround": surround.index == 1,
            "checkUpdates": checkAtStart.index == 1,
            "unlockAll": unlock.index == 0,
        ]
    }

    // Checks for a newer release, or installs the one found. A quiet check says nothing unless it finds one.
    func update(quiet: Bool) {
        if updating { return }
        installing = pending != nil
        updating = true
        updateRow.names = ["Working"]
        if !quiet { updateRow.info = installing ? "Downloading \(pending ?? "")" : "Checking for updates" }
        runUpdate(installing ? "install" : "check") { [self] ok, output in
            let installed = installing
            updating = false
            installing = false
            if ok && installed {
                pending = nil
                // An install prints nothing when the release it found is no longer newer.
                updateRow.info = output.isEmpty ? "You have the latest version"
                    : "Updated to \(output). Run ./build_game.sh, then reopen the launcher"
            } else if ok {
                pending = output.isEmpty ? nil : output
                updateRow.info = pending.map { "Version \($0) is available. Choose Install" } ?? "You have the latest version"
            } else if !quiet {
                updateRow.info = output
            }
            updateRow.names = [pending == nil ? "Check" : "Install"]
            needsDisplay = true
        }
    }

    func change(_ delta: Int) {
        let r = rows[row]
        r.index = min(max(r.index + delta, 0), r.names.count - 1)
    }

    // Shows the update page with why play or quit has to wait; false when nothing is installing.
    func waitForInstall() -> Bool {
        guard installing else { return false }
        tab = LauncherView.updatePage
        inPage = true
        row = 0
        updateRow.info = "Wait for the update to finish"
        needsDisplay = true
        return true
    }

    func accept() {
        if !inPage {
            if (tab == LauncherView.playPage || tab == LauncherView.exitPage) && waitForInstall() { return }
            if tab == LauncherView.playPage { onPlay(); return }
            if tab == LauncherView.exitPage { NSApp.terminate(nil); return }
            inPage = true
            row = 0
        } else if let action = rows[row].action {
            action()
        } else {
            change(1)
        }
    }

    func back() {
        if inPage { inPage = false } else if !waitForInstall() { NSApp.terminate(nil) }
    }

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 123: if inPage { row = max(row - 1, 0) } else { tab = max(tab - 1, 0) }
        case 124: if inPage { row = min(row + 1, rows.count - 1) } else { tab = min(tab + 1, pages.count - 1) }
        case 126: if inPage { change(1) }
        case 125:
            if !event.isARepeat { downEntered = false }
            if !inPage { if !rows.isEmpty { accept(); downEntered = true } }
            else if !(event.isARepeat && downEntered) { change(-1) }
        case 36, 76, 49: accept()
        case 53, 51: back()
        default: super.keyDown(with: event); return
        }
        needsDisplay = true
    }

    func hitTab(_ p: NSPoint) -> Int { tabs.firstIndex { $0.contains(p) } ?? -1 }
    func hitColumn(_ p: NSPoint) -> Int {
        guard p.y >= LauncherView.labelY - 6, p.y <= LauncherView.valueY + LauncherView.valueH, p.x >= LauncherView.side else { return -1 }
        let i = Int((p.x - LauncherView.side) / columnWidth)
        return i < rows.count ? i : -1
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let m = hitTab(p), i = hitColumn(p)
        if !inPage && m >= 0 && m != tab { tab = m; needsDisplay = true }
        if inPage && i >= 0 && i != row { row = i; needsDisplay = true }
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let m = hitTab(p), i = hitColumn(p)
        defer { needsDisplay = true }
        if m >= 0 { tab = m; inPage = false; accept(); return }
        guard i >= 0 else { return }
        // From the tabs, the first click only enters the page and selects the setting.
        let entering = !inPage
        inPage = true
        row = i
        if entering { return }
        if rows[i].action != nil { accept() } else { change(p.x < LauncherView.side + (CGFloat(i) + 0.5) * columnWidth ? -1 : 1) }
    }

    override func rightMouseDown(with event: NSEvent) { back(); needsDisplay = true }

    // Text vertically centred in rect; shadowed text gets the logo's dark drop shadow.
    func text(_ s: String, _ f: NSFont, _ color: NSColor, _ rect: NSRect, _ align: NSTextAlignment = .left,
              shadow: Bool = false, oblique: CGFloat = 0) {
        let style = NSMutableParagraphStyle()
        style.alignment = align
        var attributes: [NSAttributedString.Key: Any] = [.font: f, .foregroundColor: color, .paragraphStyle: style, .obliqueness: oblique]
        if shadow {
            let drop = NSShadow()
            drop.shadowColor = rgb(30, 22, 18, 220)
            drop.shadowOffset = NSSize(width: 2, height: -2)
            attributes[.shadow] = drop
        }
        let string = NSAttributedString(string: s, attributes: attributes)
        let height = min(rect.height, string.boundingRect(with: rect.size, options: [.usesLineFragmentOrigin]).height)
        string.draw(in: NSRect(x: rect.minX, y: rect.minY + (rect.height - height) / 2, width: rect.width, height: height))
    }

    // A triangle pointing along dx, dy from its base centred on x, y.
    func arrow(_ x: CGFloat, _ y: CGFloat, _ dx: CGFloat, _ dy: CGFloat, _ color: NSColor) {
        let path = NSBezierPath()
        path.move(to: NSPoint(x: x + 7 * dx, y: y + 7 * dy))
        path.line(to: NSPoint(x: x - 5 * dy, y: y - 5 * dx))
        path.line(to: NSPoint(x: x + 5 * dy, y: y + 5 * dx))
        path.close()
        color.setFill()
        path.fill()
    }

    func width(_ s: String, _ f: NSFont) -> CGFloat { NSAttributedString(string: s, attributes: [.font: f]).size().width }

    override func draw(_ dirtyRect: NSRect) {
        let w = bounds.width, h = bounds.height, side = LauncherView.side, topH = LauncherView.topH
        let labelY = LauncherView.labelY, valueY = LauncherView.valueY, valueH = LauncherView.valueH
        let tabY = LauncherView.tabY, tabH = LauncherView.tabH
        let sand = rgb(241, 197, 143), dark = rgb(42, 29, 21), muted = rgb(191, 178, 165), chrome = rgb(226, 228, 228)
        rgb(74, 60, 51).setFill()
        bounds.fill()
        background?.draw(in: bounds, from: .zero, operation: .copy, fraction: 1, respectFlipped: true, hints: nil)

        // Letterbox: a solid top bar, and the backdrop fading into the same dark under the settings.
        rgb(13, 10, 9).setFill()
        NSRect(x: 0, y: 0, width: w, height: topH).fill()
        let fadeStops: [CGFloat] = [0, 0.26, 1]
        NSGradient(colors: [rgb(13, 10, 9, 0), rgb(13, 10, 9, 230), rgb(13, 10, 9)], atLocations: fadeStops, colorSpace: .sRGB)?
            .draw(in: NSRect(x: 0, y: LauncherView.fadeY, width: w, height: h - LauncherView.fadeY), angle: 90)

        // Top bar: title and build state on the left, key prompts on the right.
        text("DEAD OR ALIVE 2 ULTIMATE  \u{00B7}  " + (built ? "READY TO PLAY" : "NOT BUILT YET")
             + (pending != nil ? "  \u{00B7}  UPDATE AVAILABLE" : ""), smallFont, muted,
             NSRect(x: side, y: 0, width: 500, height: topH))
        var x = w - side
        for (key, label) in [("", "CHANGE"), ("Esc", "BACK"), ("Return", "SELECT")] {
            x -= width(label, smallFont)
            text(label, smallFont, muted, NSRect(x: x, y: 0, width: width(label, smallFont) + 4, height: topH))
            let pillW: CGFloat = key.isEmpty ? 26 : width(key, smallFont) + 14
            x -= pillW + 6
            let pill = NSRect(x: x, y: topH / 2 - 10, width: pillW, height: 20)
            rgb(242, 227, 207).setFill()
            NSBezierPath(roundedRect: pill, xRadius: 10, yRadius: 10).fill()
            if key.isEmpty { arrow(x + 9, topH / 2 + 2, 0, -1, dark); arrow(x + 17, topH / 2 - 2, 0, 1, dark) }
            else { text(key, smallFont, dark, pill, .center) }
            x -= 22
        }

        // Logo in the sky at the right, as in launcher.cs.
        let right = w - 44, two = width("2", bigFont)
        text("2", bigFont, chrome, NSRect(x: 0, y: 60, width: right, height: 74), .right, shadow: true, oblique: 0.2)
        text("DEAD OR ALIVE", logoFont, chrome, NSRect(x: 0, y: 78, width: right - two - 8, height: 46), .right, shadow: true, oblique: 0.2)
        text("ULTIMATE", ultimateFont, chrome, NSRect(x: 0, y: 124, width: right - 4, height: 30), .right, shadow: true)

        // The chosen page in the bottom bar: its information line, then its settings side by side.
        let page = pages[tab]
        text(inPage ? page.rows[row].info : page.info, infoFont, rgb(216, 202, 188),
             NSRect(x: side, y: LauncherView.infoY, width: w - 2 * side, height: 22))
        if tab == LauncherView.playPage {
            text("PRESS START BUTTON", titleFont, chrome, NSRect(x: side, y: valueY - 8, width: w - 2 * side, height: 40), shadow: true)
        }
        let colW = columnWidth
        for (i, r) in page.rows.enumerated() {
            let left = side + CGFloat(i) * colW
            let selected = inPage && i == row
            if selected {
                rgb(232, 191, 138, 45).setFill()
                NSRect(x: left, y: labelY - 6, width: colW, height: valueY + valueH + 6 - labelY).fill(using: .sourceOver)
            }
            if i > 0 {
                rgb(255, 255, 255, 36).setFill()
                NSRect(x: left, y: labelY - 2, width: 1, height: valueY + valueH + 2 - labelY).fill(using: .sourceOver)
            }
            text(r.label.uppercased(), smallFont, selected ? .white : rgb(198, 182, 166), NSRect(x: left + 12, y: labelY, width: colW - 16, height: 18))
            let value = r.names[r.index].uppercased()
            let arrowColor = selected ? sand : rgb(241, 197, 143, 130)
            arrow(left + 19, valueY + valueH / 2, -1, 0, arrowColor)
            arrow(left + 30 + width(value, menuFont), valueY + valueH / 2, 1, 0, arrowColor)
            text(value, menuFont, selected ? .white : rgb(232, 222, 212), NSRect(x: left + 26, y: valueY, width: colW - 30, height: valueH))
        }

        // Page tabs along the bottom, the current one underlined in sunset gold.
        rgb(255, 255, 255, 36).setFill()
        NSRect(x: side, y: tabY - 8, width: w - 2 * side, height: 1).fill(using: .sourceOver)
        var tx = side
        tabs = pages.enumerated().map { m, p in
            let tw = width(p.name, tabFont)
            let rect = NSRect(x: tx, y: tabY, width: tw, height: tabH)
            text(p.name, tabFont, m == tab ? rgb(255, 243, 228) : rgb(169, 156, 144), NSRect(x: tx, y: tabY, width: tw + 4, height: tabH))
            if m == tab {
                NSGradient(starting: rgb(247, 220, 181), ending: rgb(201, 138, 85))?.draw(in: NSRect(x: tx, y: tabY + tabH, width: tw, height: 3), angle: 0)
            }
            tx += tw + 18
            return rect
        }
    }
}

final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 960, height: 540),
                          styleMask: [.titled, .closable], backing: .buffered, defer: false)
    let view = LauncherView(frame: NSRect(x: 0, y: 0, width: 960, height: 540))
    var launched = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        view.onPlay = { [unowned self] in self.play() }
        if view.checkAtStart.index == 1 { view.update(quiet: true) }
        window.title = "Dead or Alive 2 Ultimate"
        window.delegate = self
        window.contentView = view
        window.acceptsMouseMovedEvents = true
        window.makeFirstResponder(view)
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func play() {
        do {
            try FileManager.default.createDirectory(
                at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try JSONSerialization.data(
                withJSONObject: view.chosen, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: settingsURL)
        } catch {
            NSAlert(error: error).runModal()
            return
        }
        launched = true
        NSApp.terminate(nil)
    }

    // Other settings save on play; Check At Start must also hold after a plain quit.
    func keepCheckSetting() {
        let check = view.checkAtStart.index == 1
        guard !launched, check != boolSetting("checkUpdates", true) else { return }
        settings["checkUpdates"] = check
        try? FileManager.default.createDirectory(at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONSerialization.data(withJSONObject: settings, options: [.prettyPrinted, .sortedKeys]).write(to: settingsURL)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool { !view.waitForInstall() }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        view.waitForInstall() ? .terminateCancel : .terminateNow
    }

    func windowWillClose(_ notification: Notification) {
        keepCheckSetting()
        if !launched { exit(1) }
    }

    // AppKit exits 0 after terminate, so a Dock or Cmd-Q quit would start the game.
    func applicationWillTerminate(_ notification: Notification) {
        keepCheckSetting()
        exit(launched ? 0 : 1)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let launcher = Launcher()
app.delegate = launcher
app.run()
exit(launcher.launched ? 0 : 1)
