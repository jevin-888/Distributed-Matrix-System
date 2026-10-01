#pragma once

#include "common/NetworkOptions.h"
#include "common/Protocol.h"
#include "common/VideoCacheManager.h"
#include "master/CommandBroadcaster.h"
#include "master/HTTPFileServer.h"
#include "master/NodeDiscovery.h"
#include "kvm/KvmSessionManager.h"

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dms {


struct MasterNodeOptions {
    uint16_t httpPort = HTTP_PORT;
    std::string cacheDir = "/tmp/video_cache";
    uint64_t maxCacheSizeBytes = 400ULL * 1024ULL * 1024ULL;
    std::string webRoot = "web";
    std::string regionsFile;
    uint64_t defaultSyncDelayMs = 3000;
    ScreenLayout layout{2, 2, 1920, 1080};
    NetworkOptions network;
};

struct MasterPlaybackStatus {
    std::string state = "idle";
    std::string videoUrl;
    uint32_t commandId = 0;
    uint64_t syncTimestamp = 0;
    uint64_t updatedAt = 0;
    uint32_t expectedNodeCount = 0;
    uint32_t readyNodeCount = 0;
    uint32_t playingNodeCount = 0;
    uint32_t errorNodeCount = 0;
    uint32_t missingNodeCount = 0;
    int64_t maxStartSkewMs = 0;
};

class MasterNodeApp {
public:
    MasterNodeApp();
    ~MasterNodeApp();

    MasterNodeApp(const MasterNodeApp&) = delete;
    MasterNodeApp& operator=(const MasterNodeApp&) = delete;

    bool initialize(const MasterNodeOptions& options);
    void shutdown();

    bool play(const std::string& videoUrl,
              const VideoInfo& videoInfo,
              uint64_t delayMs);
    bool stop();
    bool pause();
    bool resume();
    bool setAudioOutputMode(AudioOutputMode mode);
    AudioOutputMode getAudioOutputMode() const;
    bool setAudioVolumePercent(uint32_t volumePercent);
    uint32_t getAudioVolumePercent() const;
    KvmSession getKvmSession() const;
    bool acquireKvmSession(uint32_t controllerNodeId, uint32_t targetNodeId,
                           uint64_t leaseMs, KvmSession& session);
    bool releaseKvmSession(uint32_t sessionId);
    bool setNodeRole(uint32_t nodeId, NodeRole role);
    bool setNodeNetwork(const Protocol::SetNodeNetworkCommand& command);
    bool setNodeSource(const Protocol::SetNodeSourceCommand& command);

    bool setScreenLayout(const ScreenLayout& layout);
    bool setScreenLayout(const ScreenLayout& layout,
                        const std::vector<ScreenPlacement>& placements);
    // Update the active layout and refresh an in-progress playback command.
    // The REST layer uses this entry point for live layout edits.
    bool updateScreenLayout(const ScreenLayout& layout,
                            const std::vector<ScreenPlacement>& placements);
    ScreenLayout getScreenLayout() const;
    std::vector<ScreenPlacement> getScreenPlacements() const;
    bool updateWindowLayout(const std::vector<WindowPlacement>& placements);
    std::vector<WindowPlacement> getWindowPlacements() const;
    MasterPlaybackStatus getPlaybackStatus() const;

    std::string getLocalHttpUrl() const;
    bool preloadVideo(const std::string& videoUrl);
    bool isRunning() const { return m_running.load(); }

private:
    bool syncPlay(const std::string& videoUrl,
                  const VideoInfo& videoInfo,
                  const std::vector<Protocol::CropRegion>& layoutConfig,
                  uint64_t delayMs);
    bool replayCurrentLayoutLocked();
    bool applyWindowLayoutLocked();
    bool setLocalIp(const std::string& localIp);
    std::string convertToAccessibleUrl(const std::string& videoUrl);
    void updatePlaybackStatus(const std::string& state,
                              const std::string& videoUrl = {},
                              uint32_t commandId = 0,
                              uint64_t syncTimestamp = 0);
    void setExpectedNodeIds(const std::vector<Protocol::CropRegion>& layoutConfig);
    void clearExpectedNodeIds();
    MasterPlaybackStatus aggregateNodeStatus(const MasterPlaybackStatus& status) const;
    void kvmLeaseLoop();
    void layoutSyncLoop();

    std::unique_ptr<CommandBroadcaster> m_broadcaster;
    std::unique_ptr<VideoCacheManager> m_cacheManager;
    std::unique_ptr<dms::HTTPFileServer> m_httpServer;
    std::unique_ptr<NodeDiscovery> m_nodeDiscovery;
    std::unique_ptr<KvmSessionManager> m_kvmSessionManager;

    mutable std::mutex m_stateMutex;
    mutable std::mutex m_commandMutex;
    std::string m_localIp;
    uint16_t m_localHttpPort;
    std::string m_localHttpUrl;
    std::string m_cacheDir;
    ScreenLayout m_screenLayout;
    std::vector<ScreenPlacement> m_screenPlacements;
    std::vector<WindowPlacement> m_windowPlacements;
    std::map<uint32_t, SignalSourceConfig> m_nodeSourceOverrides;
    VideoInfo m_currentVideoInfo{0, 0, 0, 0};
    uint64_t m_currentPlaybackDelayMs = 3000;
    AudioOutputMode m_audioOutputMode;
    uint32_t m_audioVolumePercent;
    MasterPlaybackStatus m_playbackStatus;
    std::set<uint32_t> m_expectedNodeIds;
    mutable std::set<uint32_t> m_observedCommandNodeIds;

    std::atomic<bool> m_running;
    std::thread m_kvmLeaseThread;
    std::thread m_layoutSyncThread;
    std::set<uint32_t> m_layoutSyncNodeIds;
    std::map<std::string, uint64_t> m_nodeAssignmentAttempts;
};

} // namespace dms
