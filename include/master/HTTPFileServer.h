#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <future>
#include <unordered_map>
#include <vector>

namespace dms {

class MasterNodeApp;
class NodeDiscovery;

class HTTPFileServer {
public:
    HTTPFileServer(const std::string& rootDir,
                   uint16_t port,
                   int maxConnections,
                   const std::string& webRoot,
                   const std::string& regionsFile,
                   uint64_t defaultSyncDelayMs);
    ~HTTPFileServer();

    HTTPFileServer(const HTTPFileServer&) = delete;
    HTTPFileServer& operator=(const HTTPFileServer&) = delete;

    bool start();
    void stop();

    bool isRunning() const { return m_running.load(); }
    uint16_t getPort() const { return m_port; }
    std::string getRootDir() const { return m_rootDir; }
    int getActiveConnections() const { return m_activeConnections.load(); }
    uint64_t getTotalRequests() const { return m_totalRequests.load(); }
    uint64_t getTotalBytesSent() const { return m_totalBytesSent.load(); }

    void setNodeDiscovery(NodeDiscovery* nodeDiscovery) { m_nodeDiscovery = nodeDiscovery; }
    void setMasterApp(MasterNodeApp* masterApp) { m_masterApp = masterApp; }

private:
    void serverThread();
    void handleClient(int clientSocket);
    bool readRequest(int clientSocket, std::string& request);
    bool parseRequest(const std::string& request,
                      std::string& method,
                      std::string& path,
                      std::unordered_map<std::string, std::string>& headers,
                      std::string& body);

    void sendResponse(int clientSocket,
                      int statusCode,
                      const std::string& statusText,
                      const std::unordered_map<std::string, std::string>& headers,
                      const std::string& body = "");
    void sendJson(int clientSocket, int statusCode, const std::string& body);
    void sendFile(int clientSocket,
                  const std::string& filePath,
                  bool sendBody,
                  int64_t rangeStart = -1,
                  int64_t rangeEnd = -1);
    void sendError(int clientSocket, int statusCode, const std::string& message);

    std::string getMimeType(const std::string& filename) const;
    static bool urlDecode(const std::string& encoded, std::string& decoded);
    bool resolveRequestPath(const std::string& requestPath, std::string& filePath) const;

    void handleApiRequest(int clientSocket,
                          const std::string& method,
                          const std::string& path,
                          const std::string& body);
    void handleGetNodes(int clientSocket);
    void handleSetNodeRole(int clientSocket, const std::string& body);
    void handleSetNodeNetwork(int clientSocket, const std::string& body);
    void handleSetNodeSource(int clientSocket, const std::string& body);
    void handleDiscoverNodes(int clientSocket, const std::string& body);
    void handleGetScreens(int clientSocket);
    void handleSetScreens(int clientSocket, const std::string& body);
    void handleGetWindows(int clientSocket);
    void handleSetWindows(int clientSocket, const std::string& body);
    void handleGetRegions(int clientSocket);
    void handleSetRegions(int clientSocket, const std::string& body);
    void handleGetStatus(int clientSocket);
    void handleGetAudioOutput(int clientSocket);
    void handleSetAudioOutput(int clientSocket, const std::string& body);
    void handleGetAudioVolume(int clientSocket);
    void handleSetAudioVolume(int clientSocket, const std::string& body);
    void handleGetKvmStatus(int clientSocket);
    void handleAcquireKvm(int clientSocket, const std::string& body);
    void handleReleaseKvm(int clientSocket, const std::string& body);
    void handlePlay(int clientSocket, const std::string& body);
    void handleSimpleControl(int clientSocket, const std::string& operation, const std::string& body);
    void handlePreload(int clientSocket, const std::string& body);

    std::string m_rootDir;
    std::string m_webRoot;
    std::string m_regionsFile;
    uint16_t m_port;
    uint64_t m_defaultSyncDelayMs;
    int m_maxConnections;

    std::atomic<bool> m_running;
    int m_serverSocket;
    std::thread m_serverThread;
    std::mutex m_clientTasksMutex;
    std::mutex m_regionsMutex;
    std::vector<std::future<void>> m_clientTasks;

    std::atomic<int> m_activeConnections;
    std::atomic<uint64_t> m_totalRequests;
    std::atomic<uint64_t> m_totalBytesSent;

    NodeDiscovery* m_nodeDiscovery;
    MasterNodeApp* m_masterApp;

    static const std::unordered_map<std::string, std::string> MIME_TYPES;
    static constexpr size_t MAX_REQUEST_SIZE = 1024 * 1024;
};

} // namespace dms
