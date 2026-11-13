import Combine
import Foundation
import Security
import SystemExtensions

@MainActor
final class SystemExtensionActivationManager: NSObject, ObservableObject {
    static let shared = SystemExtensionActivationManager()

    enum DriverState: Equatable {
        case checking
        case inactive
        case active
        case differentBuild(version: String, sameVersion: Bool)
        case unverified
        case waitingForApproval
        case uninstalling
    }

    private enum RequestKind {
        case activation
        case deactivation
    }

    @Published private(set) var driverState: DriverState = .checking
    @Published private(set) var statusMessage = ""
    @Published private(set) var requestInFlight = false
    @Published private(set) var pendingReboot: String?

    var isExtensionKnownActive: Bool { driverState == .active && pendingReboot == nil }

    let extensionIdentifier: String
    private let bundledDriver: Bundle?
    private let bundledCodeHash: Data?
    private var propertiesRequest: OSSystemExtensionRequest?
    private var lifecycleRequest: OSSystemExtensionRequest?
    private var requestKind: RequestKind?
    private var verifyingActivation = false

    override init() {
        let identifier = Bundle.main.bundleIdentifier! + ".driver"
        extensionIdentifier = identifier
        let url = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(identifier + ".dext")
        bundledDriver = Bundle(url: url)
        bundledCodeHash = Self.codeHash(at: url)
        super.init()
        refreshExtensionActiveState()
    }

    func activate() {
        submit(.activation)
    }

    func deactivate() {
        switch driverState {
        case .active, .differentBuild:
            submit(.deactivation)
        default:
            return
        }
    }

    func refreshExtensionActiveState() {
        guard lifecycleRequest == nil, propertiesRequest == nil else { return }
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main
        )
        propertiesRequest = request
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private func submit(_ kind: RequestKind) {
        guard lifecycleRequest == nil, pendingReboot == nil else { return }
        let request: OSSystemExtensionRequest
        switch kind {
        case .activation:
            request = .activationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main)
            statusMessage = "Requesting activation of the bundled driver…"
        case .deactivation:
            request = .deactivationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main)
            statusMessage = "Requesting deactivation of the bundled driver…"
        }
        propertiesRequest = nil
        verifyingActivation = false
        lifecycleRequest = request
        requestKind = kind
        requestInFlight = true
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private static func codeHash(at url: URL) -> Data? {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(url as CFURL, [], &code) == errSecSuccess,
              let code else { return nil }
        var information: CFDictionary?
        let flags = SecCSFlags(rawValue: kSecCSSigningInformation)
        guard SecCodeCopySigningInformation(code, flags, &information) == errSecSuccess,
              let information else { return nil }
        return (information as NSDictionary)[kSecCodeInfoUnique] as? Data
    }

    private func sameVersion(as properties: OSSystemExtensionProperties) -> Bool {
        bundledDriver?.infoDictionary?["CFBundleVersion"] as? String == properties.bundleVersion
            && bundledDriver?.infoDictionary?["CFBundleShortVersionString"] as? String
                == properties.bundleShortVersion
    }

    private func matchesBundledDriver(_ properties: OSSystemExtensionProperties) -> Bool {
        guard sameVersion(as: properties), let bundledCodeHash else { return false }
        return Self.codeHash(at: properties.url) == bundledCodeHash
    }
}

// Both request types deliver their delegate calls on the main queue.
extension SystemExtensionActivationManager: @preconcurrency OSSystemExtensionRequestDelegate {
    func request(
        _ request: OSSystemExtensionRequest,
        foundProperties properties: [OSSystemExtensionProperties]
    ) {
        guard request === propertiesRequest else { return }
        propertiesRequest = nil
        let records = properties.filter { $0.bundleIdentifier == extensionIdentifier }
        let enabled = records.filter { $0.isEnabled && !$0.isUninstalling }
        if enabled.contains(where: matchesBundledDriver) {
            driverState = .active
        } else if records.contains(where: {
            $0.isAwaitingUserApproval && !$0.isUninstalling && matchesBundledDriver($0)
        }) {
            driverState = .waitingForApproval
        } else if let other = enabled.first {
            if sameVersion(as: other),
               (bundledCodeHash == nil || Self.codeHash(at: other.url) == nil) {
                driverState = .unverified
            } else {
                driverState = .differentBuild(
                    version: "\(other.bundleShortVersion) (\(other.bundleVersion))",
                    sameVersion: sameVersion(as: other)
                )
            }
        } else if records.contains(where: { $0.isUninstalling }) {
            driverState = .uninstalling
        } else {
            driverState = .inactive
        }

        if verifyingActivation {
            verifyingActivation = false
            statusMessage = isExtensionKnownActive ? "Bundled driver active." : ""
        }
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        guard request === lifecycleRequest else { return }
        driverState = .waitingForApproval
        statusMessage = "Approve Venus in System Settings → Login Items & Extensions → Driver Extensions."
    }

    func request(
        _ request: OSSystemExtensionRequest,
        actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension replacement: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        guard request === lifecycleRequest, requestKind == .activation else { return .cancel }
        statusMessage = "Replacing the installed driver with the bundled driver."
        return .replace
    }

    func request(
        _ request: OSSystemExtensionRequest,
        didFinishWithResult result: OSSystemExtensionRequest.Result
    ) {
        guard request === lifecycleRequest, let kind = requestKind else { return }
        lifecycleRequest = nil
        requestKind = nil
        requestInFlight = false

        switch result {
        case .completed:
            verifyingActivation = kind == .activation
            statusMessage = verifyingActivation ? "Checking the active driver…" : "Driver deactivated."
            if kind == .deactivation { driverState = .inactive }
        case .willCompleteAfterReboot:
            pendingReboot = kind == .activation
                ? "Restart the Mac to finish activating the bundled driver."
                : "Restart the Mac to finish removing the driver."
            statusMessage = ""
        @unknown default:
            statusMessage = "The system returned an unknown driver request result."
        }
        refreshExtensionActiveState()
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        if request === propertiesRequest {
            propertiesRequest = nil
            if driverState == .checking { driverState = .unverified }
            if verifyingActivation {
                verifyingActivation = false
                statusMessage = "Could not verify the active driver: \(error.localizedDescription)"
            }
            return
        }
        guard request === lifecycleRequest else { return }
        lifecycleRequest = nil
        requestKind = nil
        requestInFlight = false
        statusMessage = "Driver request failed: \(error.localizedDescription)"
        refreshExtensionActiveState()
    }
}
