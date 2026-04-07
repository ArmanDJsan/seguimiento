/**
 * VideoHubClient.h
 *
 * Base TCP client for controlling Blackmagic VideoHub routing.
 * Provides simple routing and labeling helpers using the logical names
 * defined in the configuration file.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
// winsock2.h must be included before windows.h to avoid conflicts with winsock.h
#include <winsock2.h>

class VideoHubClient {
public:
    VideoHubClient(const std::string& host,
                   uint16_t port,
                   std::unordered_map<std::string, int> inputLookup = {});
    ~VideoHubClient();

    bool Connect();
    void Disconnect();
    bool IsConnected() const;

    bool RouteInputToOutput(int outputIndex, int sourceIndex);
    bool RouteInputToOutput(int outputIndex, const std::string& sourceName);

    // Route all outputs to matching inputs (1->1, 2->2, ..., count->count)
    bool RouteAllOutputsToMatchingInputs(int count = 16);

    bool RefreshInputLabels(const std::unordered_map<int, std::string>& labels);

private:
    bool InitializeWinsock();
    bool SendCommand(const std::string& payload);
    void LogSocketError(const std::string& context);

    std::string m_host;
    uint16_t m_port;
    SOCKET m_socket;
    bool m_winsockInitialized;
    std::unordered_map<std::string, int> m_inputLookup;
    int m_maxSourceIndex;
};
