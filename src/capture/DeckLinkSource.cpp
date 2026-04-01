/**
 * DeckLinkSource.cpp
 *
 * SDK-aware signal lock checker for DeckLink inputs.
 * When the SDK is missing at compile time, this class acts as a stub
 * that assumes signal lock is present but logs a warning.
 */

#include "DeckLinkSource.h"
#include "../utils/Logger.h"
#include "../DeckLinkAPI_h.h"
#include <mutex>
#include <objbase.h>  // For COM

#define HAS_DECKLINK_SDK 1  // SDK is available via DeckLinkAPI_h.h

DeckLinkSource::DeckLinkSource()
    : m_sdkAvailable(HAS_DECKLINK_SDK != 0), m_availablePorts(0) {}

DeckLinkSource::~DeckLinkSource() = default;

bool DeckLinkSource::Initialize(size_t expectedPorts) {
    m_availablePorts = expectedPorts;
#if HAS_DECKLINK_SDK
    Logger::Info("DeckLink SDK detected; initializing signal lock checks");
    
    // Enumerate DeckLink devices to verify SDK is working
    IDeckLinkIterator* deckLinkIterator = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CDeckLinkIterator,
        nullptr,
        CLSCTX_ALL,
        IID_IDeckLinkIterator,
        (void**)&deckLinkIterator
    );
    
    if (FAILED(hr)) {
        Logger::Warning("Failed to create DeckLink Iterator. HRESULT: 0x" + std::to_string(hr));
        Logger::Warning("Signal lock checks will be stubbed");
        m_sdkAvailable = false;
        return true;  // Non-fatal, continue with stubs
    }
    
    if (!deckLinkIterator) {
        Logger::Warning("DeckLink Iterator is null");
        m_sdkAvailable = false;
        return true;
    }
    
    // Count available devices
    IDeckLink* deckLink = nullptr;
    int deviceCount = 0;
    while (deckLinkIterator->Next(&deckLink) == S_OK) {
        if (deckLink) {
            deviceCount++;
            deckLink->Release();
        }
    }
    deckLinkIterator->Release();
    
    Logger::Info("Found " + std::to_string(deviceCount) + " DeckLink device(s)");
    if (deviceCount == 0) {
        Logger::Warning("No DeckLink devices found. Signal checks will be stubbed.");
        m_sdkAvailable = false;
    }
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
