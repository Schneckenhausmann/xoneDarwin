// HostApp_ActivateExtension.swift
// Drop this into a minimal macOS SwiftUI / AppKit host app.
// The host app is REQUIRED by Apple — DriverKit extensions cannot be
// installed standalone; they must be embedded in an app bundle.
//
// Entitlement needed on the HOST APP target:
//   com.apple.developer.system-extension.install = true

import SystemExtensions
import os

private let dextIdentifier = "com.yourcompany.XboxOneWirelessDriver"
private let log = Logger(subsystem: "com.yourcompany.XboxOneWirelessDriverApp",
                          category: "Extension")

// ──────────────────────────────────────────────
// Call this from your AppDelegate / @main on startup
// ──────────────────────────────────────────────
func activateXboxDriverExtension() {
    let request = OSSystemExtensionRequest
        .activationRequest(forExtensionWithIdentifier: dextIdentifier,
                           queue: .main)
    request.delegate = ExtensionRequestDelegate.shared
    OSSystemExtensionManager.shared.submitRequest(request)
    log.info("Submitted activation request for \(dextIdentifier)")
}

func deactivateXboxDriverExtension() {
    let request = OSSystemExtensionRequest
        .deactivationRequest(forExtensionWithIdentifier: dextIdentifier,
                             queue: .main)
    request.delegate = ExtensionRequestDelegate.shared
    OSSystemExtensionManager.shared.submitRequest(request)
    log.info("Submitted deactivation request for \(dextIdentifier)")
}

// ──────────────────────────────────────────────
// Delegate for activation result callbacks
// ──────────────────────────────────────────────
final class ExtensionRequestDelegate: NSObject, OSSystemExtensionRequestDelegate {

    static let shared = ExtensionRequestDelegate()

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction
    {
        log.info("Replacing extension \(existing.bundleShortVersion) → \(ext.bundleShortVersion)")
        return .replace  // always update to the bundled version
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        // macOS shows a system prompt: user must approve in System Settings
        // → Privacy & Security → allow the extension.
        log.warning("User approval required — prompt shown in System Settings")
        // Show an alert guiding the user to System Settings:
        DispatchQueue.main.async {
            let alert = NSAlert()
            alert.messageText     = "Xbox Wireless Driver requires approval"
            alert.informativeText = "Please open System Settings → Privacy & Security "
                                  + "and allow the Xbox driver extension to run."
            alert.addButton(withTitle: "Open System Settings")
            alert.addButton(withTitle: "Later")
            if alert.runModal() == .alertFirstButtonReturn {
                NSWorkspace.shared.open(
                    URL(string: "x-apple.systempreferences:com.apple.preference.security")!)
            }
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            log.info("Extension activated successfully")
        case .willCompleteAfterReboot:
            log.warning("Extension will activate after reboot")
        @unknown default:
            log.error("Unknown result: \(result.rawValue)")
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: Error) {
        log.error("Extension activation failed: \(error.localizedDescription)")
    }
}
