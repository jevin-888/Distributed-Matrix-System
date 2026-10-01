#include "master/HTTPFileServer.h"
#include "common/Logger.h"
#include "master/MasterNodeApp.h"
#include "master/NodeDiscovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

namespace dms {
namespace {

std::string statusTextFor(int statusCode) {
    switch (statusCode) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool pathIsWithin(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    auto baseIt = base.begin();
    auto candidateIt = candidate.begin();
    for (; baseIt != base.end(); ++baseIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *baseIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

void requireExactObject(const json& value,
                        std::initializer_list<const char*> required,
                        std::initializer_list<const char*> optional,
                        const char* name) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be a JSON object");
    }
    std::set<std::string> allowed;
    for (const char* key : required) {
        allowed.emplace(key);
        if (!value.contains(key)) {
            throw std::invalid_argument(std::string(name) + " is missing field: " + key);
        }
    }
    for (const char* key : optional) {
        allowed.emplace(key);
    }
    for (const auto& item : value.items()) {
        if (allowed.count(item.key()) == 0) {
            throw std::invalid_argument(std::string(name) + " contains unknown field: " + item.key());
        }
    }
}

bool sendAll(int socketFd, const char* data, size_t length, uint64_t& bytesSent) {
    size_t offset = 0;
    while (offset < length) {
        const ssize_t result = send(socketFd, data + offset, length - offset, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        offset += static_cast<size_t>(result);
        bytesSent += static_cast<uint64_t>(result);
    }
    return true;
}

} // namespace

void validateRegionsDocument(const json& document);

const std::unordered_map<std::string, std::string> HTTPFileServer::MIME_TYPES = {
    {".mp4", "video/mp4"},
    {".mkv", "video/x-matroska"},
    {".avi", "video/x-msvideo"},
    {".mov", "video/quicktime"},
    {".webm", "video/webm"},
    {".m4v", "video/x-m4v"},
    {".ts", "video/mp2t"},
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".ico", "image/x-icon"}
};

HTTPFileServer::HTTPFileServer(const std::string& rootDir,
                               uint16_t port,
                               int maxConnections,
                               const std::string& webRoot,
                               const std::string& regionsFile,
                               uint64_t defaultSyncDelayMs)
    : m_rootDir(rootDir)
    , m_webRoot(webRoot)
    , m_regionsFile(regionsFile)
    , m_port(port)
    , m_defaultSyncDelayMs(defaultSyncDelayMs)
    , m_maxConnections(maxConnections)
    , m_running(false)
    , m_serverSocket(-1)
    , m_activeConnections(0)
    , m_totalRequests(0)
    , m_totalBytesSent(0)
    , m_nodeDiscovery(nullptr)
    , m_masterApp(nullptr) {
}

HTTPFileServer::~HTTPFileServer() {
    stop();
}

bool HTTPFileServer::start() {
    if (m_running.load() || m_port == 0 || m_maxConnections <= 0) {
        return false;
    }

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        LOG_ERROR("Failed to create HTTP socket: %s", std::strerror(errno));
        return false;
    }

    int reuseAddress = 1;
    if (setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR,
                   &reuseAddress, sizeof(reuseAddress)) != 0) {
        LOG_ERROR("Failed to configure HTTP socket: %s", std::strerror(errno));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);
    if (bind(m_serverSocket, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(m_serverSocket, m_maxConnections) < 0) {
        LOG_ERROR("Failed to bind/listen on HTTP port %u: %s", m_port, std::strerror(errno));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    m_running.store(true);
    try {
        m_serverThread = std::thread(&HTTPFileServer::serverThread, this);
    } catch (const std::exception& error) {
        m_running.store(false);
        close(m_serverSocket);
        m_serverSocket = -1;
        LOG_ERROR("Failed to start HTTP server thread: %s", error.what());
        return false;
    }
    LOG_INFO("HTTP server started: port=%u, mediaRoot=%s, webRoot=%s",
             m_port, m_rootDir.c_str(), m_webRoot.c_str());
    return true;
}

void HTTPFileServer::stop() {
    m_running.store(false);
    if (m_serverSocket >= 0) {
        shutdown(m_serverSocket, SHUT_RDWR);
        close(m_serverSocket);
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    m_serverSocket = -1;

    std::vector<std::future<void>> clientTasks;
    {
        std::lock_guard<std::mutex> lock(m_clientTasksMutex);
        clientTasks.swap(m_clientTasks);
    }
    for (auto& task : clientTasks) {
        if (!task.valid()) {
            continue;
        }
        try {
            task.get();
        } catch (const std::exception& error) {
            LOG_ERROR("HTTP client task failed: %s", error.what());
        } catch (...) {
            LOG_ERROR("HTTP client task failed with an unknown exception");
        }
    }
}

void HTTPFileServer::serverThread() {
    while (m_running.load()) {
        struct sockaddr_in clientAddress {};
        socklen_t addressLength = sizeof(clientAddress);
        const int clientSocket = accept(m_serverSocket,
                                        reinterpret_cast<struct sockaddr*>(&clientAddress),
                                        &addressLength);
        if (clientSocket < 0) {
            if (m_running.load() && errno != EINTR) {
                LOG_ERROR("HTTP accept failed: %s", std::strerror(errno));
            }
            continue;
        }

        struct timeval ioTimeout {};
        ioTimeout.tv_sec = 3;
        if (setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
                       &ioTimeout, sizeof(ioTimeout)) != 0 ||
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO,
                       &ioTimeout, sizeof(ioTimeout)) != 0) {
            LOG_WARNING("Failed to configure HTTP client socket: %s", std::strerror(errno));
            close(clientSocket);
            continue;
        }

        const int previousConnections = m_activeConnections.fetch_add(1);
        if (previousConnections >= m_maxConnections) {
            m_activeConnections.fetch_sub(1);
            sendError(clientSocket, 503, "Connection limit reached");
            close(clientSocket);
            continue;
        }
        std::lock_guard<std::mutex> lock(m_clientTasksMutex);
        for (auto task = m_clientTasks.begin(); task != m_clientTasks.end();) {
            if (task->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++task;
                continue;
            }
            try {
                task->get();
            } catch (const std::exception& error) {
                LOG_ERROR("HTTP client task failed: %s", error.what());
            } catch (...) {
                LOG_ERROR("HTTP client task failed with an unknown exception");
            }
            task = m_clientTasks.erase(task);
        }
        try {
            m_clientTasks.emplace_back(std::async(std::launch::async, [this, clientSocket]() {
                try {
                    handleClient(clientSocket);
                } catch (const std::exception& error) {
                    LOG_ERROR("Unhandled HTTP request error: %s", error.what());
                } catch (...) {
                    LOG_ERROR("Unhandled HTTP request error");
                }
                close(clientSocket);
                m_activeConnections.fetch_sub(1);
            }));
        } catch (const std::exception& error) {
            LOG_ERROR("Failed to launch HTTP client task: %s", error.what());
            close(clientSocket);
            m_activeConnections.fetch_sub(1);
        }
    }
}

bool HTTPFileServer::readRequest(int clientSocket, std::string& request) {
    request.clear();
    std::array<char, 8192> buffer{};
    size_t expectedSize = std::numeric_limits<size_t>::max();

    while (request.size() < MAX_REQUEST_SIZE) {
        const ssize_t bytesRead = recv(clientSocket, buffer.data(), buffer.size(), 0);
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytesRead == 0) {
            return expectedSize != std::numeric_limits<size_t>::max() && request.size() == expectedSize;
        }
        request.append(buffer.data(), static_cast<size_t>(bytesRead));

        const size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            continue;
        }
        if (expectedSize == std::numeric_limits<size_t>::max()) {
            expectedSize = headerEnd + 4;
            std::istringstream headersStream(request.substr(0, headerEnd));
            std::string line;
            std::getline(headersStream, line); // request line
            bool hasContentLength = false;
            while (std::getline(headersStream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos) {
                    return false;
                }
                std::string key = line.substr(0, colon);
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                std::string value = line.substr(colon + 1);
                const size_t first = value.find_first_not_of(" \t");
                const size_t last = value.find_last_not_of(" \t");
                value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
                if (key == "transfer-encoding") {
                    return false;
                }
                if (key == "content-length") {
                    if (hasContentLength || value.empty() ||
                        !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                        return false;
                    }
                    hasContentLength = true;
                    try {
                        const unsigned long long length = std::stoull(value);
                        if (length > MAX_REQUEST_SIZE - expectedSize) {
                            return false;
                        }
                        expectedSize += static_cast<size_t>(length);
                    } catch (...) {
                        return false;
                    }
                }
            }
        }
        if (request.size() >= expectedSize) {
            return request.size() == expectedSize;
        }
    }
    return false;
}

void HTTPFileServer::handleClient(int clientSocket) {
    std::string request;
    if (!readRequest(clientSocket, request)) {
        sendError(clientSocket, request.size() >= MAX_REQUEST_SIZE ? 413 : 400,
                  request.size() >= MAX_REQUEST_SIZE ? "Request too large" : "Invalid request");
        return;
    }
    m_totalRequests.fetch_add(1);

    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    if (!parseRequest(request, method, path, headers, body)) {
        sendError(clientSocket, 400, "Malformed HTTP request");
        return;
    }

    const size_t queryPosition = path.find('?');
    if (queryPosition != std::string::npos) {
        path.erase(queryPosition);
    }

    if (method == "OPTIONS") {
        sendResponse(clientSocket, 204, "No Content", {}, "");
        return;
    }
    if (path.rfind("/api/", 0) == 0) {
        if ((method == "POST" || method == "PUT")) {
            const auto contentType = headers.find("content-type");
            if (contentType == headers.end() ||
                contentType->second.rfind("application/json", 0) != 0) {
                sendError(clientSocket, 415, "Content-Type must be application/json");
                return;
            }
        }
        handleApiRequest(clientSocket, method, path, body);
        return;
    }
    if (method != "GET" && method != "HEAD") {
        sendError(clientSocket, 405, "Only GET and HEAD are supported for files");
        return;
    }

    std::string filePath;
    if (!resolveRequestPath(path, filePath)) {
        sendError(clientSocket, 404, "File not found");
        return;
    }

    struct stat fileStat {};
    if (stat(filePath.c_str(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        sendError(clientSocket, 404, "File not found");
        return;
    }

    int64_t rangeStart = -1;
    int64_t rangeEnd = -1;
    const auto range = headers.find("range");
    if (range != headers.end()) {
        const std::string prefix = "bytes=";
        if (fileStat.st_size <= 0 || range->second.rfind(prefix, 0) != 0 ||
            range->second.find(',') != std::string::npos) {
            sendError(clientSocket, 416, "Invalid or unsupported byte range");
            return;
        }
        const std::string value = range->second.substr(prefix.size());
        const size_t dash = value.find('-');
        auto parseRangeNumber = [](const std::string& text) -> uint64_t {
            if (text.empty() || !std::all_of(text.begin(), text.end(),
                                             [](unsigned char ch) { return std::isdigit(ch); })) {
                throw std::invalid_argument("invalid range number");
            }
            return std::stoull(text);
        };
        try {
            if (dash == std::string::npos || value.find('-', dash + 1) != std::string::npos) {
                throw std::invalid_argument("invalid range syntax");
            }
            const std::string startText = value.substr(0, dash);
            const std::string endText = value.substr(dash + 1);
            if (startText.empty()) {
                const uint64_t suffixLength = parseRangeNumber(endText);
                if (suffixLength == 0) {
                    throw std::invalid_argument("invalid suffix");
                }
                rangeStart = suffixLength >= static_cast<uint64_t>(fileStat.st_size)
                    ? 0 : fileStat.st_size - static_cast<int64_t>(suffixLength);
                rangeEnd = fileStat.st_size - 1;
            } else {
                const uint64_t rawStart = parseRangeNumber(startText);
                const uint64_t rawEnd = endText.empty()
                    ? static_cast<uint64_t>(fileStat.st_size - 1)
                    : parseRangeNumber(endText);
                if (rawStart > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
                    rawEnd > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                    throw std::out_of_range("range overflow");
                }
                rangeStart = static_cast<int64_t>(rawStart);
                rangeEnd = std::min<int64_t>(static_cast<int64_t>(rawEnd), fileStat.st_size - 1);
            }
        } catch (...) {
            sendError(clientSocket, 416, "Invalid byte range");
            return;
        }
        if (rangeStart < 0 || rangeEnd < rangeStart || rangeStart >= fileStat.st_size) {
            sendError(clientSocket, 416, "Byte range is outside the file");
            return;
        }
    }

    sendFile(clientSocket, filePath, method == "GET", rangeStart, rangeEnd);
}

bool HTTPFileServer::parseRequest(const std::string& request,
                                  std::string& method,
                                  std::string& path,
                                  std::unordered_map<std::string, std::string>& headers,
                                  std::string& body) {
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }

    std::istringstream stream(request.substr(0, headerEnd));
    std::string line;
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::string version;
    std::string extra;
    std::istringstream requestLine(line);
    if (!(requestLine >> method >> path >> version) || (requestLine >> extra) ||
        (version != "HTTP/1.0" && version != "HTTP/1.1") || path.empty() || path.front() != '/') {
        return false;
    }
    if (!std::all_of(method.begin(), method.end(), [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; })) {
        return false;
    }

    headers.clear();
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        std::string key = line.substr(0, colon);
        if (!std::all_of(key.begin(), key.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '-';
            })) {
            return false;
        }
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        std::string value = line.substr(colon + 1);
        const size_t first = value.find_first_not_of(" \t");
        const size_t last = value.find_last_not_of(" \t");
        value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
        if (!headers.emplace(key, value).second) {
            return false;
        }
    }
    if (headers.find("transfer-encoding") != headers.end()) {
        return false;
    }

    body = request.substr(headerEnd + 4);
    const auto contentLength = headers.find("content-length");
    if (contentLength == headers.end()) {
        return body.empty();
    }
    if (contentLength->second.empty() ||
        !std::all_of(contentLength->second.begin(), contentLength->second.end(),
                     [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }
    try {
        const unsigned long long rawLength = std::stoull(contentLength->second);
        if (rawLength > MAX_REQUEST_SIZE || body.size() != static_cast<size_t>(rawLength)) {
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

void HTTPFileServer::sendResponse(int clientSocket,
                                  int statusCode,
                                  const std::string& statusText,
                                  const std::unordered_map<std::string, std::string>& headers,
                                  const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    response << "Connection: close\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    for (const auto& header : headers) {
        response << header.first << ": " << header.second << "\r\n";
    }
    response << "\r\n";
    response << body;

    const std::string data = response.str();
    uint64_t bytesSent = 0;
    if (sendAll(clientSocket, data.data(), data.size(), bytesSent)) {
        m_totalBytesSent.fetch_add(bytesSent);
    }
}

void HTTPFileServer::sendJson(int clientSocket, int statusCode, const std::string& body) {
    sendResponse(clientSocket, statusCode, statusTextFor(statusCode),
                 {{"Content-Type", "application/json; charset=utf-8"},
                  {"Content-Length", std::to_string(body.size())}}, body);
}

void HTTPFileServer::sendError(int clientSocket, int statusCode, const std::string& message) {
    sendJson(clientSocket, statusCode, json{{"success", false}, {"error", message}}.dump());
}

void HTTPFileServer::sendFile(int clientSocket,
                              const std::string& filePath,
                              bool sendBody,
                              int64_t rangeStart,
                              int64_t rangeEnd) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file) {
        sendError(clientSocket, 500, "Cannot open file");
        return;
    }

    const int64_t fileSize = static_cast<int64_t>(file.tellg());
    const bool partial = rangeStart >= 0;
    const int64_t start = partial ? rangeStart : 0;
    const int64_t end = partial ? rangeEnd : fileSize - 1;
    const int64_t contentLength = fileSize == 0 ? 0 : end - start + 1;

    std::unordered_map<std::string, std::string> headers = {
        {"Content-Type", getMimeType(filePath)},
        {"Content-Length", std::to_string(contentLength)},
        {"Accept-Ranges", "bytes"}
    };
    if (partial) {
        headers["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                   std::to_string(end) + "/" + std::to_string(fileSize);
    }

    std::ostringstream response;
    response << "HTTP/1.1 " << (partial ? 206 : 200) << " "
             << (partial ? "Partial Content" : "OK") << "\r\n";
    response << "Connection: close\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    for (const auto& header : headers) {
        response << header.first << ": " << header.second << "\r\n";
    }
    response << "\r\n";

    uint64_t bytesSent = 0;
    const std::string headerText = response.str();
    if (!sendAll(clientSocket, headerText.data(), headerText.size(), bytesSent)) {
        return;
    }

    if (sendBody && contentLength > 0) {
        file.seekg(start);
        std::vector<char> buffer(64 * 1024);
        int64_t remaining = contentLength;
        while (remaining > 0 && file) {
            const std::streamsize requested = static_cast<std::streamsize>(
                std::min<int64_t>(remaining, static_cast<int64_t>(buffer.size())));
            file.read(buffer.data(), requested);
            const std::streamsize received = file.gcount();
            if (received <= 0 ||
                !sendAll(clientSocket, buffer.data(), static_cast<size_t>(received), bytesSent)) {
                break;
            }
            remaining -= received;
        }
    }
    m_totalBytesSent.fetch_add(bytesSent);
}

std::string HTTPFileServer::getMimeType(const std::string& filename) const {
    std::string extension = std::filesystem::path(filename).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto mime = MIME_TYPES.find(extension);
    return mime == MIME_TYPES.end() ? "application/octet-stream" : mime->second;
}

bool HTTPFileServer::urlDecode(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    auto hexValue = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] != '%') {
            decoded.push_back(encoded[index]);
            continue;
        }
        if (index + 2 >= encoded.size()) {
            return false;
        }
        const int high = hexValue(encoded[index + 1]);
        const int low = hexValue(encoded[index + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return true;
}

bool HTTPFileServer::resolveRequestPath(const std::string& requestPath, std::string& filePath) const {
    std::string decoded;
    if (!urlDecode(requestPath, decoded) || decoded.empty() || decoded.front() != '/' || decoded.find('\0') != std::string::npos) {
        return false;
    }

    std::string relative = decoded.substr(1);
    if (relative.empty()) {
        relative = "index.html";
    }

    std::vector<std::filesystem::path> bases;
    if (decoded.rfind("/media/", 0) == 0) {
        relative = decoded.substr(7);
        bases.emplace_back(m_rootDir);
    } else {
        if (!m_webRoot.empty()) {
            bases.emplace_back(m_webRoot);
        }
        bases.emplace_back(m_rootDir);
    }

    std::error_code error;
    for (const auto& baseValue : bases) {
        const auto base = std::filesystem::weakly_canonical(baseValue, error);
        if (error) {
            error.clear();
            continue;
        }
        const auto candidate = std::filesystem::weakly_canonical(base / relative, error);
        if (error || !pathIsWithin(base, candidate)) {
            error.clear();
            continue;
        }
        if (std::filesystem::is_regular_file(candidate, error)) {
            filePath = candidate.string();
            return true;
        }
        error.clear();
    }
    return false;
}

void HTTPFileServer::handleApiRequest(int clientSocket,
                                      const std::string& method,
                                      const std::string& path,
                                      const std::string& body) {
    if (path == "/api/nodes" && method == "GET") {
        handleGetNodes(clientSocket);
    } else if (path == "/api/node-role" && method == "PUT") {
        handleSetNodeRole(clientSocket, body);
    } else if (path == "/api/node-network" && method == "PUT") {
        handleSetNodeNetwork(clientSocket, body);
    } else if (path == "/api/node-source" && method == "PUT") {
        handleSetNodeSource(clientSocket, body);
    } else if (path == "/api/nodes/discover" && method == "POST") {
        handleDiscoverNodes(clientSocket, body);
    } else if (path == "/api/regions" && method == "GET") {
        handleGetRegions(clientSocket);
    } else if (path == "/api/regions" && method == "PUT") {
        handleSetRegions(clientSocket, body);
    } else if (path == "/api/screens" && method == "GET") {
        handleGetScreens(clientSocket);
    } else if (path == "/api/screens" && method == "PUT") {
        handleSetScreens(clientSocket, body);
    } else if (path == "/api/windows" && method == "GET") {
        handleGetWindows(clientSocket);
    } else if (path == "/api/windows" && method == "PUT") {
        handleSetWindows(clientSocket, body);
    } else if (path == "/api/status" && method == "GET") {
        handleGetStatus(clientSocket);
    } else if (path == "/api/audio-output" && method == "GET") {
        handleGetAudioOutput(clientSocket);
    } else if (path == "/api/audio-output" && method == "PUT") {
        handleSetAudioOutput(clientSocket, body);
    } else if (path == "/api/audio-volume" && method == "GET") {
        handleGetAudioVolume(clientSocket);
    } else if (path == "/api/audio-volume" && method == "PUT") {
        handleSetAudioVolume(clientSocket, body);
    } else if (path == "/api/kvm/status" && method == "GET") {
        handleGetKvmStatus(clientSocket);
    } else if (path == "/api/kvm/acquire" && method == "POST") {
        handleAcquireKvm(clientSocket, body);
    } else if (path == "/api/kvm/release" && method == "POST") {
        handleReleaseKvm(clientSocket, body);
    } else if (path == "/api/play" && method == "POST") {
        handlePlay(clientSocket, body);
    } else if (path == "/api/pause" && method == "POST") {
        handleSimpleControl(clientSocket, "pause", body);
    } else if (path == "/api/resume" && method == "POST") {
        handleSimpleControl(clientSocket, "resume", body);
    } else if (path == "/api/stop" && method == "POST") {
        handleSimpleControl(clientSocket, "stop", body);
    } else if (path == "/api/preload" && method == "POST") {
        handlePreload(clientSocket, body);
    } else {
        static const std::array<std::string, 19> knownPaths = {
            "/api/nodes", "/api/nodes/discover", "/api/screens", "/api/windows",
            "/api/status", "/api/play",
            "/api/pause", "/api/resume", "/api/stop", "/api/preload",
            "/api/audio-output", "/api/audio-volume", "/api/regions",
            "/api/kvm/status", "/api/kvm/acquire", "/api/kvm/release", "/api/node-role",
            "/api/node-network", "/api/node-source"
        };
        if (std::find(knownPaths.begin(), knownPaths.end(), path) != knownPaths.end()) {
            sendError(clientSocket, 405, "Method not allowed for API endpoint");
        } else {
            sendError(clientSocket, 404, "API endpoint not found");
        }
    }
}

void HTTPFileServer::handleGetNodes(int clientSocket) {
    if (!m_nodeDiscovery) {
        sendError(clientSocket, 503, "Node discovery is unavailable");
        return;
    }

    json nodes = json::array();
    for (const auto& node : m_nodeDiscovery->getNodes()) {
        nodes.push_back({
            {"nodeId", node.nodeId},
            {"displayNodeId", formatNodeId(node.nodeId)},
            {"nodeRole", nodeRoleName(node.nodeRole)},
            {"ipMode", node.ipMode},
            {"ip", node.ip},
            {"deviceModel", node.deviceModel},
            {"deviceName", node.deviceName},
            {"softwareVersion", node.softwareVersion},
            {"boardInfo", node.boardInfo},
            {"macAddress", node.macAddress},
            {"subnetMask", node.subnetMask},
            {"gateway", node.gateway},
            {"deviceType", node.deviceType},
            {"resolution", node.resolution},
            {"status", node.status},
            {"playState", node.playState},
            {"cpu", node.cpu},
            {"memory", node.memory},
            {"audioOutputMode", node.audioOutputMode},
            {"masterClockSynchronized", node.masterClockSynchronized},
            {"masterClockOffsetMs", node.masterClockOffsetMs},
            {"commandId", node.commandId},
            {"actualStartTimestamp", node.actualStartTimestamp},
            {"lastError", node.lastError},
            {"kvmEnabled", node.kvmEnabled},
            {"kvmRole", kvmRoleName(node.kvmRole)},
            {"kvmPort", node.kvmPort},
             {"kvmSessionId", node.kvmSessionId},
             {"kvmState", node.kvmState},
             {"kvmLastError", node.kvmLastError},
             {"signalSourceType", node.signalSourceType},
             {"signalSourceConfigured", node.signalSourceConfigured},
             {"signalSourceActive", node.signalSourceActive},
             {"signalSourceEndpoint", node.signalSourceEndpoint},
             {"signalSourceWidth", node.signalSourceWidth},
             {"signalSourceHeight", node.signalSourceHeight},
             {"signalSourceFramerateNumerator", node.signalSourceFramerateNumerator},
             {"signalSourceFramerateDenominator", node.signalSourceFramerateDenominator},
             {"signalSourcePixelFormat", node.signalSourcePixelFormat},
             {"signalSourceTransport", node.signalSourceTransport},
             {"lastSeen", node.lastSeen}
        });
    }
    sendJson(clientSocket, 200, json{{"nodes", nodes}}.dump());
}

json screenPlacementToJson(const dms::ScreenPlacement& placement) {
    return json{
        {"nodeId", placement.nodeId},
        {"x", placement.x},
        {"y", placement.y},
        {"width", placement.width},
        {"height", placement.height}
    };
}

std::vector<dms::ScreenPlacement> parseScreenPlacements(const json& value) {
    if (!value.is_array() || value.size() > 256) {
        throw std::invalid_argument("placements must be an array with at most 256 entries");
    }

    std::vector<dms::ScreenPlacement> placements;
    placements.reserve(value.size());
    for (const auto& item : value) {
        requireExactObject(item, {"nodeId", "x", "y", "width", "height"}, {},
                           "screen placement");
        if (!item.at("nodeId").is_number_unsigned()) {
            throw std::invalid_argument("screen placement.nodeId must be an unsigned integer");
        }
        dms::ScreenPlacement placement;
        placement.nodeId = item.at("nodeId").get<uint32_t>();
        placement.x = item.at("x").get<double>();
        placement.y = item.at("y").get<double>();
        placement.width = item.at("width").get<double>();
        placement.height = item.at("height").get<double>();
        if (placement.nodeId == 0 || !std::isfinite(placement.x) ||
            !std::isfinite(placement.y) || !std::isfinite(placement.width) ||
            !std::isfinite(placement.height) || placement.x < 0.0 || placement.y < 0.0 ||
            placement.width <= 0.0 || placement.height <= 0.0 ||
            placement.x + placement.width > 1.0 ||
            placement.y + placement.height > 1.0) {
            throw std::invalid_argument("screen placement must stay inside the 0-1 canvas");
        }
        placements.push_back(placement);
    }
    return placements;
}

json windowPlacementToJson(const dms::WindowPlacement& placement) {
    return json{{"windowId", placement.windowId},
                {"sourceNodeId", placement.sourceNodeId},
                {"x", placement.x},
                {"y", placement.y},
                {"width", placement.width},
                {"height", placement.height},
                {"zOrder", placement.zOrder}};
}

std::vector<dms::WindowPlacement> parseWindowPlacements(const json& value) {
    if (!value.is_array() || value.size() > MAX_WINDOW_LAYERS) {
        throw std::invalid_argument("windows must be an array with at most 256 entries");
    }
    std::vector<dms::WindowPlacement> placements;
    placements.reserve(value.size());
    std::set<uint32_t> windowIds;
    std::map<uint32_t, uint32_t> countsBySource;
    for (const auto& item : value) {
        requireExactObject(item, {"windowId", "sourceNodeId", "x", "y", "width",
                                  "height", "zOrder"}, {}, "window");
        if (!item.at("windowId").is_number_unsigned() ||
            !item.at("sourceNodeId").is_number_unsigned() ||
            !item.at("zOrder").is_number_unsigned() ||
            !item.at("x").is_number() || !item.at("y").is_number() ||
            !item.at("width").is_number() || !item.at("height").is_number()) {
            throw std::invalid_argument("window fields have invalid types");
        }
        dms::WindowPlacement placement;
        placement.windowId = item.at("windowId").get<uint32_t>();
        placement.sourceNodeId = item.at("sourceNodeId").get<uint32_t>();
        placement.x = item.at("x").get<double>();
        placement.y = item.at("y").get<double>();
        placement.width = item.at("width").get<double>();
        placement.height = item.at("height").get<double>();
        placement.zOrder = item.at("zOrder").get<uint32_t>();
        if (placement.windowId == 0 || placement.sourceNodeId == 0 ||
            placement.zOrder == 0 || !std::isfinite(placement.x) ||
            !std::isfinite(placement.y) || !std::isfinite(placement.width) ||
            !std::isfinite(placement.height) || placement.x < 0.0 ||
            placement.y < 0.0 || placement.width <= 0.0 ||
            placement.height <= 0.0 || placement.x + placement.width > 1.0000001 ||
            placement.y + placement.height > 1.0000001 ||
            !windowIds.insert(placement.windowId).second ||
            ++countsBySource[placement.sourceNodeId] > MAX_WINDOWS_PER_SOURCE) {
            throw std::invalid_argument(
                "windows must be unique, inside the 0-1 wall, and limited to 16 per input node");
        }
        placements.push_back(placement);
    }
    return placements;
}

void HTTPFileServer::handleSetNodeRole(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"nodeId", "role"}, {}, "node role request");
        if (!request.at("nodeId").is_number_unsigned() || !request.at("role").is_string()) {
            throw std::invalid_argument("nodeId must be unsigned and role must be a string");
        }
        const uint64_t rawNodeId = request.at("nodeId").get<uint64_t>();
        NodeRole role;
        if (rawNodeId == 0 || rawNodeId > std::numeric_limits<uint32_t>::max() ||
            !parseNodeRole(request.at("role").get<std::string>(), role)) {
            throw std::invalid_argument("role must be unassigned, encode, decode, or codec");
        }
        const uint32_t nodeId = static_cast<uint32_t>(rawNodeId);
        {
            std::lock_guard<std::mutex> lock(m_regionsMutex);
            if (!m_regionsFile.empty() && std::filesystem::exists(m_regionsFile)) {
                std::ifstream input(m_regionsFile);
                json document;
                if (!input || !(input >> document) || !document.is_object() ||
                    !document.contains("regions") || !document.at("regions").is_array()) {
                    throw std::runtime_error("Cannot inspect region bindings");
                }
                for (auto& region : document.at("regions")) {
                    if (region.is_object() && !region.contains("inputNodeIds")) {
                        region["inputNodeIds"] = json::array();
                    }
                }
                validateRegionsDocument(document);
                for (const auto& region : document.at("regions")) {
                    const std::string regionName = region.value("name", "");
                    const auto& inputNodeIds = region.at("inputNodeIds");
                    const bool boundAsInput = std::any_of(inputNodeIds.begin(), inputNodeIds.end(),
                        [nodeId](const json& value) {
                            return value.is_number_unsigned() && value.get<uint32_t>() == nodeId;
                        });
                    const auto& placements = region.at("placements");
                    const bool boundAsOutput = std::any_of(placements.begin(), placements.end(),
                        [nodeId](const json& placement) {
                            return placement.is_object() && placement.contains("nodeId") &&
                                placement.at("nodeId").is_number_unsigned() &&
                                placement.at("nodeId").get<uint32_t>() == nodeId;
                        });
                    if (boundAsInput && role != NodeRole::ENCODE && role != NodeRole::CODEC) {
                        sendError(clientSocket, 409, "Node is bound as input to region: " + regionName);
                        return;
                    }
                    if (boundAsOutput && role != NodeRole::DECODE && role != NodeRole::CODEC) {
                        sendError(clientSocket, 409, "Node is bound as output to region: " + regionName);
                        return;
                    }
                }
            }
        }
        if (!m_masterApp->setNodeRole(nodeId, role)) {
            sendError(clientSocket, 409, "Node role command could not be sent");
            return;
        }
        sendJson(clientSocket, 200,
                 json{{"success", true}, {"nodeId", nodeId}, {"role", nodeRoleName(role)}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleSetNodeNetwork(int clientSocket, const std::string& body) {
    if (!m_masterApp || !m_nodeDiscovery) {
        sendError(clientSocket, 503, "Node network control is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"nodeId", "mode", "address", "prefixLength",
                                     "gateway", "dnsServers"}, {}, "node network request");
        if (!request.at("nodeId").is_number_unsigned() ||
            !request.at("mode").is_string() || !request.at("address").is_string() ||
            !request.at("prefixLength").is_number_unsigned() ||
            !request.at("gateway").is_string() || !request.at("dnsServers").is_array()) {
            throw std::invalid_argument("nodeId, mode, address, prefixLength, gateway, and dnsServers have invalid types");
        }
        Protocol::SetNodeNetworkCommand command;
        const uint64_t rawNodeId = request.at("nodeId").get<uint64_t>();
        if (rawNodeId == 0 || rawNodeId > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("nodeId must be a positive integer");
        }
        command.nodeId = static_cast<uint32_t>(rawNodeId);
        command.mode = request.at("mode").get<std::string>();
        command.address = request.at("address").get<std::string>();
        const uint64_t rawPrefix = request.at("prefixLength").get<uint64_t>();
        if (rawPrefix > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("prefixLength is out of range");
        }
        command.prefixLength = static_cast<uint32_t>(rawPrefix);
        command.gateway = request.at("gateway").get<std::string>();
        if (request.at("dnsServers").size() > 2) {
            throw std::invalid_argument("dnsServers accepts at most two addresses");
        }
        for (const auto& dns : request.at("dnsServers")) {
            if (!dns.is_string()) throw std::invalid_argument("dnsServers must contain strings");
            command.dnsServers.push_back(dns.get<std::string>());
        }
        std::string validationError;
        if (!Protocol::validateSetNodeNetworkCommand(command, &validationError)) {
            throw std::invalid_argument(validationError);
        }
        if (!m_masterApp->setNodeNetwork(command)) {
            sendError(clientSocket, 409, "Node network command could not be sent; verify that the node is online");
            return;
        }
        sendJson(clientSocket, 200, json{
            {"success", true},
            {"nodeId", command.nodeId},
            {"mode", command.mode},
            {"address", command.address},
            {"prefixLength", command.prefixLength},
            {"gateway", command.gateway},
            {"dnsServers", command.dnsServers},
            {"reconnectRequired", true}
        }.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleSetNodeSource(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"nodeId", "sourceType", "endpoint", "width", "height",
                                     "framerateNumerator", "framerateDenominator", "pixelFormat"},
                           {}, "node source request");
        if (!request.at("nodeId").is_number_unsigned() ||
            !request.at("sourceType").is_string() || !request.at("endpoint").is_string() ||
            !request.at("width").is_number_unsigned() ||
            !request.at("height").is_number_unsigned() ||
            !request.at("framerateNumerator").is_number_unsigned() ||
            !request.at("framerateDenominator").is_number_unsigned() ||
            !request.at("pixelFormat").is_string()) {
            throw std::invalid_argument("node source request contains invalid field types");
        }
        const uint64_t rawNodeId = request.at("nodeId").get<uint64_t>();
        if (rawNodeId == 0 || rawNodeId > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("nodeId must be a positive integer");
        }
        SignalSourceConfig source;
        if (!parseSignalSourceType(request.at("sourceType").get<std::string>(), source.type)) {
            throw std::invalid_argument("sourceType must be none, capture, stream, or network_camera");
        }
        source.endpoint = request.at("endpoint").get<std::string>();
        source.width = request.at("width").get<uint32_t>();
        source.height = request.at("height").get<uint32_t>();
        source.framerateNumerator = request.at("framerateNumerator").get<uint32_t>();
        source.framerateDenominator = request.at("framerateDenominator").get<uint32_t>();
        source.pixelFormat = request.at("pixelFormat").get<std::string>();
        std::string validationError;
        if (!Protocol::validateSignalSourceConfig(source, &validationError)) {
            throw std::invalid_argument(validationError);
        }
        Protocol::SetNodeSourceCommand command;
        command.nodeId = static_cast<uint32_t>(rawNodeId);
        command.source = std::move(source);
        if (!m_masterApp->setNodeSource(command)) {
            sendError(clientSocket, 409, "Node is offline or signal source update failed");
            return;
        }
        sendJson(clientSocket, 200, json{{"success", true}, {"nodeId", command.nodeId}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleDiscoverNodes(int clientSocket, const std::string& body) {
    if (!m_nodeDiscovery) {
        sendError(clientSocket, 503, "Node discovery is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {}, {}, "discovery request");
        const DiscoveryScanResult result = m_nodeDiscovery->discover();
        if (result.busy) {
            sendError(clientSocket, 409, result.error);
            return;
        }
        if (!result.success) {
            sendError(clientSocket, 400, result.error);
            return;
        }
        json networks = json::array();
        for (const auto& network : result.networks) networks.push_back(network);
        sendJson(clientSocket, 200, json{
            {"success", true},
            {"networks", networks},
            {"addressesProbed", result.addressesProbed},
            {"nodesFound", result.nodesFound},
            {"totalNodes", result.totalNodes},
            {"elapsedMs", result.elapsedMs}
        }.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleGetScreens(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    const dms::ScreenLayout layout = m_masterApp->getScreenLayout();
    json screens = json::array();
    for (uint32_t row = 0; row < layout.rows; ++row) {
        for (uint32_t col = 0; col < layout.cols; ++col) {
            screens.push_back({
                {"nodeId", row * layout.cols + col + 1},
                {"row", row},
                {"col", col},
                {"width", layout.width},
                {"height", layout.height}
            });
        }
    }
    json response = json{
        {"layout", {{"rows", layout.rows}, {"cols", layout.cols},
                    {"screenWidth", layout.width}, {"screenHeight", layout.height},
                    {"totalWidth", layout.cols * layout.width},
                    {"totalHeight", layout.rows * layout.height}}},
        {"screens", screens}
    };
    const auto placements = m_masterApp->getScreenPlacements();
    // An empty list is meaningful: it represents a deliberately cleared
    // canvas, rather than an older server that did not persist placements.
    response["placements"] = json::array();
    for (const auto& placement : placements) {
        response["placements"].push_back(screenPlacementToJson(placement));
    }
    sendJson(clientSocket, 200, response.dump());
}

void HTTPFileServer::handleGetRegions(int clientSocket) {
    std::lock_guard<std::mutex> lock(m_regionsMutex);
    try {
        if (m_regionsFile.empty() || !std::filesystem::exists(m_regionsFile)) {
            sendJson(clientSocket, 200, json{{"regions", json::array()}}.dump());
            return;
        }
        std::ifstream input(m_regionsFile);
        if (!input) {
            throw std::runtime_error("Cannot open regions file");
        }
        json document;
        input >> document;
        if (document.is_object() && document.contains("regions") &&
            document.at("regions").is_array()) {
            for (auto& region : document.at("regions")) {
                if (region.is_object() && !region.contains("inputNodeIds")) {
                    region["inputNodeIds"] = json::array();
                }
            }
        }
        validateRegionsDocument(document);
        sendJson(clientSocket, 200, document.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 500, error.what());
    }
}

void HTTPFileServer::handleSetRegions(int clientSocket, const std::string& body) {
    std::lock_guard<std::mutex> lock(m_regionsMutex);
    try {
        const json document = json::parse(body);
        validateRegionsDocument(document);
        if (m_nodeDiscovery) {
            const auto nodes = m_nodeDiscovery->getNodes();
            const auto nodeById = [&nodes](uint32_t nodeId) -> const DiscoveredNode* {
                const auto found = std::find_if(nodes.begin(), nodes.end(),
                    [nodeId](const DiscoveredNode& node) { return node.nodeId == nodeId; });
                return found == nodes.end() ? nullptr : &*found;
            };
            for (const auto& region : document.at("regions")) {
                for (const auto& value : region.at("inputNodeIds")) {
                    const DiscoveredNode* node = nodeById(value.get<uint32_t>());
                    if (node && node->nodeRole != NodeRole::ENCODE &&
                        node->nodeRole != NodeRole::CODEC) {
                        throw std::invalid_argument("region inputNodeIds must reference input-capable nodes");
                    }
                }
                for (const auto& placement : region.at("placements")) {
                    const DiscoveredNode* node = nodeById(placement.at("nodeId").get<uint32_t>());
                    if (node && node->nodeRole != NodeRole::DECODE &&
                        node->nodeRole != NodeRole::CODEC) {
                        throw std::invalid_argument("region placements must reference output-capable nodes");
                    }
                }
            }
        }
        const std::filesystem::path target(m_regionsFile);
        if (target.empty()) {
            throw std::runtime_error("Regions file is not configured");
        }
        if (!target.parent_path().empty()) {
            std::filesystem::create_directories(target.parent_path());
        }
        const std::filesystem::path temporary = target.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot create temporary regions file");
            }
            output << document.dump(2) << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error("Cannot write regions file");
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Cannot replace regions file: " + error.message());
        }
        sendJson(clientSocket, 200, document.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleSetScreens(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"rows", "cols", "screenWidth", "screenHeight"},
                           {"placements"}, "screens request");
        const dms::ScreenLayout layout{
            request.at("rows").get<uint32_t>(),
            request.at("cols").get<uint32_t>(),
            request.at("screenWidth").get<uint32_t>(),
            request.at("screenHeight").get<uint32_t>()
        };
        const bool hasPlacements = request.contains("placements");
        const auto placements = hasPlacements
            ? parseScreenPlacements(request.at("placements"))
            : std::vector<dms::ScreenPlacement>{};
        const bool updated = hasPlacements
            ? m_masterApp->updateScreenLayout(layout, placements)
            : m_masterApp->updateScreenLayout(layout, {});
        if (!updated) {
            sendError(clientSocket, 400, "Invalid screen layout");
            return;
        }
        json response{{"success", true}};
        if (hasPlacements) {
            response["placements"] = json::array();
            for (const auto& placement : placements) {
                response["placements"].push_back(screenPlacementToJson(placement));
            }
        }
        sendJson(clientSocket, 200, response.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleGetStatus(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    const dms::MasterPlaybackStatus status = m_masterApp->getPlaybackStatus();
    sendJson(clientSocket, 200, json{
        {"running", m_masterApp->isRunning()},
        {"state", status.state},
        {"videoUrl", status.videoUrl},
        {"commandId", status.commandId},
        {"syncTimestamp", status.syncTimestamp},
        {"updatedAt", status.updatedAt},
        {"nodes", {
            {"expected", status.expectedNodeCount},
            {"ready", status.readyNodeCount},
            {"playing", status.playingNodeCount},
            {"error", status.errorNodeCount},
            {"missing", status.missingNodeCount},
            {"maxStartSkewMs", status.maxStartSkewMs}
        }},
        {"http", {{"activeConnections", getActiveConnections()},
                  {"totalRequests", getTotalRequests()},
                  {"totalBytesSent", getTotalBytesSent()}}}
    }.dump());
}

void HTTPFileServer::handleGetWindows(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    json windows = json::array();
    for (const auto& placement : m_masterApp->getWindowPlacements()) {
        windows.push_back(windowPlacementToJson(placement));
    }
    sendJson(clientSocket, 200, json{{"windows", windows}}.dump());
}

void HTTPFileServer::handleSetWindows(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"windows"}, {}, "windows request");
        const auto windows = parseWindowPlacements(request.at("windows"));
        if (!m_masterApp->updateWindowLayout(windows)) {
            sendError(clientSocket, 409,
                      "Window layout references an unavailable input or could not reach an output");
            return;
        }
        json responseWindows = json::array();
        for (const auto& placement : windows) {
            responseWindows.push_back(windowPlacementToJson(placement));
        }
        sendJson(clientSocket, 200,
                 json{{"success", true}, {"windows", responseWindows}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleGetAudioOutput(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    sendJson(clientSocket, 200,
             json{{"mode", audioOutputModeName(m_masterApp->getAudioOutputMode())}}.dump());
}

void HTTPFileServer::handleSetAudioOutput(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"mode"}, {}, "audio output request");
        if (!request.at("mode").is_string()) {
            throw std::invalid_argument("audio output mode must be a string");
        }
        AudioOutputMode mode;
        if (!parseAudioOutputMode(request.at("mode").get<std::string>(), mode)) {
            throw std::invalid_argument("audio output mode must be hdmi, analog, or both");
        }
        if (!m_masterApp->setAudioOutputMode(mode)) {
            sendError(clientSocket, 409, "Audio output command could not be sent");
            return;
        }
        sendJson(clientSocket, 200,
                 json{{"success", true}, {"mode", audioOutputModeName(mode)}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleGetAudioVolume(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    sendJson(clientSocket, 200,
             json{{"volumePercent", m_masterApp->getAudioVolumePercent()}}.dump());
}

void HTTPFileServer::handleSetAudioVolume(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"volumePercent"}, {}, "audio volume request");
        if (!request.at("volumePercent").is_number_unsigned()) {
            throw std::invalid_argument("audio volume percent must be an unsigned integer");
        }
        const uint64_t value = request.at("volumePercent").get<uint64_t>();
        if (value > 100) {
            throw std::invalid_argument("audio volume percent must be between 0 and 100");
        }
        const uint32_t volumePercent = static_cast<uint32_t>(value);
        if (!m_masterApp->setAudioVolumePercent(volumePercent)) {
            sendError(clientSocket, 409, "Audio volume command could not be sent");
            return;
        }
        sendJson(clientSocket, 200,
                 json{{"success", true}, {"volumePercent", volumePercent}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleGetKvmStatus(int clientSocket) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    const KvmSession session = m_masterApp->getKvmSession();
    sendJson(clientSocket, 200, json{
        {"sessionId", session.sessionId},
        {"controllerNodeId", session.controllerNodeId},
        {"controllerNodeLabel", session.controllerNodeId ? formatNodeId(session.controllerNodeId) : ""},
        {"targetNodeId", session.targetNodeId},
        {"targetNodeLabel", session.targetNodeId ? formatNodeId(session.targetNodeId) : ""},
        {"state", session.sessionId ? session.state : "idle"},
        {"acquiredAt", session.acquiredAt},
        {"expiresAt", session.expiresAt}
    }.dump());
}

void HTTPFileServer::handleAcquireKvm(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"targetNodeId", "leaseMs"}, {},
                           "KVM acquire request");
        if (!request.at("targetNodeId").is_number_unsigned() ||
            !request.at("leaseMs").is_number_unsigned()) {
            throw std::invalid_argument("KVM targetNodeId and leaseMs must be unsigned integers");
        }
        const uint64_t target = request.at("targetNodeId").get<uint64_t>();
        const uint64_t leaseMs = request.at("leaseMs").get<uint64_t>();
        if (target == 0 || target > std::numeric_limits<uint32_t>::max() ||
            leaseMs < 1000 || leaseMs > 3600000) {
            throw std::invalid_argument("Invalid KVM target node or lease duration");
        }
        KvmSession session;
        if (!m_masterApp->acquireKvmSession(0, static_cast<uint32_t>(target), leaseMs, session)) {
            sendError(clientSocket, 409, "KVM session unavailable or target node does not support KVM");
            return;
        }
        sendJson(clientSocket, 200, json{
            {"sessionId", session.sessionId}, {"controllerNodeId", session.controllerNodeId},
            {"controllerNodeLabel", session.controllerNodeId ? formatNodeId(session.controllerNodeId) : ""},
            {"targetNodeId", session.targetNodeId},
            {"targetNodeLabel", formatNodeId(session.targetNodeId)},
            {"state", session.state}, {"acquiredAt", session.acquiredAt},
            {"expiresAt", session.expiresAt}, {"targetIp", session.targetIp},
            {"targetPort", session.targetPort}, {"sessionToken", session.sessionToken}
        }.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleReleaseKvm(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"sessionId"}, {}, "KVM release request");
        if (!request.at("sessionId").is_number_unsigned()) {
            throw std::invalid_argument("KVM sessionId must be an unsigned integer");
        }
        const uint64_t sessionId = request.at("sessionId").get<uint64_t>();
        if (sessionId == 0 || sessionId > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("Invalid KVM sessionId");
        }
        if (!m_masterApp->releaseKvmSession(static_cast<uint32_t>(sessionId))) {
            sendError(clientSocket, 409, "KVM session does not exist or release broadcast failed");
            return;
        }
        sendJson(clientSocket, 200, json{{"success", true}, {"sessionId", sessionId}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void validateRegionsDocument(const json& document) {
    requireExactObject(document, {"regions"}, {}, "regions request");
    const json& regions = document.at("regions");
    if (!regions.is_array() || regions.size() > 48) {
        throw std::invalid_argument("regions must be an array with at most 48 entries");
    }

    std::set<std::string> names;
    std::set<uint32_t> boundNodeIds;
    for (const auto& region : regions) {
        requireExactObject(region, {"name", "inputNodeIds", "layout", "placements", "savedAt"},
                           {}, "region");
        if (!region.at("name").is_string()) {
            throw std::invalid_argument("region.name must be a string");
        }
        const std::string name = region.at("name").get<std::string>();
        if (name.empty() || name.size() > 128 || !names.emplace(name).second) {
            throw std::invalid_argument("region.name must be unique and contain 1-128 bytes");
        }
        if (!region.at("savedAt").is_number_unsigned()) {
            throw std::invalid_argument("region.savedAt must be an unsigned integer");
        }

        const json& layout = region.at("layout");
        requireExactObject(layout, {"rows", "cols", "screenWidth", "screenHeight"}, {},
                           "region.layout");
        for (const char* key : {"rows", "cols", "screenWidth", "screenHeight"}) {
            if (!layout.at(key).is_number_unsigned()) {
                throw std::invalid_argument(std::string("region.layout.") + key +
                                            " must be an unsigned integer");
            }
        }
        const uint64_t rows = layout.at("rows").get<uint64_t>();
        const uint64_t cols = layout.at("cols").get<uint64_t>();
        const uint64_t width = layout.at("screenWidth").get<uint64_t>();
        const uint64_t height = layout.at("screenHeight").get<uint64_t>();
        if (rows < 1 || rows > 8 || cols < 1 || cols > 8 || rows * cols > 64 ||
            width < 1 || width > 16384 || height < 1 || height > 16384) {
            throw std::invalid_argument("region.layout values are out of range");
        }
        const json& placements = region.at("placements");
        if (!placements.is_array() || placements.size() > 256) {
            throw std::invalid_argument("region.placements must be an array with at most 256 entries");
        }
        if (placements.size() > rows * cols) {
            throw std::invalid_argument("region.placements cannot exceed the output wall capacity");
        }
        std::set<uint32_t> regionNodeIds;
        const json& inputNodeIds = region.at("inputNodeIds");
        if (!inputNodeIds.is_array() || inputNodeIds.size() > 256) {
            throw std::invalid_argument("region.inputNodeIds must be an array with at most 256 entries");
        }
        for (const auto& value : inputNodeIds) {
            if (!value.is_number_unsigned()) {
                throw std::invalid_argument("region.inputNodeIds must contain unsigned integers");
            }
            const uint32_t nodeId = value.get<uint32_t>();
            if (nodeId == 0 || !regionNodeIds.emplace(nodeId).second) {
                throw std::invalid_argument("region inputNodeIds must be unique and non-zero");
            }
            if (!boundNodeIds.emplace(nodeId).second) {
                throw std::invalid_argument("a node cannot be bound to multiple regions");
            }
        }
        std::set<uint32_t> nodeIds;
        for (const auto& placement : placements) {
            requireExactObject(placement, {"nodeId", "x", "y", "width", "height"}, {},
                               "region.placement");
            if (!placement.at("nodeId").is_number_unsigned()) {
                throw std::invalid_argument("region.placement.nodeId must be an unsigned integer");
            }
            const uint32_t nodeId = placement.at("nodeId").get<uint32_t>();
            if (nodeId == 0 || !nodeIds.emplace(nodeId).second ||
                !regionNodeIds.emplace(nodeId).second) {
                throw std::invalid_argument("region placement nodeId must be unique and non-zero");
            }
            if (!boundNodeIds.emplace(nodeId).second) {
                throw std::invalid_argument("a node cannot be bound to multiple regions");
            }
            for (const char* key : {"x", "y", "width", "height"}) {
                if (!placement.at(key).is_number()) {
                    throw std::invalid_argument(std::string("region.placement.") + key +
                                                " must be a number");
                }
            }
            const double x = placement.at("x").get<double>();
            const double y = placement.at("y").get<double>();
            const double placementWidth = placement.at("width").get<double>();
            const double placementHeight = placement.at("height").get<double>();
            if (x < 0.0 || y < 0.0 || placementWidth <= 0.0 || placementHeight <= 0.0 ||
                x + placementWidth > 1.000001 || y + placementHeight > 1.000001) {
                throw std::invalid_argument("region placement must stay inside the normalized 0-1 wall");
            }
        }
    }
}

void HTTPFileServer::handlePlay(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"videoUrl", "video"},
                           {"delayMs", "layout", "placements"}, "play request");
        const std::string videoUrl = request.at("videoUrl").get<std::string>();
        const json video = request.at("video");
        requireExactObject(video, {"width", "height"}, {"fps", "duration"}, "video");
        const dms::VideoInfo videoInfo{
            video.at("width").get<uint32_t>(),
            video.at("height").get<uint32_t>(),
            video.value("fps", 0U),
            video.value("duration", 0ULL)
        };
        const uint64_t delayMs = request.value("delayMs", m_defaultSyncDelayMs);
        if (videoUrl.empty() || videoInfo.width == 0 || videoInfo.height == 0 ||
            videoInfo.width > 65535 || videoInfo.height > 65535 || videoInfo.fps > 240 ||
            delayMs > 24ULL * 60ULL * 60ULL * 1000ULL) {
            sendError(clientSocket, 400, "Invalid playback parameters");
            return;
        }

        const dms::ScreenLayout previousLayout = m_masterApp->getScreenLayout();
        const auto previousPlacements = m_masterApp->getScreenPlacements();
        bool layoutChanged = false;
        if (request.contains("layout")) {
            const json layoutJson = request.at("layout");
            requireExactObject(layoutJson, {"rows", "cols", "screenWidth", "screenHeight"}, {}, "layout");
            const dms::ScreenLayout layout{
                layoutJson.at("rows").get<uint32_t>(),
                layoutJson.at("cols").get<uint32_t>(),
                layoutJson.at("screenWidth").get<uint32_t>(),
                layoutJson.at("screenHeight").get<uint32_t>()
            };
            const bool hasPlacements = request.contains("placements");
            const auto placements = hasPlacements
                ? parseScreenPlacements(request.at("placements"))
                : std::vector<dms::ScreenPlacement>{};
            const bool updated = hasPlacements
                ? m_masterApp->setScreenLayout(layout, placements)
                : m_masterApp->setScreenLayout(layout);
            if (!updated) {
                sendError(clientSocket, 400, "Invalid screen layout");
                return;
            }
            layoutChanged = true;
        }

        if (!m_masterApp->play(videoUrl, videoInfo, delayMs)) {
            if (layoutChanged) {
                m_masterApp->setScreenLayout(previousLayout, previousPlacements);
            }
            sendError(clientSocket, 409, "Playback command could not be sent");
            return;
        }
        const dms::MasterPlaybackStatus playbackStatus = m_masterApp->getPlaybackStatus();
        sendJson(clientSocket, 200, json{{"success", true},
                                         {"status", playbackStatus.state},
                                         {"commandId", playbackStatus.commandId},
                                         {"syncTimestamp", playbackStatus.syncTimestamp}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

void HTTPFileServer::handleSimpleControl(int clientSocket,
                                         const std::string& operation,
                                         const std::string& body) {
    try {
        const json request = json::parse(body);
        requireExactObject(request, {}, {}, "control request");
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
        return;
    }
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }

    bool success = false;
    if (operation == "pause") {
        success = m_masterApp->pause();
    } else if (operation == "resume") {
        success = m_masterApp->resume();
    } else if (operation == "stop") {
        success = m_masterApp->stop();
    }
    if (!success) {
        sendError(clientSocket, 409, "Control command could not be sent");
        return;
    }
    sendJson(clientSocket, 200, json{{"success", true}, {"operation", operation}}.dump());
}

void HTTPFileServer::handlePreload(int clientSocket, const std::string& body) {
    if (!m_masterApp) {
        sendError(clientSocket, 503, "Master application is unavailable");
        return;
    }
    try {
        const json request = json::parse(body);
        requireExactObject(request, {"videoUrl"}, {}, "preload request");
        const std::string videoUrl = request.at("videoUrl").get<std::string>();
        if (videoUrl.empty()) {
            sendError(clientSocket, 400, "videoUrl must not be empty");
            return;
        }
        if (!m_masterApp->preloadVideo(videoUrl)) {
            sendError(clientSocket, 409, "Video preload failed");
            return;
        }
        sendJson(clientSocket, 200, json{{"success", true}}.dump());
    } catch (const std::exception& error) {
        sendError(clientSocket, 400, error.what());
    }
}

} // namespace dms


