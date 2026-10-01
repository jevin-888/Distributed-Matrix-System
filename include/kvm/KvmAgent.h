#pragma once

#include "common/Protocol.h"
#include "kvm/KvmTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dms {

class KvmAgent {
public:
    using PreviewFrameProvider =
        std::function<std::shared_ptr<const std::vector<uint8_t>>()>;

    KvmAgent();
    ~KvmAgent();

    KvmAgent(const KvmAgent&) = delete;
    KvmAgent& operator=(const KvmAgent&) = delete;

    bool initialize(uint32_t nodeId, const KvmOptions& options);
    bool start();
    void shutdown();
    bool setNodeId(uint32_t nodeId);
    void setPreviewFrameProvider(PreviewFrameProvider provider);
    void applyRoute(const Protocol::KvmRouteCommand& command);
    void releaseSession(uint32_t sessionId);
    KvmAgentStatus getStatus() const;

private:
    void listenerLoop();
    void previewListenerLoop();
    void previewClientLoop(int clientSocket);
    void controllerLoop(uint64_t generation);
    bool handleTargetConnection(int clientSocket);
    bool handlePreviewConnection(int clientSocket);
    void stopSessionWorkers();
    void setState(const std::string& state, const std::string& error = {});

    std::atomic<uint32_t> m_nodeId;
    KvmOptions m_options;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_running;
    std::atomic<uint64_t> m_sessionGeneration;
    std::atomic<int> m_serverSocket;
    std::atomic<int> m_previewServerSocket;
    std::atomic<int> m_controllerSocket;
    std::atomic<int> m_targetClientSocket;
    std::thread m_listenerThread;
    std::thread m_previewListenerThread;
    std::thread m_controllerThread;
    std::mutex m_previewClientsMutex;
    std::vector<int> m_previewClientSockets;
    std::vector<std::thread> m_previewThreads;

    mutable std::mutex m_stateMutex;
    Protocol::KvmRouteCommand m_route;
    std::string m_state;
    std::string m_lastError;
    PreviewFrameProvider m_previewFrameProvider;
};

} // namespace dms
