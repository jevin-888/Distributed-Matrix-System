#include "kvm/KvmAgent.h"
#include "common/Logger.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace dms {
namespace {

constexpr uint32_t KVM_WIRE_MAGIC = 0x4B564D31;
constexpr uint16_t KVM_WIRE_VERSION = 1;
constexpr size_t KVM_HEADER_SIZE = 28;
constexpr uint32_t MAX_KVM_PAYLOAD = 8;
constexpr uint16_t KVM_PREVIEW_PORT_OFFSET = 1;

enum class KvmFrameType : uint16_t {
    HELLO = 1,
    KEYBOARD = 2,
    MOUSE = 3,
    ACK = 4,
    PING = 5,
};

struct KvmFrame {
    KvmFrameType type = KvmFrameType::HELLO;
    uint32_t sessionId = 0;
    uint32_t sequence = 0;
    uint64_t token = 0;
    std::vector<uint8_t> payload;
};

void appendU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value >> 24));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

void appendU64(std::vector<uint8_t>& output, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

uint16_t readU16(const uint8_t* input) {
    return static_cast<uint16_t>((static_cast<uint16_t>(input[0]) << 8) | input[1]);
}

uint32_t readU32(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

uint64_t readU64(const uint8_t* input) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value = (value << 8) | input[index];
    }
    return value;
}

bool writeAll(int socketFd, const uint8_t* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(socketFd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool sendHttp(int socketFd, const std::string& response) {
    size_t offset = 0;
    while (offset < response.size()) {
        const ssize_t sent = send(socketFd, response.data() + offset,
                                  response.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool readAll(int socketFd, uint8_t* data, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(socketFd, data + received, length - received, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        if (result <= 0) return false;
        received += static_cast<size_t>(result);
    }
    return true;
}

bool sendFrame(int socketFd, const KvmFrame& frame) {
    if (frame.sessionId == 0 || frame.token == 0 || frame.payload.size() > MAX_KVM_PAYLOAD) {
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(KVM_HEADER_SIZE + frame.payload.size());
    appendU32(bytes, KVM_WIRE_MAGIC);
    appendU16(bytes, KVM_WIRE_VERSION);
    appendU16(bytes, static_cast<uint16_t>(frame.type));
    appendU32(bytes, frame.sessionId);
    appendU32(bytes, frame.sequence);
    appendU64(bytes, frame.token);
    appendU32(bytes, static_cast<uint32_t>(frame.payload.size()));
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return writeAll(socketFd, bytes.data(), bytes.size());
}

bool receiveFrame(int socketFd, KvmFrame& frame) {
    std::array<uint8_t, KVM_HEADER_SIZE> header{};
    if (!readAll(socketFd, header.data(), header.size()) ||
        readU32(header.data()) != KVM_WIRE_MAGIC ||
        readU16(header.data() + 4) != KVM_WIRE_VERSION) {
        return false;
    }
    const uint16_t rawType = readU16(header.data() + 6);
    const uint32_t payloadLength = readU32(header.data() + 24);
    if (rawType < static_cast<uint16_t>(KvmFrameType::HELLO) ||
        rawType > static_cast<uint16_t>(KvmFrameType::PING) ||
        payloadLength > MAX_KVM_PAYLOAD) {
        return false;
    }
    frame = {};
    frame.type = static_cast<KvmFrameType>(rawType);
    frame.sessionId = readU32(header.data() + 8);
    frame.sequence = readU32(header.data() + 12);
    frame.token = readU64(header.data() + 16);
    frame.payload.resize(payloadLength);
    return payloadLength == 0 || readAll(socketFd, frame.payload.data(), frame.payload.size());
}

bool writeReport(int deviceFd, const std::vector<uint8_t>& report) {
    size_t written = 0;
    while (written < report.size()) {
        const ssize_t result = write(deviceFd, report.data() + written, report.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        written += static_cast<size_t>(result);
    }
    return true;
}

uint8_t modifierMask(uint16_t keyCode) {
    switch (keyCode) {
        case KEY_LEFTCTRL: return 0x01;
        case KEY_LEFTSHIFT: return 0x02;
        case KEY_LEFTALT: return 0x04;
        case KEY_LEFTMETA: return 0x08;
        case KEY_RIGHTCTRL: return 0x10;
        case KEY_RIGHTSHIFT: return 0x20;
        case KEY_RIGHTALT: return 0x40;
        case KEY_RIGHTMETA: return 0x80;
        default: return 0;
    }
}

uint8_t hidUsage(uint16_t keyCode) {
    switch (keyCode) {
        case KEY_A: return 4; case KEY_B: return 5; case KEY_C: return 6;
        case KEY_D: return 7; case KEY_E: return 8; case KEY_F: return 9;
        case KEY_G: return 10; case KEY_H: return 11; case KEY_I: return 12;
        case KEY_J: return 13; case KEY_K: return 14; case KEY_L: return 15;
        case KEY_M: return 16; case KEY_N: return 17; case KEY_O: return 18;
        case KEY_P: return 19; case KEY_Q: return 20; case KEY_R: return 21;
        case KEY_S: return 22; case KEY_T: return 23; case KEY_U: return 24;
        case KEY_V: return 25; case KEY_W: return 26; case KEY_X: return 27;
        case KEY_Y: return 28; case KEY_Z: return 29;
        case KEY_1: return 30; case KEY_2: return 31; case KEY_3: return 32;
        case KEY_4: return 33; case KEY_5: return 34; case KEY_6: return 35;
        case KEY_7: return 36; case KEY_8: return 37; case KEY_9: return 38;
        case KEY_0: return 39; case KEY_ENTER: return 40; case KEY_ESC: return 41;
        case KEY_BACKSPACE: return 42; case KEY_TAB: return 43; case KEY_SPACE: return 44;
        case KEY_MINUS: return 45; case KEY_EQUAL: return 46; case KEY_LEFTBRACE: return 47;
        case KEY_RIGHTBRACE: return 48; case KEY_BACKSLASH: return 49;
        case KEY_SEMICOLON: return 51; case KEY_APOSTROPHE: return 52;
        case KEY_GRAVE: return 53; case KEY_COMMA: return 54; case KEY_DOT: return 55;
        case KEY_SLASH: return 56; case KEY_CAPSLOCK: return 57;
        case KEY_F1: return 58; case KEY_F2: return 59; case KEY_F3: return 60;
        case KEY_F4: return 61; case KEY_F5: return 62; case KEY_F6: return 63;
        case KEY_F7: return 64; case KEY_F8: return 65; case KEY_F9: return 66;
        case KEY_F10: return 67; case KEY_F11: return 68; case KEY_F12: return 69;
        case KEY_INSERT: return 73; case KEY_HOME: return 74; case KEY_PAGEUP: return 75;
        case KEY_DELETE: return 76; case KEY_END: return 77; case KEY_PAGEDOWN: return 78;
        case KEY_RIGHT: return 79; case KEY_LEFT: return 80; case KEY_DOWN: return 81;
        case KEY_UP: return 82; default: return 0;
    }
}

void updateKeyboardReport(std::array<uint8_t, 8>& report, uint16_t keyCode, bool pressed) {
    const uint8_t modifier = modifierMask(keyCode);
    if (modifier != 0) {
        if (pressed) report[0] |= modifier;
        else report[0] &= static_cast<uint8_t>(~modifier);
        return;
    }
    const uint8_t usage = hidUsage(keyCode);
    if (usage == 0) return;
    auto begin = report.begin() + 2;
    auto end = report.end();
    const auto position = std::find(begin, end, usage);
    if (pressed && position == end) {
        const auto empty = std::find(begin, end, 0);
        if (empty != end) *empty = usage;
    } else if (!pressed && position != end) {
        *position = 0;
    }
}

int8_t clampMotion(int value) {
    return static_cast<int8_t>(std::max(-127, std::min(127, value)));
}

} // namespace

KvmAgent::KvmAgent()
    : m_nodeId(0), m_initialized(false), m_running(false), m_sessionGeneration(0),
      m_serverSocket(-1), m_previewServerSocket(-1), m_controllerSocket(-1),
      m_targetClientSocket(-1),
      m_state("disabled") {}

KvmAgent::~KvmAgent() { shutdown(); }

bool KvmAgent::initialize(uint32_t nodeId, const KvmOptions& options) {
    if (m_initialized.load() || nodeId == 0 || options.listenPort == 0 ||
        (options.enabled && options.role == KvmRole::DISABLED)) {
        return false;
    }
    m_nodeId.store(nodeId);
    m_options = options;
    setState(options.enabled ? "idle" : "disabled");
    m_initialized.store(true);
    return true;
}

bool KvmAgent::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return false;
    if (!m_options.enabled) return true;
    if (!kvmCanTarget(m_options.role)) return true;

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        setState("error", "Cannot create KVM listener socket");
        m_running.store(false);
        return false;
    }
    int reuse = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_options.listenPort);
    if (bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socketFd, 4) != 0) {
        close(socketFd);
        setState("error", "Cannot bind KVM listener port");
        m_running.store(false);
        return false;
    }
    m_serverSocket.store(socketFd);
    m_listenerThread = std::thread(&KvmAgent::listenerLoop, this);
    if (m_options.listenPort >= 65535) {
        LOG_WARNING("Live preview disabled because KVM listen port has no adjacent port");
        return true;
    }
    const int previewSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (previewSocket < 0) {
        LOG_WARNING("Live preview socket could not be created");
        return true;
    }
    setsockopt(previewSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    address.sin_port = htons(static_cast<uint16_t>(m_options.listenPort + KVM_PREVIEW_PORT_OFFSET));
    if (bind(previewSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(previewSocket, 2) != 0) {
        close(previewSocket);
        LOG_WARNING("Live preview port %u is unavailable",
                    static_cast<unsigned>(m_options.listenPort + KVM_PREVIEW_PORT_OFFSET));
        return true;
    }
    m_previewServerSocket.store(previewSocket);
    m_previewListenerThread = std::thread(&KvmAgent::previewListenerLoop, this);
    return true;
}

bool KvmAgent::setNodeId(uint32_t nodeId) {
    if (nodeId == 0) return false;
    m_nodeId.store(nodeId);
    return true;
}

void KvmAgent::shutdown() {
    m_running.store(false);
    stopSessionWorkers();
    const int serverSocket = m_serverSocket.exchange(-1);
    if (serverSocket >= 0) {
        ::shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
    }
    if (m_listenerThread.joinable()) m_listenerThread.join();
    const int previewSocket = m_previewServerSocket.exchange(-1);
    if (previewSocket >= 0) {
        ::shutdown(previewSocket, SHUT_RDWR);
        close(previewSocket);
    }
    if (m_previewListenerThread.joinable()) m_previewListenerThread.join();
    {
        std::lock_guard<std::mutex> lock(m_previewClientsMutex);
        for (const int clientSocket : m_previewClientSockets) {
            ::shutdown(clientSocket, SHUT_RDWR);
        }
    }
    for (std::thread& worker : m_previewThreads) {
        if (worker.joinable()) worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_previewClientsMutex);
        m_previewClientSockets.clear();
        m_previewThreads.clear();
    }
    m_initialized.store(false);
}

void KvmAgent::setPreviewFrameProvider(PreviewFrameProvider provider) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_previewFrameProvider = std::move(provider);
}

void KvmAgent::stopSessionWorkers() {
    m_sessionGeneration.fetch_add(1);
    const int controllerSocket = m_controllerSocket.load();
    if (controllerSocket >= 0) ::shutdown(controllerSocket, SHUT_RDWR);
    const int targetSocket = m_targetClientSocket.load();
    if (targetSocket >= 0) ::shutdown(targetSocket, SHUT_RDWR);
    if (m_controllerThread.joinable() &&
        m_controllerThread.get_id() != std::this_thread::get_id()) {
        m_controllerThread.join();
    }
}

void KvmAgent::applyRoute(const Protocol::KvmRouteCommand& command) {
    if (!m_options.enabled ||
        command.expiresAt <= Protocol::getCurrentTimestamp() ||
        (command.controllerNodeId != m_nodeId.load() &&
         command.targetNodeId != m_nodeId.load())) return;

    stopSessionWorkers();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_route = command;
        m_lastError.clear();
        m_state = command.controllerNodeId == m_nodeId.load() ? "connecting" : "waiting";
    }
    const uint64_t generation = m_sessionGeneration.load();
    if (command.controllerNodeId == m_nodeId.load()) {
        if (!kvmCanControl(m_options.role)) {
            setState("error", "Node is not configured as a KVM controller");
            return;
        }
        m_controllerThread = std::thread(&KvmAgent::controllerLoop, this, generation);
    } else if (!kvmCanTarget(m_options.role)) {
        setState("error", "Node is not configured as a KVM target");
    }
}

void KvmAgent::releaseSession(uint32_t sessionId) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_route.sessionId != sessionId) return;
    }
    stopSessionWorkers();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_route = {};
        m_state = m_options.enabled ? "idle" : "disabled";
        m_lastError.clear();
    }
}

KvmAgentStatus KvmAgent::getStatus() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    KvmAgentStatus status;
    status.enabled = m_options.enabled;
    status.role = m_options.role;
    status.port = m_options.enabled ? m_options.listenPort : 0;
    status.sessionId = m_route.sessionId;
    status.state = m_state;
    status.lastError = m_lastError;
    return status;
}

void KvmAgent::setState(const std::string& state, const std::string& error) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = state;
    m_lastError = error;
    if (!error.empty()) LOG_ERROR("KVM agent: %s", error.c_str());
}

void KvmAgent::listenerLoop() {
    while (m_running.load()) {
        pollfd descriptor{m_serverSocket.load(), POLLIN, 0};
        const int pollResult = poll(&descriptor, 1, 200);
        if (pollResult <= 0 || !(descriptor.revents & POLLIN)) continue;
        const int clientSocket = accept(descriptor.fd, nullptr, nullptr);
        if (clientSocket < 0) continue;
        // receiveFrame() reads the fixed handshake header synchronously. A
        // client that connects and then stops sending must not monopolize
        // the only listener socket and block later KVM sessions.
        timeval timeout{3, 0};
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        m_targetClientSocket.store(clientSocket);
        handleTargetConnection(clientSocket);
        m_targetClientSocket.store(-1);
        close(clientSocket);
    }
}

void KvmAgent::previewListenerLoop() {
    while (m_running.load()) {
        pollfd descriptor{m_previewServerSocket.load(), POLLIN, 0};
        const int pollResult = poll(&descriptor, 1, 200);
        if (pollResult <= 0 || !(descriptor.revents & POLLIN)) continue;
        const int clientSocket = accept(descriptor.fd, nullptr, nullptr);
        if (clientSocket < 0) continue;
        {
            std::lock_guard<std::mutex> lock(m_previewClientsMutex);
            m_previewClientSockets.push_back(clientSocket);
            try {
                m_previewThreads.emplace_back(&KvmAgent::previewClientLoop, this,
                                              clientSocket);
            } catch (const std::exception& error) {
                m_previewClientSockets.pop_back();
                close(clientSocket);
                LOG_WARNING("Cannot start preview client worker: %s", error.what());
            }
        }
    }
}

void KvmAgent::previewClientLoop(int clientSocket) {
    timeval timeout{2, 0};
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    handlePreviewConnection(clientSocket);
    ::shutdown(clientSocket, SHUT_RDWR);
    close(clientSocket);
    std::lock_guard<std::mutex> lock(m_previewClientsMutex);
    const auto iterator = std::find(m_previewClientSockets.begin(),
                                    m_previewClientSockets.end(), clientSocket);
    if (iterator != m_previewClientSockets.end()) {
        m_previewClientSockets.erase(iterator);
    }
}

bool KvmAgent::handlePreviewConnection(int clientSocket) {
    std::array<char, 1024> request{};
    const ssize_t received = recv(clientSocket, request.data(), request.size() - 1, 0);
    if (received <= 0) return false;
    const std::string requestText(request.data(), static_cast<size_t>(received));
    const size_t lineEnd = requestText.find("\r\n");
    const std::string requestLine = requestText.substr(0, lineEnd);
    if (requestLine.rfind("GET /kvm/preview.jpg?", 0) != 0 ||
        requestLine.size() < 20 || requestLine.back() == ' ') {
        return sendHttp(clientSocket,
                        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
    }
    const size_t queryStart = requestLine.find('?');
    const size_t queryEnd = requestLine.find(' ', queryStart);
    if (queryStart == std::string::npos || queryEnd == std::string::npos) {
        return sendHttp(clientSocket,
                        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
    }
    uint32_t sessionId = 0;
    uint64_t token = 0;
    bool hasSessionId = false;
    bool hasToken = false;
    bool invalidQuery = false;
    bool hasCacheBuster = false;
    bool hasStream = false;
    bool stream = false;
    size_t fieldStart = queryStart + 1;
    while (fieldStart < queryEnd) {
        const size_t fieldEnd = requestLine.find('&', fieldStart);
        const size_t end = fieldEnd == std::string::npos || fieldEnd > queryEnd
            ? queryEnd : fieldEnd;
        const size_t equals = requestLine.find('=', fieldStart);
        if (equals == std::string::npos || equals >= end) {
            invalidQuery = true;
            break;
        }
        const std::string name = requestLine.substr(fieldStart, equals - fieldStart);
        const std::string value = requestLine.substr(equals + 1, end - equals - 1);
        try {
            if (name == "sessionId") {
                if (hasSessionId || value.empty()) {
                    invalidQuery = true;
                    break;
                }
                size_t parsedLength = 0;
                const unsigned long long parsed = std::stoull(value, &parsedLength);
                if (parsedLength != value.size() || parsed == 0 ||
                    parsed > std::numeric_limits<uint32_t>::max()) {
                    invalidQuery = true;
                    break;
                }
                sessionId = static_cast<uint32_t>(parsed);
                hasSessionId = true;
            } else if (name == "token") {
                if (hasToken || value.empty()) {
                    invalidQuery = true;
                    break;
                }
                size_t parsedLength = 0;
                token = std::stoull(value, &parsedLength);
                if (parsedLength != value.size() || token == 0) {
                    invalidQuery = true;
                    break;
                }
                hasToken = true;
            } else if (name == "v") {
                if (hasCacheBuster || value.empty()) {
                    invalidQuery = true;
                    break;
                }
                hasCacheBuster = true;
            } else if (name == "stream") {
                if (hasStream || (value != "0" && value != "1")) {
                    invalidQuery = true;
                    break;
                }
                hasStream = true;
                stream = value == "1";
            } else {
                invalidQuery = true;
                break;
            }
        } catch (...) {
            invalidQuery = true;
            break;
        }
        if (fieldEnd == std::string::npos || fieldEnd >= queryEnd) break;
        fieldStart = fieldEnd + 1;
    }
    if (invalidQuery || hasSessionId != hasToken) {
        return sendHttp(clientSocket,
                        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    }
    if (hasSessionId) {
        Protocol::KvmRouteCommand route;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            route = m_route;
        }
        if (sessionId != route.sessionId || token != route.sessionToken ||
            route.targetNodeId != m_nodeId.load() ||
            route.expiresAt <= Protocol::getCurrentTimestamp()) {
            return sendHttp(clientSocket,
                            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
        }
    }
    PreviewFrameProvider provider;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        provider = m_previewFrameProvider;
    }
    const auto frame = provider ? provider() : nullptr;
    if ((!frame || frame->empty()) && !stream) {
        return sendHttp(clientSocket,
                        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    }
    if (stream) {
        if (!sendHttp(clientSocket,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=dmsframe\r\n"
                      "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                      "Connection: close\r\n\r\n")) {
            return false;
        }
        std::shared_ptr<const std::vector<uint8_t>> lastFrame;
        while (m_running.load()) {
            const auto currentFrame = provider ? provider() : nullptr;
            if (!currentFrame || currentFrame->empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                continue;
            }
            if (lastFrame && currentFrame.get() == lastFrame.get()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            std::ostringstream partHeader;
            partHeader << "--dmsframe\r\nContent-Type: image/jpeg\r\nContent-Length: "
                       << currentFrame->size() << "\r\n\r\n";
            if (!sendHttp(clientSocket, partHeader.str()) ||
                !writeAll(clientSocket, currentFrame->data(), currentFrame->size()) ||
                !sendHttp(clientSocket, "\r\n")) {
                return false;
            }
            lastFrame = currentFrame;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "
            << frame->size()
            << "\r\nCache-Control: no-store, no-cache, must-revalidate\r\n"
               "Connection: close\r\n\r\n";
    if (!sendHttp(clientSocket, header.str())) return false;
    return writeAll(clientSocket, frame->data(), frame->size());
}

bool KvmAgent::handleTargetConnection(int clientSocket) {
    KvmFrame hello;
    if (!receiveFrame(clientSocket, hello) || hello.type != KvmFrameType::HELLO) return false;
    Protocol::KvmRouteCommand route;
    const auto routeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            route = m_route;
        }
        if (hello.sessionId == route.sessionId && hello.token == route.sessionToken &&
            hello.sequence == route.controllerNodeId &&
            route.targetNodeId == m_nodeId.load() &&
            route.expiresAt > Protocol::getCurrentTimestamp()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (m_running.load() && std::chrono::steady_clock::now() < routeDeadline);

    if (hello.sessionId != route.sessionId || hello.token != route.sessionToken ||
        hello.sequence != route.controllerNodeId ||
        route.targetNodeId != m_nodeId.load() ||
        route.expiresAt <= Protocol::getCurrentTimestamp()) {
        return false;
    }

    // A disconnected USB gadget can block an O_WRONLY open forever. Probe it
    // non-blocking so one failed session cannot occupy the KVM listener.
    const int keyboardFd = open(m_options.keyboardHidDevice.c_str(),
                                O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    const int mouseFd = open(m_options.mouseHidDevice.c_str(),
                             O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (keyboardFd < 0 || mouseFd < 0) {
        if (keyboardFd >= 0) close(keyboardFd);
        if (mouseFd >= 0) close(mouseFd);
        setState("error", "USB HID gadget devices are unavailable");
        return false;
    }
    timeval activeTimeout{3, 0};
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &activeTimeout, sizeof(activeTimeout));
    int keepAlive = 1;
    setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
    if (!sendFrame(clientSocket, {KvmFrameType::ACK, route.sessionId, 0,
                                  route.sessionToken, {}})) {
        close(keyboardFd);
        close(mouseFd);
        setState("error", "Cannot acknowledge KVM session handshake");
        return false;
    }
    setState("active");
    bool success = true;
    while (m_running.load() && route.expiresAt > Protocol::getCurrentTimestamp()) {
        KvmFrame frame;
        if (!receiveFrame(clientSocket, frame)) break;
        if (frame.sessionId != route.sessionId || frame.token != route.sessionToken) {
            success = false;
            break;
        }
        if (frame.type == KvmFrameType::KEYBOARD && frame.payload.size() == 8) {
            success = writeReport(keyboardFd, frame.payload);
        } else if (frame.type == KvmFrameType::MOUSE && frame.payload.size() == 4) {
            success = writeReport(mouseFd, frame.payload);
        } else if (frame.type == KvmFrameType::PING && frame.payload.empty()) {
            success = sendFrame(clientSocket, {KvmFrameType::ACK, route.sessionId,
                                                frame.sequence, route.sessionToken, {}});
        } else {
            success = false;
        }
        if (!success) break;
    }
    writeReport(keyboardFd, std::vector<uint8_t>(8, 0));
    writeReport(mouseFd, std::vector<uint8_t>(4, 0));
    close(keyboardFd);
    close(mouseFd);
    if (m_running.load() && getStatus().sessionId == route.sessionId) {
        if (success) setState("waiting");
        else setState("error", "KVM input channel closed after an invalid or failed frame");
    }
    return success;
}

void KvmAgent::controllerLoop(uint64_t generation) {
    Protocol::KvmRouteCommand route;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        route = m_route;
    }

    std::vector<int> inputFds;
    for (const auto& device : m_options.inputDevices) {
        const int fd = open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) inputFds.push_back(fd);
    }
    if (inputFds.empty()) {
        setState("error", "No configured KVM input device is available");
        return;
    }

    int socketFd = -1;
    while (m_running.load() && generation == m_sessionGeneration.load() &&
           Protocol::getCurrentTimestamp() < route.expiresAt) {
        socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (socketFd >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(route.targetPort);
            if (inet_pton(AF_INET, route.targetIp.c_str(), &address.sin_addr) == 1 &&
                connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                break;
            }
            close(socketFd);
            socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (socketFd < 0) {
        for (const int fd : inputFds) close(fd);
        if (generation == m_sessionGeneration.load()) setState("error", "Cannot connect to KVM target");
        return;
    }
    m_controllerSocket.store(socketFd);
    timeval handshakeTimeout{4, 0};
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &handshakeTimeout, sizeof(handshakeTimeout));
    KvmFrame handshakeAck;
    if (!sendFrame(socketFd, {KvmFrameType::HELLO, route.sessionId, m_nodeId.load(),
                              route.sessionToken, {}}) ||
        !receiveFrame(socketFd, handshakeAck) || handshakeAck.type != KvmFrameType::ACK ||
        handshakeAck.sessionId != route.sessionId || handshakeAck.sequence != 0 ||
        handshakeAck.token != route.sessionToken || !handshakeAck.payload.empty()) {
        setState("error", "KVM target handshake failed");
    } else {
        timeval blockingTimeout{};
        setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &blockingTimeout, sizeof(blockingTimeout));
        setState("active");
        std::array<uint8_t, 8> keyboard{};
        uint8_t mouseButtons = 0;
        int mouseX = 0, mouseY = 0, mouseWheel = 0;
        bool keyboardDirty = false, mouseDirty = false;
        uint32_t sequence = 1;
        std::vector<pollfd> descriptors;
        for (const int fd : inputFds) descriptors.push_back({fd, POLLIN, 0});

        while (m_running.load() && generation == m_sessionGeneration.load() &&
               Protocol::getCurrentTimestamp() < route.expiresAt) {
            const int result = poll(descriptors.data(), descriptors.size(), 100);
            if (result < 0 && errno != EINTR) break;
            for (auto& descriptor : descriptors) {
                if (!(descriptor.revents & POLLIN)) continue;
                input_event events[32];
                const ssize_t bytes = read(descriptor.fd, events, sizeof(events));
                if (bytes <= 0) continue;
                const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
                for (size_t index = 0; index < count; ++index) {
                    const input_event& event = events[index];
                    if (event.type == EV_KEY && event.code < BTN_MOUSE) {
                        updateKeyboardReport(keyboard, event.code, event.value != 0);
                        keyboardDirty = true;
                    } else if (event.type == EV_KEY && event.code >= BTN_MOUSE) {
                        const uint8_t mask = event.code == BTN_LEFT ? 1 : event.code == BTN_RIGHT ? 2 :
                                             event.code == BTN_MIDDLE ? 4 : 0;
                        if (event.value) mouseButtons |= mask;
                        else mouseButtons &= static_cast<uint8_t>(~mask);
                        mouseDirty = mask != 0 || mouseDirty;
                    } else if (event.type == EV_REL) {
                        if (event.code == REL_X) mouseX += event.value;
                        else if (event.code == REL_Y) mouseY += event.value;
                        else if (event.code == REL_WHEEL) mouseWheel += event.value;
                        mouseDirty = true;
                    }
                    if (event.type != EV_SYN || event.code != SYN_REPORT) continue;
                    if (keyboardDirty) {
                        const std::vector<uint8_t> report(keyboard.begin(), keyboard.end());
                        if (!sendFrame(socketFd, {KvmFrameType::KEYBOARD, route.sessionId,
                                                  sequence++, route.sessionToken, report})) goto finished;
                        keyboardDirty = false;
                    }
                    if (mouseDirty) {
                        const std::vector<uint8_t> report{mouseButtons,
                            static_cast<uint8_t>(clampMotion(mouseX)),
                            static_cast<uint8_t>(clampMotion(mouseY)),
                            static_cast<uint8_t>(clampMotion(mouseWheel))};
                        if (!sendFrame(socketFd, {KvmFrameType::MOUSE, route.sessionId,
                                                  sequence++, route.sessionToken, report})) goto finished;
                        mouseX = mouseY = mouseWheel = 0;
                        mouseDirty = false;
                    }
                }
            }
        }
    }

finished:
    m_controllerSocket.store(-1);
    ::shutdown(socketFd, SHUT_RDWR);
    close(socketFd);
    for (const int fd : inputFds) close(fd);
    if (m_running.load() && generation == m_sessionGeneration.load()) setState("idle");
}

} // namespace dms
