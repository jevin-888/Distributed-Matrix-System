#pragma once

#include "common/NetworkOptions.h"
#include "common/Protocol.h"
#include "common/SyncTimestampGenerator.h"
#include "common/VideoCacheManager.h"
#include "node/CommandReceiver.h"
#include "node/MediaPlayer.h"
#include "oled/OledDisplay.h"
#include "common/NodeRole.h"
#include "kvm/KvmAgent.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace dms {


struct SlaveNodeOptions {
    uint32_t nodeId = 0;
    std::string nodeIdFile = "/etc/distributed_matrix/node_id";
    NodeRole nodeRole = NodeRole::UNASSIGNED;
    std::string nodeRoleFile = "/etc/distributed_matrix/node_role";
    std::string cacheDir = "/tmp/video_cache";
    uint64_t maxCacheSizeBytes = 400ULL * 1024ULL * 1024ULL;
    std::string idleImagePath = "/usr/share/distributed_matrix/assets/default-idle.png";
    bool hdmiPreview = false;
    std::string startupAnimationPath =
        "/usr/share/distributed_matrix/assets/boot-animation.mp4";
    uint32_t startupAnimationTimeoutMs = 12000;
    MediaPlayerOptions player;
    NetworkOptions network;
    std::string networkConfigFile = "/etc/distributed_matrix/network.conf";
    std::string sourceConfigFile = "/etc/distributed_matrix/source.json";
    KvmOptions kvm;
};

class SlaveNodeApp {
public:
    enum class PlayState {
        IDLE,
        PREPARING,
        READY,
        READY_PAUSED,
        PLAYING,
        PAUSED,
        ERROR
    };

    SlaveNodeApp();
    ~SlaveNodeApp();

    SlaveNodeApp(const SlaveNodeApp&) = delete;
    SlaveNodeApp& operator=(const SlaveNodeApp&) = delete;

    bool initialize(const SlaveNodeOptions& options);
    bool start();
    void shutdown();

    uint32_t getNodeId() const { return m_nodeId.load(); }
    bool isRunning() const { return m_running.load(); }
    PlayState getPlayState() const { return m_playState.load(); }
    std::string getLastError() const;

private:
    std::string getHostname() const;
    std::string getSerialNumber() const;
    std::pair<std::string, std::string> getNetworkRatesMbps();
    void onPreparePlayCommand(uint32_t commandId,
                              const Protocol::SyncPlayCommand& command);
    void onCommitPlayCommand(uint32_t commandId);
    void onSyncPlayCommand(const std::string& videoUrl,
                           uint32_t videoWidth,
                           uint32_t videoHeight,
                           uint64_t syncTimestamp,
                           const std::vector<Protocol::CropRegion>& crops);
    void onStopCommand();
    void onPauseCommand();
    void onResumeCommand();
    void onSetAudioOutputCommand(AudioOutputMode mode);
    void onSetAudioVolumeCommand(uint32_t volumePercent);
    void onSetDisplayLayoutCommand(const Protocol::SetDisplayLayoutCommand& command);
    void onSetWindowLayoutCommand(const Protocol::SetWindowLayoutCommand& command);
    void onSetNodeSourceCommand(const Protocol::SetNodeSourceCommand& command);
    void onSetNodeIdCommand(const Protocol::SetNodeIdCommand& command);

    bool prepareVideo(const std::string& videoUrl,
                      uint32_t videoWidth,
                      uint32_t videoHeight,
                      const Protocol::CropRegion& cropRegion,
                      uint64_t generation);
    bool startPlayback(uint64_t syncTimestamp);
    bool showStartupAnimation();
    bool showIdleImage();
    bool showIdleFallbackImage();
    void stopPlayback(bool restoreIdleImage = true);
    void pausePlayback();
    void resumePlayback();
    void playbackMonitorLoop();
    void schedulerLoop();

    void heartbeatThread();
    void discoveryThread();
    bool broadcastHeartbeat();
    bool setNodeRole(NodeRole role, bool persist);
    bool setNodeNetwork(const Protocol::SetNodeNetworkCommand& command);
    bool loadNodeRole();
    bool loadNodeId();
    bool persistNodeId(uint32_t nodeId);
    bool loadNetworkConfig();
    bool persistNetworkConfig(const Protocol::SetNodeNetworkCommand& command);
    bool loadSourceConfig();
    bool persistSourceConfig(const SignalSourceConfig& source);
    std::string createHeartbeatMessage();
    std::string getLocalIp() const;
    std::string getDeviceModel() const;
    std::string getDeviceName() const;
    std::string getSoftwareVersion() const;
    std::string getBoardInfo() const;
    std::string getMacAddress() const;
    std::string getSubnetMask() const;
    std::string getGateway() const;
    std::string getDeviceType() const;
    std::string getDisplayResolution() const;
    double getCpuUsagePercent();
    double getMemoryUsagePercent() const;
    static const char* playStateName(PlayState state);
    void setError(const std::string& error);

    std::atomic<uint32_t> m_nodeId;
    std::string m_nodeIdFile;
    std::atomic<NodeRole> m_nodeRole;
    std::string m_nodeRoleFile;
    std::string m_cacheDir;
    std::string m_idleImagePath;
    std::string m_startupAnimationPath;
    uint32_t m_startupAnimationTimeoutMs;
    NetworkOptions m_networkOptions;
    std::string m_networkConfigFile;
    Protocol::SetNodeNetworkCommand m_appliedNetworkConfig;
    bool m_hasAppliedNetworkConfig;
    SignalSourceConfig m_sourceConfig;
    std::string m_sourceConfigFile;
    bool m_hasActiveSignalCrop;
    uint32_t m_activeSignalSourceWidth;
    uint32_t m_activeSignalSourceHeight;
    Protocol::CropRegion m_activeSignalCrop;
    SyncTimestampGenerator m_masterClock;

    std::unique_ptr<CommandReceiver> m_receiver;
    std::unique_ptr<VideoCacheManager> m_cacheManager;
    std::unique_ptr<MediaPlayer> m_player;
    std::unique_ptr<KvmAgent> m_kvmAgent;
    OledDisplay m_oled;

    std::atomic<bool> m_initialized;
    std::atomic<bool> m_running;
    std::atomic<PlayState> m_playState;
    std::atomic<AudioOutputMode> m_audioOutputMode;

    std::thread m_heartbeatThread;
    std::thread m_discoveryThread;
    std::thread m_schedulerThread;
    std::thread m_playbackThread;
    std::atomic<uint64_t> m_prepareGeneration;
    std::atomic<uint32_t> m_currentCommandId;
    std::atomic<uint32_t> m_pendingCommitCommandId;
    std::atomic<bool> m_commitReceived;
    std::mutex m_prepareTasksMutex;
    std::vector<std::future<void>> m_prepareTasks;

    mutable std::mutex m_stateMutex;
    std::mutex m_displayModeMutex;
    std::string m_currentVideoUrl;
    std::string m_currentVideoPath;
    Protocol::CropRegion m_currentCropRegion;
    uint64_t m_syncTimestamp;
    uint64_t m_actualStartTimestamp;
    std::string m_lastError;

    std::mutex m_cpuMutex;
    uint64_t m_previousCpuTotal;
    uint64_t m_previousCpuIdle;
    std::mutex m_networkRateMutex;
    uint64_t m_previousTxBytes;
    uint64_t m_previousRxBytes;
    uint64_t m_previousNetworkSampleMs;
    int m_discoverySocket;
};

} // namespace dms
