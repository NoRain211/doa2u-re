// SPDX-License-Identifier: GPL-3.0-or-later
// macOS launcher: pick resolution, anti-aliasing, sharpening, ambient occlusion, widescreen, 5.1 surround, the free camera,
// the intro skip, the staff-roll dedication card and texture dumping, remember them in private/launcher.json, then exit 0 so
// launcher.sh starts the game. Mirrors tools/launcher.cs, its GAME page's build included,
// with MSAA up to SDL_GPU's 8x: the backdrop under a top letterbox bar, and a panel
// at the left with the page tabs, the chosen page's settings and their information.
// Adapted from committed DOA3 tools/launcher_macos.swift at ce393e4.
// Keyboard and mouse. Exits 1 if the window is closed.
import AppKit

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1
    ? CommandLine.arguments[1] : FileManager.default.currentDirectoryPath)
let settingsURL = root.appendingPathComponent("private/launcher.json")

let heights = [480, 720, 1080, 1440, 2160]
let samples = [1, 2, 4, 8]
let screens = ["4:3", "16:9", "16:10", "21:9", "32:9"]
let sharpens = ["off", "low", "medium", "high"]
let dumps = ["off", "png", "dds"]

var settings: [String: Any] = [:]
if let data = try? Data(contentsOf: settingsURL),
   let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
    settings = json
}
// The GAME page builds from the player's own Disc 2 ISO, remembered in the shared game_source setting.
var gameSource: String = settings["game_source"] as? String ?? ""
let buildLogURL = root.appendingPathComponent("private/build.log")
func isBuilt() -> Bool {
    FileManager.default.fileExists(atPath: root.appendingPathComponent("build/recomp-program/recomp_program_runner").path)
}

func intSetting(_ key: String, _ fallback: Int) -> Int {
    (settings[key] as? NSNumber)?.intValue ?? fallback
}
func boolSetting(_ key: String, _ fallback: Bool) -> Bool {
    (settings[key] as? NSNumber)?.boolValue ?? fallback
}
func stringSetting(_ key: String, _ fallback: String) -> String {
    settings[key] as? String ?? fallback
}
// "4:3", "16:9", "16:10", "32:9" or a 21:9 "W:H"; settings saved before 16:10
// hold only a widescreen flag.
func screenSetting() -> String {
    settings["screen"] as? String ?? (boolSetting("widescreen", true) ? "16:9" : "4:3")
}
// The main screen's own shape when it is a 21:9 display (wider than 16:9, narrower than
// 3:1, so not 32:9), else 21:9 as 2560x1080 (64:27).
func ultrawideShape() -> String {
    guard let screen = NSScreen.screens.first else { return "64:27" }
    let w = Int(screen.frame.width * screen.backingScaleFactor)
    let h = Int(screen.frame.height * screen.backingScaleFactor)
    return w * 9 > h * 16 && w < h * 3 ? "\(w):\(h)" : "64:27"
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
    // Horizon Bar layout, as in tools/launcher.cs: a top letterbox bar, and the page tabs, settings rows and
    // information in a panel at the left.
    static let topH: CGFloat = 44, side: CGFloat = 30, fadeY: CGFloat = 334
    static let panelW: CGFloat = 480, inset: CGFloat = 20
    static let tabY: CGFloat = 56, tabH: CGFloat = 26, listY: CGFloat = 100, rowH: CGFloat = 36, infoY: CGFloat = 480
    static let gamePage = 0, updatePage = 4

    // launcher.cs's defaults: the screen height, the most MSAA, SMAA and widescreen on, sharpening off.
    let resolution = Row("Resolution", "Sets the render resolution. 480p opens in a window",
                         heights.map { "\($0)p" }, heights.lastIndex(where: { $0 <= intSetting("height", screenHeight) }) ?? 0)
    // Windows saves up to 32x; SDL_GPU stops at 8x.
    let msaa = Row("MSAA", "Smooths polygon edges; a count the GPU lacks falls back to its highest",
                   samples.map { $0 == 1 ? "Off" : "\($0)x" }, samples.lastIndex(where: { $0 <= intSetting("msaa", 8) }) ?? 0)
    let smaa = Row("SMAA", "Smooths edges with SMAA after MSAA", ["Off", "On"], boolSetting("smaa", true) ? 1 : 0)
    let sharpen = Row("Sharpening", "Sharpens the picture with AMD FidelityFX CAS after anti-aliasing",
                      ["Off", "Low", "Medium", "High"], sharpens.firstIndex(of: stringSetting("sharpen", "off")) ?? 0)
    let ao = Row("Ambient Occlusion", "Darkens creases and contact areas; off leaves the picture as the game drew it",
                 ["Off", "On"], boolSetting("ambientOcclusion", false) ? 1 : 0)
    let screen = Row("Screen", "Widescreen widens the game's own camera; its 16:9 menus keep their shape", screens,
                     screens.firstIndex(of: screenSetting()) ?? 3)
    let surround = Row("Output", "5.1 plays the game's own surround mix", ["Stereo", "5.1"], boolSetting("surround", false) ? 1 : 0)
    let intro = Row("Intro Movies", "Skip goes straight to the title screen at boot", ["Play", "Skip"],
                    boolSetting("skipIntro", false) ? 1 : 0)
    let playRow = Row("Start Game", "Saves these settings and starts the game", ["Start"], 0)
    let chooseGame = Row("Disc 2 ISO", "", ["Choose"], 0)
    let buildRow = Row("Build", "Builds the game from your Disc 2 ISO; takes about 10 minutes", ["Build"], 0)
    let updateRow = Row("Update", "Choose Check to look for a newer release", ["Check"], 0)
    let checkAtStart = Row("Check At Start", "Looks for a newer release when the launcher opens", ["Off", "On"],
                           boolSetting("checkUpdates", true) ? 1 : 0)
    let unlock = Row("Unlocks", "Earn offers only what the profile has earned; Everything is never saved to it",
                     ["Everything", "Earn"], boolSetting("unlockAll", true) ? 0 : 1)
    let freeCamera = Row("Free Camera", "In game, F1 or a right-stick click flies the camera", ["Off", "On"],
                         boolSetting("freeCamera", false) ? 1 : 0)
    let cpuCostumes = Row("CPU Costumes", "Random dresses CPU opponents in any unlocked costume; offline only", ["Default", "Random"],
                          boolSetting("randomCpuCostumes", false) ? 1 : 0)
    let dedication = Row("Dedication", "Shows a memorial card before the staff roll", ["Show", "Hide"],
                         boolSetting("dedicationCard", true) ? 0 : 1)
    let textureDump = Row("Texture Dump", "Writes each texture once to textures/dumps for editing; can stutter while it writes",
                          ["Off", "PNG", "DDS"], dumps.firstIndex(of: stringSetting("textureDump", "off")) ?? 0)
    // No JUKEBOX page: this launcher has no jukebox rows.
    lazy var pages = [
        Page(name: "GAME", info: "Starts the game, or builds it from your own Disc 2 ISO", rows: [playRow, chooseGame, buildRow]),
        Page(name: "VIDEO", info: "Sets resolution, anti-aliasing, sharpening, occlusion, screen shape and the intro movies",
             rows: [resolution, msaa, smaa, sharpen, ao, screen, intro]),
        Page(name: "AUDIO", info: "Chooses stereo or the game's own 5.1 surround mix", rows: [surround]),
        Page(name: "GAMEPLAY", info: "Chooses the unlocks, CPU costumes, free camera, dedication card and texture dumping",
             rows: [unlock, cpuCostumes, freeCamera, dedication, textureDump]),
        Page(name: "UPDATE", info: "Checks GitHub for a newer release and installs it", rows: [updateRow, checkAtStart]),
    ]
    // Focus is on the page tabs, or (inPage) on the current page's row. It opens on Start Game, or on
    // Disc 2 ISO before a build.
    var tab = LauncherView.gamePage, row = isBuilt() ? 0 : 1, inPage = true
    // A line that replaces the information text until the next input: build progress or a refused action.
    var notice: String?
    // The build runs build_game.sh with its output in private/build.log. The first exit request during a
    // build asks for a second.
    var buildProcess: Process?
    var buildTimer: Timer?
    var buildStart = Date()
    var stopDeadline = Date.distantPast // a quit during a build asks for a second one before this
    // The newer version a check found, and whether a check or install is running.
    // Play and quit wait for an install, which replaces the tools.
    var pending: String?
    var updating = false, installing = false
    var tabs: [NSRect] = []
    var built = isBuilt()
    var onPlay: () -> Void = {}
    // A private/launcher-background.png or .jpg replaces the generated backdrop.
    let background = ["private/launcher-background.png", "private/launcher-background.jpg", "tools/launcher-background.jpg"]
        .lazy.compactMap { NSImage(contentsOf: root.appendingPathComponent($0)) }.first

    let menuFont = font("HelveticaNeue-BoldItalic", 19)
    let tabFont = font("HelveticaNeue-BoldItalic", 14), infoFont = font("HelveticaNeue-Italic", 14), smallFont = font("HelveticaNeue-Bold", 12)
    let ultimateFont = font("HelveticaNeue-BoldItalic", 21)
    let logoFont = font("Arial-Black", 30), bigFont = font("Arial-Black", 54)

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        updateRow.action = { [unowned self] in self.update(quiet: false) }
        playRow.action = { [unowned self] in self.startGame() }
        chooseGame.action = { [unowned self] in self.chooseGameDisc() }
        buildRow.action = { [unowned self] in self.startBuild() }
        showGame()
        registerForDraggedTypes([.fileURL])
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil))
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    var rows: [Row] { pages[tab].rows }

    var chosen: [String: Any] {
        var chosen: [String: Any] = [
            "height": heights[resolution.index],
            "msaa": samples[msaa.index],
            "smaa": smaa.index == 1,
            "sharpen": sharpens[sharpen.index],
            "ambientOcclusion": ao.index == 1,
            "widescreen": screen.index != 0,
            "screen": screen.index == 3 ? ultrawideShape() : screens[screen.index],
            "skipIntro": intro.index == 1,
            "surround": surround.index == 1,
            "checkUpdates": checkAtStart.index == 1,
            "unlockAll": unlock.index == 0,
            "freeCamera": freeCamera.index == 1,
            "randomCpuCostumes": cpuCostumes.index == 1,
            "dedicationCard": dedication.index == 0,
            "textureDump": dumps[textureDump.index],
        ]
        if !gameSource.isEmpty { chosen["game_source"] = gameSource }
        return chosen
    }

    func showGame() {
        chooseGame.info = gameSource.isEmpty ? "No Disc 2 chosen; choose its .iso, or drop it here"
            : "Disc: " + (gameSource as NSString).lastPathComponent
    }

    func chooseGameDisc() {
        let panel = NSOpenPanel()
        panel.message = "Choose your Dead or Alive Ultimate Disc 2 image (.iso)"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        useGame(url.path)
    }

    // Takes an ISO from the panel or a drop; anything else is refused.
    func useGame(_ path: String) {
        guard buildProcess == nil else { notice = "Wait for the build to finish"; return }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory), !isDirectory.boolValue,
              (path as NSString).pathExtension.lowercased() == "iso" else {
            notice = "Choose your Disc 2 .iso image"
            tab = LauncherView.gamePage
            inPage = true
            row = 1 // Disc 2 ISO
            return
        }
        gameSource = path
        settings["game_source"] = path
        showGame()
        tab = LauncherView.gamePage
        inPage = true
        row = 2 // Build
        notice = "Disc: " + (path as NSString).lastPathComponent + ". Press Build"
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                         options: [.urlReadingFileURLsOnly: true]) as? [URL]
        guard let url = urls?.first else { return false }
        useGame(url.path)
        needsDisplay = true
        return true
    }

    func startBuild() {
        guard buildProcess == nil else { return }
        if installing { notice = "Wait for the update to finish"; return }
        if gameSource.isEmpty { notice = "Choose your Disc 2 ISO first"; return }
        let manager = FileManager.default
        try? manager.createDirectory(at: buildLogURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        manager.createFile(atPath: buildLogURL.path, contents: nil)
        guard let log = try? FileHandle(forWritingTo: buildLogURL) else { notice = "Cannot write private/build.log"; return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/bash")
        process.arguments = [root.appendingPathComponent("build_game.sh").path, gameSource]
        process.currentDirectoryURL = root
        var environment = ProcessInfo.processInfo.environment
        environment["PYTHONUNBUFFERED"] = "1"
        environment["PYTHONIOENCODING"] = "utf-8"
        process.environment = environment
        process.standardOutput = log
        process.standardError = log
        process.standardInput = FileHandle.nullDevice
        process.terminationHandler = { [weak self] finished in
            let status = finished.terminationStatus, exited = finished.terminationReason == .exit
            DispatchQueue.main.async { self?.buildFinished(status: status, exited: exited) }
        }
        do { try process.run() } catch { notice = "Cannot start the build: \(error.localizedDescription)"; return }
        buildProcess = process
        stopDeadline = .distantPast
        buildStart = Date()
        buildRow.names = ["Working"]
        notice = "Starting the build"
        buildTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self = self, self.buildProcess != nil, Date() >= self.stopDeadline else { return }
            let seconds = Int(Date().timeIntervalSince(self.buildStart))
            self.notice = String(format: "BUILDING %02d:%02d  ", seconds / 60, seconds % 60) + self.logTail()
            self.needsDisplay = true
        }
    }

    // The last non-empty line of the build log.
    func logTail() -> String {
        guard let handle = try? FileHandle(forReadingFrom: buildLogURL) else { return "" }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        try? handle.seek(toOffset: size > 4096 ? size - 4096 : 0)
        let data = (try? handle.readToEnd()) ?? Data()
        return String(decoding: data, as: UTF8.self).split(whereSeparator: \.isNewline).last
            .map { $0.trimmingCharacters(in: .whitespaces) } ?? ""
    }

    func buildFinished(status: Int32, exited: Bool) {
        buildTimer?.invalidate()
        buildTimer = nil
        buildProcess = nil
        buildRow.names = ["Build"]
        built = isBuilt()
        showGame()
        if exited && status == 0 {
            notice = "Build complete. Choose Start Game to play"
            tab = LauncherView.gamePage
            inPage = true
            row = 0 // Start Game
        } else {
            let tail = logTail()
            notice = tail.hasPrefix("Build failed") ? tail : "Build failed: " + tail
            tab = LauncherView.gamePage
            inPage = true
            row = 2 // Build
        }
        needsDisplay = true
    }

    // Stops the build and everything it started: the children are found through their parents.
    func stopBuild() {
        guard let process = buildProcess, process.isRunning else { return }
        let tree = Process()
        tree.executableURL = URL(fileURLWithPath: "/bin/sh")
        tree.arguments = ["-c", "kill_tree() { for c in $(pgrep -P $1); do kill_tree $c; done; kill -TERM $1; }; kill_tree $0",
                          String(process.processIdentifier)]
        try? tree.run()
        tree.waitUntilExit()
        process.waitUntilExit()
    }

    // Leaving needs an install to finish, and a running build to be stopped, which takes a second request.
    func exitBlocked() -> Bool {
        if waitForInstall() { return true }
        guard buildProcess != nil else { return false }
        if Date() < stopDeadline { stopBuild(); return false }
        stopDeadline = Date().addingTimeInterval(3)
        tab = LauncherView.gamePage
        inPage = true
        row = 2 // Build
        notice = "A build is running. Quit again to stop it"
        needsDisplay = true
        return true
    }


    // Checks for a newer release, or installs the one found. A quiet check says nothing unless it finds one.
    func update(quiet: Bool) {
        if updating { return }
        // An install rewrites the scripts and tools a running build reads.
        if pending != nil && buildProcess != nil { notice = "Wait for the build to finish"; return }
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

    // Start Game; before a build the cursor goes to the row that comes next, Disc 2 ISO or Build.
    func startGame() {
        if waitForInstall() { return }
        tab = LauncherView.gamePage
        inPage = true
        if buildProcess != nil { row = 2; notice = "Wait for the build to finish"; return }
        if !built { row = gameSource.isEmpty ? 1 : 2; notice = "Build the game first"; return }
        onPlay()
    }

    // Q/E, PageUp/PageDown, or Left/Right on the tabs; a focused list starts at its first row.
    func switchPage(_ delta: Int) {
        let m = min(max(tab + delta, 0), pages.count - 1)
        if m != tab { tab = m; row = 0 }
    }

    // From the tabs, enters the list; on a row, runs its action. Value rows change only with Left/Right.
    func accept() {
        if !inPage {
            inPage = true
            row = 0
        } else if let action = rows[row].action {
            action()
        }
    }

    func back() {
        if inPage { inPage = false } else { NSApp.terminate(nil) }
    }

    override func keyDown(with event: NSEvent) {
        if buildProcess == nil { notice = nil }
        switch event.keyCode {
        case 123: if inPage { change(-1) } else { switchPage(-1) }
        case 124: if inPage { change(1) } else { switchPage(1) }
        case 126: if inPage { if row == 0 { inPage = false } else { row -= 1 } }
        case 125: if inPage { row = min(row + 1, rows.count - 1) } else { accept() }
        // Q and E (not Cmd-Q or Cmd-E), PageUp and PageDown.
        case 12 where !event.modifierFlags.contains(.command), 116: switchPage(-1)
        case 14 where !event.modifierFlags.contains(.command), 121: switchPage(1)
        case 36, 76, 49: accept()
        case 53, 51: back()
        default: super.keyDown(with: event); return
        }
        needsDisplay = true
    }

    func hitTab(_ p: NSPoint) -> Int { tabs.firstIndex { $0.contains(p) } ?? -1 }
    func hitRow(_ p: NSPoint) -> Int {
        guard p.x >= LauncherView.inset - 10, p.x < LauncherView.panelW - LauncherView.inset + 10, p.y >= LauncherView.listY else { return -1 }
        let i = Int((p.y - LauncherView.listY) / LauncherView.rowH)
        return i < rows.count ? i : -1
    }

    // The right edge of a row's value or button, and the left edge of its value with the left arrow.
    var valueRight: CGFloat { LauncherView.panelW - LauncherView.inset }
    func valueLeft(_ r: Row) -> CGFloat { valueRight - 28 - width(r.names[r.index].uppercased(), menuFont) }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let m = hitTab(p), i = hitRow(p)
        if m >= 0 && (m != tab || inPage) { tab = m; inPage = false; needsDisplay = true }
        if i >= 0 && (i != row || !inPage) { row = i; inPage = true; needsDisplay = true }
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let m = hitTab(p), i = hitRow(p)
        if buildProcess == nil { notice = nil }
        defer { needsDisplay = true }
        if m >= 0 { tab = m; inPage = false; return }
        guard i >= 0 else { return }
        inPage = true
        row = i
        let r = rows[i]
        // A click on the value steps it: its left half -1, its right half +1.
        if let action = r.action { action() } else if p.x >= valueLeft(r) { change(p.x < (valueLeft(r) + valueRight) / 2 ? -1 : 1) }
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
        let panelW = LauncherView.panelW, inset = LauncherView.inset, rowH = LauncherView.rowH
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
        for (key, label) in [("Esc", "BACK"), ("Return", "SELECT"), ("", "CHANGE"), ("Q/E", "PAGE")] {
            x -= width(label, smallFont)
            text(label, smallFont, muted, NSRect(x: x, y: 0, width: width(label, smallFont) + 4, height: topH))
            let pillW: CGFloat = key.isEmpty ? 26 : width(key, smallFont) + 14
            x -= pillW + 6
            let pill = NSRect(x: x, y: topH / 2 - 10, width: pillW, height: 20)
            rgb(242, 227, 207).setFill()
            NSBezierPath(roundedRect: pill, xRadius: 10, yRadius: 10).fill()
            if key.isEmpty { arrow(x + 12, topH / 2, -1, 0, dark); arrow(x + 14, topH / 2, 1, 0, dark) }
            else { text(key, smallFont, dark, pill, .center) }
            x -= 22
        }

        // Logo in the sky at the right, as in launcher.cs.
        let logoRight = w - 44, two = width("2", bigFont)
        text("2", bigFont, chrome, NSRect(x: 0, y: 60, width: logoRight, height: 74), .right, shadow: true, oblique: 0.2)
        text("DEAD OR ALIVE", logoFont, chrome, NSRect(x: 0, y: 78, width: logoRight - two - 8, height: 46), .right, shadow: true, oblique: 0.2)
        text("ULTIMATE", ultimateFont, chrome, NSRect(x: 0, y: 124, width: logoRight - 4, height: 30), .right, shadow: true)

        // The settings panel: the page tabs, the chosen page's rows, then the information.
        rgb(13, 10, 9, 190).setFill()
        NSRect(x: 0, y: topH, width: panelW, height: h - topH).fill(using: .sourceOver)
        // The current tab is underlined in sunset gold, and highlighted while the tabs have focus.
        let tabW = (panelW - 2 * inset) / CGFloat(pages.count)
        tabs = pages.enumerated().map { m, p in
            let rect = NSRect(x: inset + CGFloat(m) * tabW, y: tabY, width: tabW, height: tabH)
            if m == tab && !inPage {
                rgb(232, 191, 138, 45).setFill()
                rect.fill(using: .sourceOver)
            }
            text(p.name, tabFont, m != tab ? rgb(169, 156, 144) : inPage ? rgb(255, 243, 228) : .white, rect, .center)
            if m == tab {
                let tw = width(p.name, tabFont)
                NSGradient(starting: rgb(247, 220, 181), ending: rgb(201, 138, 85))?
                    .draw(in: NSRect(x: rect.midX - tw / 2, y: tabY + tabH, width: tw, height: 3), angle: 0)
            }
            return rect
        }
        rgb(255, 255, 255, 36).setFill()
        NSRect(x: inset, y: tabY + tabH + 8, width: panelW - 2 * inset, height: 1).fill(using: .sourceOver)

        // One row per setting: its label, then its value between arrows at the right, or an action's verb in a pill.
        let page = pages[tab], right = valueRight
        for (i, r) in page.rows.enumerated() {
            let y = LauncherView.listY + CGFloat(i) * rowH, mid = y + rowH / 2
            let selected = inPage && i == row
            if selected {
                rgb(232, 191, 138, 45).setFill()
                NSRect(x: inset - 10, y: y, width: panelW - 2 * inset + 20, height: rowH).fill(using: .sourceOver)
            }
            text(r.label.uppercased(), tabFont, selected ? .white : rgb(198, 182, 166), NSRect(x: inset, y: y, width: 220, height: rowH))
            let value = r.names[r.index].uppercased()
            if r.action != nil {
                let pillW = width(value, smallFont) + 24
                let pill = NSRect(x: right - pillW, y: mid - 11, width: pillW, height: 22)
                (selected ? rgb(242, 227, 207) : rgb(255, 255, 255, 36)).setFill()
                NSBezierPath(roundedRect: pill, xRadius: 11, yRadius: 11).fill()
                text(value, smallFont, selected ? dark : rgb(232, 222, 212), pill, .center)
            } else {
                // The arrows show which way the value can still move; the ends do not wrap.
                let left = valueLeft(r), arrowColor = selected ? sand : rgb(241, 197, 143, 130)
                if r.index > 0 { arrow(left + 7, mid, -1, 0, arrowColor) }
                if r.index < r.names.count - 1 { arrow(right - 7, mid, 1, 0, arrowColor) }
                text(value, menuFont, selected ? .white : rgb(232, 222, 212), NSRect(x: left, y: y, width: right - 14 - left, height: rowH), .right)
            }
        }

        // The notice, or the focused row's or page's information, in up to two lines.
        let style = NSMutableParagraphStyle()
        style.lineBreakMode = .byWordWrapping
        NSAttributedString(string: notice ?? (inPage ? rows[row].info : page.info),
                           attributes: [.font: infoFont, .foregroundColor: rgb(216, 202, 188), .paragraphStyle: style])
            .draw(with: NSRect(x: inset, y: LauncherView.infoY, width: panelW - 2 * inset, height: 36), // two 14 pt lines
                  options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
    }
}

// Keeps the fixed 960x540 scene centred and scaled on black when the window grows or goes fullscreen.
final class Letterbox: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()
    }

    override func resizeSubviews(withOldSize oldSize: NSSize) {
        guard let scene = subviews.first else { return }
        let s = min(bounds.width / 960, bounds.height / 540)
        scene.frame = NSRect(x: (bounds.width - 960 * s) / 2, y: (bounds.height - 540 * s) / 2,
                             width: 960 * s, height: 540 * s)
        scene.setBoundsSize(NSSize(width: 960, height: 540))
    }
}

final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 960, height: 540),
                          // The traffic lights give close, minimize and fullscreen.
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
    let view = LauncherView(frame: NSRect(x: 0, y: 0, width: 960, height: 540))
    var launched = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        view.onPlay = { [unowned self] in self.play() }
        if view.checkAtStart.index == 1 { view.update(quiet: true) }
        window.title = "Dead or Alive 2 Ultimate"
        // The Dock icon, when private/launcher-icon.png is present (fan art, so it stays untracked).
        if let icon = NSImage(contentsOf: root.appendingPathComponent("private/launcher-icon.png")) {
            NSApp.applicationIconImage = icon
        }
        window.delegate = self
        let letterbox = Letterbox(frame: view.frame)
        letterbox.addSubview(view)
        window.contentView = letterbox
        window.contentAspectRatio = view.frame.size
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
            // Keys this launcher has no row for, such as game_source, stay as saved.
            settings.merge(view.chosen) { $1 }
            let data = try JSONSerialization.data(
                withJSONObject: settings, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: settingsURL)
        } catch {
            NSAlert(error: error).runModal()
            return
        }
        launched = true
        NSApp.terminate(nil)
    }

    // Settings hold after a plain quit too, so the game plays with them later.
    func keepSettings() {
        guard !launched else { return }
        // A quit keeps an untouched 16x/32x MSAA or 21:9 shape; play saves what the rows show.
        var kept = view.chosen
        if view.msaa.index == samples.lastIndex(where: { $0 <= intSetting("msaa", 8) }) ?? 0 { kept["msaa"] = settings["msaa"] }
        if view.screen.index == screens.firstIndex(of: screenSetting()) ?? 3 { kept["screen"] = settings["screen"] }
        settings.merge(kept) { $1 }
        try? FileManager.default.createDirectory(at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONSerialization.data(withJSONObject: settings, options: [.prettyPrinted, .sortedKeys]).write(to: settingsURL)
    }

    // An install replaces the tools unattended, so closing or quitting waits for it; a build asks to be stopped.
    func windowShouldClose(_ sender: NSWindow) -> Bool { !view.exitBlocked() }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        view.exitBlocked() ? .terminateCancel : .terminateNow
    }

    func windowWillClose(_ notification: Notification) {
        keepSettings()
        if !launched { exit(1) }
    }

    // AppKit exits 0 after terminate, so a Dock or Cmd-Q quit would start the game.
    func applicationWillTerminate(_ notification: Notification) {
        keepSettings()
        exit(launched ? 0 : 1)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let launcher = Launcher()
app.delegate = launcher
app.run()
exit(launcher.launched ? 0 : 1)
