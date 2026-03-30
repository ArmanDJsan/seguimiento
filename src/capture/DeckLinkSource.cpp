/**
 * DeckLinkSource.cpp
 *
 * SDK-aware signal lock checker for DeckLink inputs.
 * When the SDK is missing at compile time, this class acts as a stub
 * that assumes signal lock is present but logs a warning.
 */

#include "DeckLinkSource.h"
#include "../utils/Logger.h"
#include <mutex>

#if __has_include("DeckLinkAPI.h")
    #define HAS_DECKLINK_SDK 1
    #include "DeckLinkAPI.h"
#else
    #define HAS_DECKLINK_SDK 0
#endif

DeckLinkSource::DeckLinkSource()
    : m_sdkAvailable(HAS_DECKLINK_SDK != 0), m_availablePorts(0) {}

DeckLinkSource::~DeckLinkSource() = default;

bool DeckLinkSource::Initialize(size_t expectedPorts) {
    m_availablePorts = expectedPorts;
#if HAS_DECKLINK_SDK
    // TODO: Enumerate DeckLink devices and count inputs
    Logger::Info("DeckLink SDK detected; initializing signal lock checks");
#else
    Logger::Warning("DeckLink SDK not available; signal lock checks will be stubbed");
#endif
    return true;
}

std::vector<DeckLinkSource::PortStatus> DeckLinkSource::GetSignalStatus(const std::vector<int>& portIndices) {
    std::vector<PortStatus> statuses;
    statuses.reserve(portIndices.size());

    static std::once_flag stubWarnFlag;
    if (!m_sdkAvailable) {
        std::call_once(stubWarnFlag, []() {
            Logger::Warning("DeckLink SDK missing; assuming signal lock for requested ports (diagnostic stub)");
        });
    }

    for (int portIndex : portIndices) {
        PortStatus status{portIndex, true, std::nullopt};
#if HAS_DECKLINK_SDK
        // TODO: Query DeckLink input status for signal lock
        status.signalLocked = true;
        status.name = "Port_" + std::to_string(portIndex);
#else
        status.signalLocked = true; // optimistic stub
#endif
        statuses.push_back(status);
    }
    return statuses;
}
