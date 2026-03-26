/**
 * SpoutManager.h
 * 
 * Manages Spout senders for multiple video channels
 * Sends zero-latency video to vMix via GPU memory sharing
 */

#pragma once

#include <d3d11.h>
#include <string>
#include <map>
#include <memory>

// Forward declaration for Spout SDK
class SpoutSender;

/**
 * Manages multiple Spout senders for video output to vMix
 */
class SpoutManager {
public:
    SpoutManager();
    ~SpoutManager();
    
    // Initialize Spout for a specific channel
    bool CreateSender(int channelID, const std::string& senderName, 
                      unsigned int width, unsigned int height);
    
    // Send a texture to vMix via Spout
    bool SendTexture(int channelID, ID3D11Texture2D* texture);
    
    // Release a specific sender
    void ReleaseSender(int channelID);
    
    // Release all senders
    void ReleaseAll();
    
private:
    struct SpoutChannel {
        SpoutSender* sender;
        std::string name;
        unsigned int width;
        unsigned int height;
        bool isActive;
    };
    
    std::map<int, std::unique_ptr<SpoutChannel>> m_channels;
};
