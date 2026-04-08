/**
 * main_test.cpp
 *
 * Standalone diagnostic harness for Phase 1 (Hardware Sweep).
 * - Checks vMix HTTP API for input health (no Offline/Error)
 * - Sweeps VideoHub inputs 1..12 and reports DeckLink signal lock status
 *
 * Intended for manual compilation on Windows with Blackmagic SDK installed
 * to validate header/library linkage and basic connectivity.
 */

#include <iostream>
#include <vector>
#include <unordered_map>
#include <numeric>
#include <sstream>
#include <iomanip>

#include "../control/VMixController.h"
#include "../control/VideoHubClient.h"
#include "../capture/DeckLinkSource.h"
#include "../utils/Logger.h"

constexpr int kPrimaryOutput = 0;

std::unordered_map<std::string, int> BuildInputLookup() {
    std::unordered_map<std::string, int> lookup;
    // VideoHub uses 0-based indexing for inputs
    for (int i = 1; i <= 12; ++i) {
        std::stringstream ss;
        ss << "CAM_" << std::setw(2) << std::setfill('0') << i;
        lookup[ss.str()] = i - 1;  // Convert to 0-based (CAM_01 = input 0, etc.)
    }
    return lookup;
}

std::vector<int> RangeInclusive(int start, int end) {
    std::vector<int> values(static_cast<size_t>(end - start + 1));
    std::iota(values.begin(), values.end(), start);
    return values;
}

int main() {
    Logger::Init("VIB_Phase1_Test");
    Logger::Info("Starting Phase 1 diagnostic harness (vMix + DeckLink 12 ports)...");

    // Configure controllers (adjust IP/ports as needed for your setup)
    VMixController vmix(L"127.0.0.1", 8088, 8099);
    VideoHubClient videoHub("192.168.1.10", 9990, BuildInputLookup());
    DeckLinkSource deckLinkSource;
    deckLinkSource.Initialize(12);

    // Check vMix diagnostics
    bool vmixOk = vmix.CheckInputsHealthy();
    std::cout << "[vMix] Inputs healthy: " << (vmixOk ? "YES" : "NO") << std::endl;

    // Connect to VideoHub (best-effort)
    if (!videoHub.Connect()) {
        std::cout << "[VideoHub] Connection failed; cannot sweep ports." << std::endl;
        return 1;
    }

    // Sweep ports 1..12 on primary output and query signal status
    auto ports = RangeInclusive(1, 12);
    for (int port : ports) {
        // VideoHub uses 0-based indexing, so convert port (1-based) to 0-based
        int videoHubInput = port - 1;
        if (!videoHub.RouteInputToOutput(kPrimaryOutput, videoHubInput)) {
            std::cout << "[VideoHub] Routing failed for port " << port << std::endl;
            return 1;
        }
    }

    const auto statuses = deckLinkSource.GetSignalStatus(ports);
    std::cout << "DeckLink SignalLock status (ports 1-12):" << std::endl;
    for (const auto& status : statuses) {
        std::cout << "  Port " << status.index << ": "
                  << (status.signalLocked ? "LOCKED" : "NO SIGNAL");
        if (status.name.has_value()) {
            std::cout << " (" << status.name.value() << ")";
        }
        std::cout << std::endl;
    }

    if (vmixOk) {
        Logger::Info("Phase 1 diagnostic completed (vMix OK).");
    } else {
        Logger::Warning("Phase 1 diagnostic completed with vMix warnings.");
    }

    return vmixOk ? 0 : 1;
}
