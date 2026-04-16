import Foundation

@MainActor
final class ControlCenterViewModel: ObservableObject {
    @Published var selectedAppURL: URL?
    @Published var daemonRunning = false
    @Published var injectedAppRunning = false
    @Published var statusMessage = "Select an app to begin."
    @Published var connectedControllerCount = 0
    @Published var appLogText = ""
    @Published var daemonLogText = ""
    @Published var appLogVersion = 0
    @Published var daemonLogVersion = 0
    @Published var adminUnlocked = false
    @Published var hasLaunchedInjectedApp = false

    private let daemonLogURL: URL
    private var daemonTailTask: Task<Void, Never>?
    private var daemonLogOffset: UInt64 = 0
    private var injectedProcess: Process?
    private var connectedSlots: Set<Int> = []

    private let daemonPath: String
    private let launcherPath: String
    private let daemonDirectoryPath: String

    init() {
        let daemonDir = Self.resolveXboxDaemonDirectory()
        daemonDirectoryPath = daemonDir.path
        daemonPath = daemonDir.appendingPathComponent("xbox_daemon").path
        launcherPath = daemonDir.appendingPathComponent("launch_ryujinx_injected.sh").path
        daemonLogURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("xbox_daemon_gui.log")

        appendAppLog("[app] daemon path: \(daemonPath)")
        appendAppLog("[app] launcher path: \(launcherPath)")
    }

    var selectedAppLabel: String {
        guard let selectedAppURL else { return "No app selected" }
        return selectedAppURL.path
    }

    var canLaunchInjectedApp: Bool {
        selectedAppURL != nil && !injectedAppRunning
    }

    var canStartDaemon: Bool {
        adminUnlocked && hasLaunchedInjectedApp && !daemonRunning
    }

    func handleFileImport(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let first = urls.first else {
                statusMessage = "No app selected."
                return
            }
            selectedAppURL = first
            statusMessage = "Selected \(first.lastPathComponent). Start the daemon next."
            appendAppLog("[app] selected app: \(first.path)")
        case .failure(let error):
            statusMessage = "Failed to select app: \(error.localizedDescription)"
            appendAppLog("[app] file picker error: \(error.localizedDescription)")
        }
    }

    func startDaemon() {
        guard canStartDaemon else {
            statusMessage = "Unlock admin, launch injection, then start daemon."
            return
        }

        guard FileManager.default.isExecutableFile(atPath: daemonPath) else {
            statusMessage = "xbox_daemon binary not found. Build it first in XboxDaemon/."
            appendAppLog("[app] missing executable: \(daemonPath)")
            return
        }

        let shell = "mkdir -p \(Self.shellQuote(daemonLogURL.deletingLastPathComponent().path)); " +
        ": > \(Self.shellQuote(daemonLogURL.path)); " +
        "cd \(Self.shellQuote(daemonDirectoryPath)); " +
        "env XBOX_EVENT_UDP=127.0.0.1:7947 XBOX_PAIR_CHANNEL=44 XBOX_WLAN_ACK=1 XBOX_MULTI_PAIR=1 ./xbox_daemon > \(Self.shellQuote(daemonLogURL.path)) 2>&1 < /dev/null &"

        let script = "do shell script \(Self.appleScriptString(shell)) with administrator privileges"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]

        do {
            try process.run()
            process.waitUntilExit()
            if process.terminationStatus == 0 {
                daemonRunning = true
                statusMessage = "Daemon started. Controller data should appear in logs."
                appendAppLog("[app] daemon started via administrator privileges")
                startTailingDaemonLog()
            } else {
                statusMessage = "Failed to start daemon."
                appendAppLog("[app] failed to start daemon (exit \(process.terminationStatus))")
            }
        } catch {
            statusMessage = "Could not request admin rights: \(error.localizedDescription)"
            appendAppLog("[app] osascript error: \(error.localizedDescription)")
        }
    }

    func stopDaemon() {
        let shell = "pkill -f \(Self.shellQuote(daemonPath)) || true"
        let script = "do shell script \(Self.appleScriptString(shell)) with administrator privileges"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            appendAppLog("[app] stop daemon request failed: \(error.localizedDescription)")
        }

        daemonTailTask?.cancel()
        daemonTailTask = nil
        daemonRunning = false
        connectedSlots.removeAll()
        connectedControllerCount = 0
        statusMessage = "Daemon stopped."
        appendAppLog("[app] daemon stopped")
    }

    func launchInjectedApp() {
        guard canLaunchInjectedApp, let selectedAppURL else {
            statusMessage = "Select app first."
            return
        }

        guard FileManager.default.isExecutableFile(atPath: launcherPath) else {
            statusMessage = "launch_ryujinx_injected.sh not found."
            appendAppLog("[app] missing launcher: \(launcherPath)")
            return
        }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [launcherPath, selectedAppURL.path]

        var env = ProcessInfo.processInfo.environment
        env["XBOX_INJECT_UDP_PORT"] = "7947"
        env["XBOX_INJECT_AXIS_LAYOUT"] = "sdl"
        env["XBOX_INJECT_STICK_GAIN"] = "1"
        env["XBOX_INJECT_STICK_DEADZONE"] = "500"
        p.environment = env

        let out = Pipe()
        p.standardOutput = out
        p.standardError = out

        out.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let chunk = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor [weak self] in
                self?.appendAppLog(chunk.trimmingCharacters(in: .newlines))
            }
        }

        p.terminationHandler = { [weak self] proc in
            Task { @MainActor [weak self] in
                self?.injectedAppRunning = false
                self?.appendAppLog("[app] injected app exited with code \(proc.terminationStatus)")
            }
        }

        do {
            try p.run()
            injectedProcess = p
            injectedAppRunning = true
            hasLaunchedInjectedApp = true
            statusMessage = "Injected app started."
            appendAppLog("[app] injected launch started for \(selectedAppURL.path)")
        } catch {
            statusMessage = "Failed to launch injected app: \(error.localizedDescription)"
            appendAppLog("[app] launch error: \(error.localizedDescription)")
        }
    }

    func stopInjectedApp() {
        injectedProcess?.terminate()
        injectedProcess = nil
        injectedAppRunning = false
        appendAppLog("[app] requested injected app terminate")
    }

    func unlockAdminPrivileges() {
        let script = "do shell script \"/usr/bin/true\" with administrator privileges"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]

        do {
            try process.run()
            process.waitUntilExit()
            if process.terminationStatus == 0 {
                adminUnlocked = true
                statusMessage = "Administrator access granted for this session."
                appendAppLog("[app] admin privileges unlocked")
            } else {
                appendAppLog("[app] admin unlock failed (exit \(process.terminationStatus))")
            }
        } catch {
            appendAppLog("[app] admin unlock error: \(error.localizedDescription)")
        }
    }

    func clearAppLog() {
        appLogText = ""
        appLogVersion += 1
    }

    func clearDaemonLog() {
        daemonLogText = ""
        daemonLogVersion += 1
    }

    private func startTailingDaemonLog() {
        daemonTailTask?.cancel()
        daemonLogOffset = 0
        daemonTailTask = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                self.pollDaemonLog()
                try? await Task.sleep(for: .milliseconds(350))
            }
        }
    }

    private func pollDaemonLog() {
        guard let handle = try? FileHandle(forReadingFrom: daemonLogURL) else {
            return
        }
        defer { try? handle.close() }

        do {
            try handle.seek(toOffset: daemonLogOffset)
            let data = handle.readDataToEndOfFile()
            guard !data.isEmpty else { return }
            daemonLogOffset += UInt64(data.count)
            if let chunk = String(data: data, encoding: .utf8) {
                appendDaemonLog(chunk.trimmingCharacters(in: .newlines))
                consumeLines(chunk)
            }
        } catch {
            appendAppLog("[app] log tail error: \(error.localizedDescription)")
        }
    }

    private func consumeLines(_ chunk: String) {
        for line in chunk.split(whereSeparator: \.isNewline) {
            parseControllerActivity(String(line))
        }
    }

    private func parseControllerActivity(_ line: String) {
        if line.contains("\"type\":\"connected\"") {
            if let slot = Self.extractSlot(line) {
                connectedSlots.insert(slot)
            }
        } else if line.contains("\"type\":\"disconnected\"") {
            if let slot = Self.extractSlot(line) {
                connectedSlots.remove(slot)
            }
        } else if line.contains("Controller CONNECTED") {
            if let slot = Self.extractTrailingNumber(line) {
                connectedSlots.insert(slot)
            }
        } else if line.contains("Controller DISCONNECTED") {
            if let slot = Self.extractTrailingNumber(line) {
                connectedSlots.remove(slot)
            }
        }

        connectedControllerCount = connectedSlots.count
    }

    private func appendAppLog(_ text: String) {
        guard !text.isEmpty else { return }
        if appLogText.isEmpty {
            appLogText = text
        } else {
            appLogText += "\n\(text)"
        }

        let lines = appLogText.split(whereSeparator: \.isNewline)
        if lines.count > 600 {
            appLogText = lines.suffix(600).joined(separator: "\n")
        }
        appLogVersion += 1
    }

    private func appendDaemonLog(_ text: String) {
        guard !text.isEmpty else { return }
        if daemonLogText.isEmpty {
            daemonLogText = text
        } else {
            daemonLogText += "\n\(text)"
        }

        let lines = daemonLogText.split(whereSeparator: \.isNewline)
        if lines.count > 1200 {
            daemonLogText = lines.suffix(1200).joined(separator: "\n")
        }
        daemonLogVersion += 1
    }

    private static func resolveXboxDaemonDirectory() -> URL {
        let fm = FileManager.default
        var candidates: [URL] = []

        candidates.append(URL(fileURLWithPath: fm.currentDirectoryPath))
        candidates.append(URL(fileURLWithPath: fm.currentDirectoryPath).appendingPathComponent("XboxDaemon"))

        let execURL = URL(fileURLWithPath: CommandLine.arguments.first ?? "")
        var cursor = execURL.deletingLastPathComponent()
        for _ in 0..<8 {
            candidates.append(cursor)
            candidates.append(cursor.appendingPathComponent("XboxDaemon"))
            cursor.deleteLastPathComponent()
        }

        for candidate in candidates {
            let testPath = candidate.appendingPathComponent("launch_ryujinx_injected.sh").path
            if fm.fileExists(atPath: testPath) {
                return candidate
            }
        }

        return URL(fileURLWithPath: fm.currentDirectoryPath).appendingPathComponent("XboxDaemon")
    }

    private static func shellQuote(_ input: String) -> String {
        return "'" + input.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func appleScriptString(_ input: String) -> String {
        let escaped = input
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }

    private static func extractSlot(_ text: String) -> Int? {
        guard let range = text.range(of: "\"slot\":") else { return nil }
        let suffix = text[range.upperBound...]
        let digits = suffix.prefix { $0.isNumber }
        return Int(digits)
    }

    private static func extractTrailingNumber(_ text: String) -> Int? {
        let digits = text.reversed().prefix { $0.isNumber }
        guard !digits.isEmpty else { return nil }
        return Int(String(digits.reversed()))
    }
}
