#pragma once

#include "common/Protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dms {

struct AudioPlaybackOutput {
    AudioOutputMode output = AudioOutputMode::HDMI;
    std::string device;
    double volume = 1.0;
};

struct CaptureOptions {
    std::string devicePath = "/dev/video0";
    std::string pixelFormat = "auto";
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framerateNumerator = 0;
    uint32_t framerateDenominator = 1;
    std::string colorimetry = "auto";
    bool audioEnabled = false;
    std::string audioCaptureDevice = "hw:CARD=hdmiin,DEV=0";
    AudioOutputMode audioOutputMode = AudioOutputMode::BOTH;
    AudioPlaybackOutput hdmiAudioOutput{
        AudioOutputMode::HDMI, "hw:CARD=rockchiphdmi,DEV=0", 16.0};
    AudioPlaybackOutput analogAudioOutput{
        AudioOutputMode::ANALOG, "hw:CARD=rk809,DEV=0", 1.0};
};

struct MediaPlayerOptions {
    bool fullscreen = true;
    uint32_t connectorId = 0;
    uint32_t planeId = 0;
    uint32_t backgroundPlaneId = 0;
    // DRM/KMS planes used for hardware-composited window layers.  A separate
    // plane is required for every layer; an empty list disables multi-window
    // output instead of falling back to a CPU compositor.
    std::vector<uint32_t> overlayPlaneIds;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t startupTimeoutMs = 10000;
    CaptureOptions capture;
};

/**
 * Controls the production playback paths. Window layouts use one MPP decoder
 * per source and one DRM/KMS overlay plane per layer; no CPU compositor is
 * present in that path.
 */
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    bool initialize(uint32_t nodeId, const MediaPlayerOptions& options);
    bool setNodeId(uint32_t nodeId);
    bool showImage(const std::string& imagePath, const std::string& nodeIp);
    bool showCapture();
    bool showSignal(const SignalSourceConfig& source);
    bool showSignalPreview(const SignalSourceConfig& source);
    bool showWindowLayout(const Protocol::SetWindowLayoutCommand& command);
    bool setSignalOutputActive(bool active);
    bool isSignalOutputActive() const;
    bool supportsH264Rtp() const;
    std::shared_ptr<const std::vector<uint8_t>> getLatestPreviewFrame() const;
    bool updateSignalCrop(uint32_t sourceWidth,
                          uint32_t sourceHeight,
                          const Protocol::CropRegion& cropRegion);
    bool captureAvailable(const SignalSourceConfig& source);
    bool playStartupAnimation(const std::string& animationPath, uint32_t timeoutMs);
    bool prepare(const std::string& videoPath,
                 uint32_t videoWidth,
                 uint32_t videoHeight,
                 const Protocol::CropRegion& cropRegion);
    bool play();
    bool pause();
    bool resume();
    void stop();

    bool isPrepared() const;
    bool isRunning();
    bool isCaptureRunning();
    bool isImageRunning();
    bool setCaptureAudioOutputMode(AudioOutputMode mode);
    AudioOutputMode getCaptureAudioOutputMode() const;
    bool setCaptureAudioVolumePercent(uint32_t volumePercent);
    uint32_t getCaptureAudioVolumePercent() const;
    std::string getLastError() const;

private:
    class Runtime;

    enum class State {
        Idle,
        Image,
        Capture,
        Prepared,
        Playing,
        Paused,
        Ended,
        Failed
    };

    std::string kmsSinkDescription(bool sync,
                                   bool showPrerollFrame,
                                   uint32_t planeId,
                                   bool restoreCrtc = true) const;
    std::string kmsTargetDescription(uint32_t planeId) const;
    std::string kmsVideoCaps() const;
    bool transitionUnlocked(int targetState, uint32_t timeoutMs, const char* operation);
    bool transitionPipelineUnlocked(void* pipeline,
                                    int targetState,
                                    uint32_t timeoutMs,
                                    const char* operation,
                                    bool background);
    bool pollBusUnlocked();
    bool pollPipelineBusUnlocked(void* pipeline, bool background);
    bool startCaptureAudioUnlocked(const std::string& captureDevice,
                                   const std::vector<AudioPlaybackOutput>& playbackOutputs);
    void pollCaptureAudioBusUnlocked();
    void retryCaptureAudioUnlocked();
    void scheduleCaptureAudioRetryUnlocked();
    void releaseCaptureAudioPipelineUnlocked();
    bool startPreviewThreadUnlocked();
    void stopPreviewThreadUnlocked();
    bool startNetworkInputThreadUnlocked();
    void stopNetworkInputThreadUnlocked();
    Protocol::SetWindowLayoutCommand prepareWindowRenderCommandUnlocked(
        const Protocol::SetWindowLayoutCommand& command) const;
    bool showWindowLayoutHardwareUnlocked(
        const Protocol::SetWindowLayoutCommand& command);
    std::string kmsWindowSinkDescription(const Protocol::WindowLayer& layer,
                                         uint32_t planeId,
                                         size_t layerIndex) const;
    bool startSignalOutputUnlocked();
    void releaseSignalOutputUnlocked();
    void previewLoop();
    void networkInputLoop();
    void networkWindowInputLoop(void* appSrc, std::string endpoint);
    static size_t networkWriteCallback(char* data, size_t size, size_t count, void* userData);
    static int networkProgressCallback(void* userData,
                                       int64_t downloadTotal,
                                       int64_t downloadNow,
                                       int64_t uploadTotal,
                                       int64_t uploadNow);
    bool consumeNetworkBytes(const uint8_t* data,
                             size_t size,
                             std::vector<uint8_t>& pending,
                             void* appSrc);
    bool showSignalUnlocked(const SignalSourceConfig& source, bool outputToDisplay);
    void releasePipelineUnlocked(bool keepStartupFrame = false);
    void releaseStartupPipelineUnlocked();
    void releaseBackgroundPipelineUnlocked();
    void setErrorUnlocked(const std::string& error);

    mutable std::mutex m_mutex;
    uint32_t m_nodeId;
    MediaPlayerOptions m_options;
    std::unique_ptr<Runtime> m_runtime;
    void* m_pipeline;
    void* m_startupPipeline;
    void* m_backgroundPipeline;
    void* m_captureAudioPipeline;
    uint32_t m_captureAudioVolumePercent;
    std::chrono::steady_clock::time_point m_nextCaptureAudioRetry;
    std::string m_lastError;
    State m_state;
    bool m_initialized;
    bool m_rtpEncodeDisabled;
    void* m_previewSink;
    std::atomic<bool> m_previewRunning;
    std::thread m_previewThread;
    void* m_networkAppSrc;
    std::atomic<bool> m_networkRunning;
    std::thread m_networkThread;
    struct NetworkWorker {
        void* appSrc = nullptr;
        std::thread thread;
    };
    std::vector<NetworkWorker> m_networkWorkers;
    mutable std::mutex m_previewMutex;
    std::shared_ptr<const std::vector<uint8_t>> m_latestPreviewFrame;
    uint32_t m_signalFrameWidth;
    uint32_t m_signalFrameHeight;
    std::atomic<bool> m_signalOutputActive;
    SignalSourceConfig m_signalSource;
};

} // namespace dms
