import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var vm: ControlCenterViewModel
    @State private var showFileImporter = false
    @State private var showDebug = false
    @State private var selectedDebugTab = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            header
            steps
            status
            Spacer(minLength: 0)
            debugConsole
        }
        .padding(24)
        .fileImporter(
            isPresented: $showFileImporter,
            allowedContentTypes: [UTType.application],
            allowsMultipleSelection: false
        ) { result in
            vm.handleFileImport(result)
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("xoneDarwin Control Center")
                .font(.largeTitle.weight(.semibold))
            Text("A macOS helper for Xbox Wireless Adapter sessions with 3-4 controllers. For casual single-controller use, Bluetooth is usually simpler.")
                .font(.body)
                .foregroundStyle(.secondary)
        }
    }

    private var steps: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 12) {
                    Button("1. Select App") {
                        showFileImporter = true
                    }
                    .buttonStyle(.borderedProminent)

                    Text(vm.selectedAppLabel)
                        .font(.callout)
                        .foregroundStyle(vm.selectedAppURL == nil ? .secondary : .primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                if vm.selectedAppURL != nil {
                    HStack(spacing: 12) {
                        Button(vm.adminUnlocked ? "Admin Unlocked" : "2. Unlock Admin") {
                            vm.unlockAdminPrivileges()
                        }
                        .buttonStyle(.bordered)
                        .disabled(vm.adminUnlocked)

                        Button("3. Launch with SDL Injection") {
                            vm.launchInjectedApp()
                        }
                        .buttonStyle(.bordered)
                        .disabled(!vm.canLaunchInjectedApp)

                        Button(vm.daemonRunning ? "Daemon Running" : "4. Start Daemon") {
                            vm.startDaemon()
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!vm.canStartDaemon)
                    }
                }

                if vm.selectedAppURL != nil {
                    Text("Tip: Launch injection first, then start the daemon for controller streaming.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
        } label: {
            Label("Workflow", systemImage: "list.number")
        }
    }

    private var status: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Label(vm.daemonRunning ? "Daemon online" : "Daemon offline", systemImage: vm.daemonRunning ? "checkmark.circle.fill" : "xmark.circle")
                        .foregroundStyle(vm.daemonRunning ? .green : .secondary)

                    Spacer()

                    Label("Connected controllers: \(vm.connectedControllerCount)", systemImage: "gamecontroller")
                        .font(.headline)
                }

                Text(vm.statusMessage)
                    .font(.callout)
                    .foregroundStyle(.secondary)

                HStack {
                    Button("Stop Daemon") {
                        vm.stopDaemon()
                    }
                    .disabled(!vm.daemonRunning)

                    Button("Stop Injected App") {
                        vm.stopInjectedApp()
                    }
                    .disabled(!vm.injectedAppRunning)
                }
            }
        } label: {
            Label("Runtime", systemImage: "gauge.with.dots.needle.67percent")
        }
    }

    private var debugConsole: some View {
        DisclosureGroup("Debug Console", isExpanded: $showDebug) {
            HStack {
                Picker("Log Source", selection: $selectedDebugTab) {
                    Text("Application").tag(0)
                    Text("Daemon").tag(1)
                }
                .pickerStyle(.segmented)

                Button("Clear") {
                    if selectedDebugTab == 0 {
                        vm.clearAppLog()
                    } else {
                        vm.clearDaemonLog()
                    }
                }
            }

            ScrollViewReader { proxy in
                ScrollView {
                    Text(selectedDebugTab == 0 ? vm.appLogText : vm.daemonLogText)
                        .font(.system(.footnote, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)

                    Color.clear
                        .frame(height: 1)
                        .id("debug-bottom")
                }
                .onChange(of: vm.appLogVersion) { _, _ in
                    guard selectedDebugTab == 0 else { return }
                    withAnimation(.easeOut(duration: 0.12)) {
                        proxy.scrollTo("debug-bottom", anchor: .bottom)
                    }
                }
                .onChange(of: vm.daemonLogVersion) { _, _ in
                    guard selectedDebugTab == 1 else { return }
                    withAnimation(.easeOut(duration: 0.12)) {
                        proxy.scrollTo("debug-bottom", anchor: .bottom)
                    }
                }
                .onChange(of: selectedDebugTab) { _, _ in
                    withAnimation(.easeOut(duration: 0.12)) {
                        proxy.scrollTo("debug-bottom", anchor: .bottom)
                    }
                }
            }
            .frame(maxHeight: 220)
            .background(.quaternary.opacity(0.3), in: RoundedRectangle(cornerRadius: 10))
        }
        .font(.callout)
    }
}
