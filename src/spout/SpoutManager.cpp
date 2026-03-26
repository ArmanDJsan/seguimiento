/**
 * SpoutManager.cpp
 * 
 * Implementation of Spout video sender management
 */

#include "SpoutManager.h"
#include "../utils/Logger.h"

SpoutManager::SpoutManager() {
    Logger::Info("SpoutManager initialized");
}

SpoutManager::~SpoutManager() {
    ReleaseAll();
}

bool SpoutManager::CreateSender(int channelID, const std::string& senderName,
                                 unsigned int width, unsigned int height) {
    Logger::Info("Creating Spout sender: " + senderName + " (" + 
                 std::to_string(width) + "x" + std::to_string(height) + ")");
    
    // Check if sender already exists
    if (m_channels.find(channelID) != m_channels.end()) {
        Logger::Warning("Spout sender already exists for channel " + std::to_string(channelID));
        return false;
    }
    
    // Create new Spout channel
    auto channel = std::make_unique<SpoutChannel>();
    channel->name = senderName;
    channel->width = width;
    channel->height = height;
    channel->isActive = false;
    
    // TODO: Initialize Spout SDK
    // channel->sender = new SpoutSender();
    // channel->sender->SetSenderName(senderName.c_str());
    // channel->isActive = true;
    
    m_channels[channelID] = std::move(channel);
    
    Logger::Info("Spout sender created successfully: " + senderName);
    return true;
}

bool SpoutManager::SendTexture(int channelID, ID3D11Texture2D* texture) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) {
        return false;
    }
    
    if (!texture) {
        return false;
    }
    
    // TODO: Send texture via Spout
    // This is the critical zero-latency path to vMix
    // it->second->sender->SendTexture(texture, it->second->width, it->second->height);
    
    return true;
}

void SpoutManager::ReleaseSender(int channelID) {
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        Logger::Info("Releasing Spout sender: " + it->second->name);
        
        // TODO: Release Spout resources
        // if (it->second->sender) {
        //     it->second->sender->ReleaseSender();
        //     delete it->second->sender;
        // }
        
        m_channels.erase(it);
    }
}

void SpoutManager::ReleaseAll() {
    Logger::Info("Releasing all Spout senders");
    
    for (auto& pair : m_channels) {
        // TODO: Release each sender
        // if (pair.second->sender) {
        //     pair.second->sender->ReleaseSender();
        //     delete pair.second->sender;
        // }
    }
    
    m_channels.clear();
}
