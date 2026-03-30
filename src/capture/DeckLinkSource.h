/**
 * DeckLinkSource.h
 *
 * Lightweight abstraction to query signal lock status for physical
 * DeckLink inputs. Falls back to a stub when the SDK is unavailable.
 */

#pragma once

#include <string>
#include <vector>
#include <optional>

class DeckLinkSource {
public:
    struct PortStatus {
        int index;
        bool signalLocked;
        std::optional<std::string> name;
    };

    DeckLinkSource();
    ~DeckLinkSource();

    // Initialize 12 physical ports (3 cards x 4 ports) for Threadripper rig
    bool Initialize(size_t expectedPorts = 12);

    // Query lock status for provided port indices
    std::vector<PortStatus> GetSignalStatus(const std::vector<int>& portIndices);

private:
    bool m_sdkAvailable;
    size_t m_availablePorts;
};
