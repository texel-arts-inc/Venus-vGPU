import AppKit
import Foundation
import SwiftUI

@MainActor
final class VenusMicroVM: ObservableObject {
    static let shared = VenusMicroVM()

    @Published private(set) var status = "Waiting for DriverKit."
    @Published private(set) var running = false
    @Published private(set) var disconnecting = false
    private var manager: Process?
    private var pending = Data()
    private var driverActive = false

    func driverChanged(active: Bool) {
        driverActive = active
        if !active {
            stop()
            status = "Waiting for DriverKit."
        } else if manager == nil && !disconnecting {
            status = "Start Venus when the GPU is connected."
        }
    }

    func start() {
        guard driverActive, manager == nil, !disconnecting else { return }
        let helper = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Helpers/venus-vgpud")
        let child = Process()
        let output = Pipe()
        child.executableURL = helper
        child.standardOutput = output
        child.standardError = output
        pending.removeAll()
        status = "Starting the Venus microVM…"
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            if data.isEmpty {
                handle.readabilityHandler = nil
            } else {
                DispatchQueue.main.async {
                    guard let self, self.manager === child, !self.disconnecting else { return }
                    self.receive(data)
                }
            }
        }
        child.terminationHandler = { [weak self] process in
            let result = process.terminationStatus
            DispatchQueue.main.async {
                guard let self else { return }
                self.manager = nil
                self.running = false
                if self.disconnecting {
                    Task { await self.checkDriverRelease(using: helper) }
                } else if result != 0 &&
                            (self.status == "Venus vGPU ready" ||
                             self.status == "Starting the Venus microVM…") {
                    self.status = "Venus exited (status \(result)). See last-guest.log in Application Support/Venus vGPU."
                }
            }
        }
        do {
            try child.run()
            manager = child
            running = true
        } catch {
            output.fileHandleForReading.readabilityHandler = nil
            status = "Could not launch Venus: \(error.localizedDescription)"
        }
    }

    func stop() {
        guard let manager, manager.isRunning else { return }
        guard !disconnecting else { return }
        disconnecting = true
        status = "Stopping Venus and releasing the GPU…"
        manager.terminate()
    }

    private func checkDriverRelease(using helper: URL) async {
        defer { disconnecting = false }
        for _ in 0..<20 {
            if await Self.driverIsIdle(using: helper) {
                status = "GPU released. You can unplug the enclosure."
                return
            }
            try? await Task.sleep(for: .milliseconds(150))
        }
        status = "Could not confirm GPU release. Keep the enclosure connected."
    }

    private nonisolated static func driverIsIdle(using helper: URL) async -> Bool {
        await withCheckedContinuation { continuation in
            let probe = Process()
            probe.executableURL = helper
            probe.arguments = ["--probe"]
            probe.standardOutput = FileHandle.nullDevice
            probe.standardError = FileHandle.nullDevice
            probe.terminationHandler = { continuation.resume(returning: $0.terminationStatus == 0) }
            do {
                try probe.run()
            } catch {
                continuation.resume(returning: false)
            }
        }
    }

    private func receive(_ data: Data) {
        pending.append(data)
        while let newline = pending.firstIndex(of: 10) {
            let line = String(decoding: pending[..<newline], as: UTF8.self)
            pending.removeSubrange(...newline)
            if !line.isEmpty {
                status = line
            }
        }
    }
}
