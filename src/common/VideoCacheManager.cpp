#include "common/VideoCacheManager.h"
#include "common/Logger.h"

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace dms {
namespace {

std::once_flag curlInitFlag;
std::atomic<bool> curlReady{false};

struct TransferContext {
    std::ofstream* file = nullptr;
    DownloadProgressCallback progress;
    const std::atomic<bool>* cancelled = nullptr;
};

size_t writeCallback(void* contents, size_t size, size_t count, void* userData) {
    auto* context = static_cast<TransferContext*>(userData);
    if (!context || !context->file || !context->file->is_open() ||
        (context->cancelled && context->cancelled->load())) {
        return 0;
    }
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        return 0;
    }
    const size_t bytes = size * count;
    context->file->write(static_cast<const char*>(contents), static_cast<std::streamsize>(bytes));
    return context->file->good() ? bytes : 0;
}

int progressCallback(void* userData,
                     curl_off_t downloadTotal,
                     curl_off_t downloaded,
                     curl_off_t,
                     curl_off_t) {
    auto* context = static_cast<TransferContext*>(userData);
    if (!context || (context->cancelled && context->cancelled->load())) {
        return 1;
    }
    if (context->progress && downloadTotal >= 0 && downloaded >= 0) {
        context->progress(static_cast<uint64_t>(downloaded),
                          static_cast<uint64_t>(downloadTotal));
    }
    return 0;
}

bool hasScheme(const std::string& value) {
    return value.find("://") != std::string::npos;
}

std::string localSourcePath(const std::string& url) {
    constexpr const char* FILE_PREFIX = "file://";
    if (url.compare(0, 7, FILE_PREFIX) == 0) {
        return url.substr(7);
    }
    return hasScheme(url) ? std::string{} : url;
}

} // namespace

VideoCacheManager::VideoCacheManager()
    : m_maxCacheSize(0),
      m_currentCacheSize(0),
      m_initialized(false),
      m_shuttingDown(false) {
}

VideoCacheManager::~VideoCacheManager() {
    shutdown();
}

bool VideoCacheManager::initialize(const char* cacheDir, uint64_t maxCacheSizeBytes) {
    if (cacheDir == nullptr || *cacheDir == '\0' || maxCacheSizeBytes == 0) {
        LOG_ERROR("Cache directory and maximum size must be valid");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (m_initialized.load()) {
        LOG_WARNING("VideoCacheManager is already initialized");
        return false;
    }

    std::call_once(curlInitFlag, [] {
        curlReady.store(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    });
    if (!curlReady.load()) {
        LOG_ERROR("Failed to initialize libcurl");
        return false;
    }

    std::error_code error;
    const std::filesystem::path directory(cacheDir);
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        LOG_ERROR("Invalid cache directory: %s", cacheDir);
        return false;
    }

    m_cacheDir = directory.lexically_normal().string();
    m_maxCacheSize = maxCacheSizeBytes;
    m_currentCacheSize.store(0);
    m_cache.clear();
    m_asyncTasks.clear();
    m_shuttingDown.store(false);

    uint64_t orphanIndex = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            LOG_ERROR("Failed to scan cache directory: %s", error.message().c_str());
            return false;
        }
        if (!item.is_regular_file(error)) {
            continue;
        }
        if (item.path().extension() == ".part") {
            std::filesystem::remove(item.path(), error);
            error.clear();
            continue;
        }
        const uint64_t size = static_cast<uint64_t>(item.file_size(error));
        if (error) {
            error.clear();
            continue;
        }
        CacheEntry entry;
        entry.localPath = item.path().string();
        entry.fileSize = size;
        entry.lastAccessTime = Protocol::getCurrentTimestamp();
        entry.isComplete = true;
        m_cache.emplace("__orphan__" + std::to_string(orphanIndex++), std::move(entry));
        m_currentCacheSize.fetch_add(size);
    }

    m_initialized.store(true);
    evictLRU(0);
    LOG_INFO("Video cache initialized: dir=%s, max=%llu, current=%llu",
             m_cacheDir.c_str(),
             static_cast<unsigned long long>(m_maxCacheSize),
             static_cast<unsigned long long>(m_currentCacheSize.load()));
    return true;
}

void VideoCacheManager::shutdown() {
    if (!m_initialized.exchange(false)) {
        return;
    }
    m_shuttingDown.store(true);
    m_cacheCondition.notify_all();

    std::vector<std::future<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        tasks.swap(m_asyncTasks);
    }
    for (auto& task : tasks) {
        if (task.valid()) {
            try {
                task.get();
            } catch (const std::exception& error) {
                LOG_ERROR("Preload task failed: %s", error.what());
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
    m_currentCacheSize.store(0);
    LOG_INFO("Video cache shutdown");
}

bool VideoCacheManager::getVideo(const std::string& url, std::string& localPath) {
    return getVideoWithProgress(url, localPath, nullptr);
}

bool VideoCacheManager::getVideoWithProgress(const std::string& url,
                                             std::string& localPath,
                                             DownloadProgressCallback progress) {
    return acquireVideo(url, localPath, std::move(progress));
}

bool VideoCacheManager::acquireVideo(const std::string& url,
                                     std::string& localPath,
                                     DownloadProgressCallback progress) {
    if (url.empty() || !m_initialized.load() || m_shuttingDown.load()) {
        return false;
    }

    bool performTransfer = false;
    {
        std::unique_lock<std::mutex> lock(m_cacheMutex);
        while (true) {
            auto found = m_cache.find(url);
            if (found != m_cache.end()) {
                localPath = found->second.localPath;
                if (found->second.isComplete &&
                    std::filesystem::is_regular_file(found->second.localPath)) {
                    found->second.lastAccessTime = Protocol::getCurrentTimestamp();
                    return true;
                }
                if (found->second.isDownloading) {
                    m_cacheCondition.wait(lock, [this, &url] {
                        const auto current = m_cache.find(url);
                        return m_shuttingDown.load() || current == m_cache.end() ||
                               !current->second.isDownloading;
                    });
                    if (m_shuttingDown.load()) {
                        return false;
                    }
                    continue;
                }
                m_cache.erase(found);
            }

            localPath = generateLocalPath(url);
            std::error_code error;
            if (std::filesystem::is_regular_file(localPath, error)) {
                const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(localPath, error));
                if (!error && size <= m_maxCacheSize) {
                    for (auto it = m_cache.begin(); it != m_cache.end();) {
                        if (it->first.rfind("__orphan__", 0) == 0 && it->second.localPath == localPath) {
                            it = m_cache.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    m_cache[url] = CacheEntry{url, localPath, size,
                        Protocol::getCurrentTimestamp(), false, true};
                    return true;
                }
            }

            m_cache[url] = CacheEntry{url, localPath, 0,
                Protocol::getCurrentTimestamp(), true, false};
            performTransfer = true;
            break;
        }
    }

    const bool success = performTransfer && transferToCache(url, localPath, std::move(progress));
    finishTransfer(url, localPath, success);
    return success;
}

bool VideoCacheManager::preloadVideo(const std::string& url) {
    if (url.empty() || !m_initialized.load() || m_shuttingDown.load()) {
        return false;
    }

    std::string localPath;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto found = m_cache.find(url);
        if (found != m_cache.end() && (found->second.isComplete || found->second.isDownloading)) {
            return true;
        }
        localPath = generateLocalPath(url);
        std::error_code error;
        if (std::filesystem::is_regular_file(localPath, error)) {
            const uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(localPath, error));
            if (!error && size <= m_maxCacheSize) {
                for (auto it = m_cache.begin(); it != m_cache.end();) {
                    if (it->first.rfind("__orphan__", 0) == 0 && it->second.localPath == localPath) {
                        it = m_cache.erase(it);
                    } else {
                        ++it;
                    }
                }
                m_cache[url] = CacheEntry{url, localPath, size,
                    Protocol::getCurrentTimestamp(), false, true};
                return true;
            }
        }
        m_cache[url] = CacheEntry{url, localPath, 0,
            Protocol::getCurrentTimestamp(), true, false};
        try {
            m_asyncTasks.emplace_back(std::async(std::launch::async, [this, url, localPath] {
                const bool success = transferToCache(url, localPath, nullptr);
                finishTransfer(url, localPath, success);
            }));
        } catch (...) {
            m_cache.erase(url);
            m_cacheCondition.notify_all();
            return false;
        }
    }
    return true;
}

void VideoCacheManager::finishTransfer(const std::string& url,
                                       const std::string& localPath,
                                       bool success) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto found = m_cache.find(url);
    if (found == m_cache.end()) {
        deleteCacheFile(localPath);
        deleteCacheFile(localPath + ".part");
        m_cacheCondition.notify_all();
        return;
    }

    const uint64_t size = success ? getFileSize(localPath) : 0;
    if (!success || size == 0 || size > m_maxCacheSize || m_shuttingDown.load()) {
        deleteCacheFile(localPath);
        deleteCacheFile(localPath + ".part");
        m_cache.erase(found);
        m_cacheCondition.notify_all();
        return;
    }

    evictLRU(size, url);
    if (m_currentCacheSize.load() > m_maxCacheSize - size) {
        deleteCacheFile(localPath);
        m_cache.erase(found);
        m_cacheCondition.notify_all();
        return;
    }
    found = m_cache.find(url);
    if (found == m_cache.end()) {
        deleteCacheFile(localPath);
        m_cacheCondition.notify_all();
        return;
    }
    found->second.fileSize = size;
    found->second.isDownloading = false;
    found->second.isComplete = true;
    found->second.lastAccessTime = Protocol::getCurrentTimestamp();
    m_currentCacheSize.fetch_add(size);
    m_cacheCondition.notify_all();
    LOG_INFO("Video cached: %s (%llu bytes)", url.c_str(),
             static_cast<unsigned long long>(size));
}

bool VideoCacheManager::transferToCache(const std::string& url,
                                        const std::string& localPath,
                                        DownloadProgressCallback progress) {
    const std::string source = localSourcePath(url);
    if (!source.empty()) {
        return copyLocalFile(source, localPath, std::move(progress));
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        LOG_ERROR("Unsupported video URL scheme: %s", url.c_str());
        return false;
    }

    const std::string partPath = localPath + ".part";
    std::ofstream file(partPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        file.close();
        deleteCacheFile(partPath);
        return false;
    }
    TransferContext context{&file, std::move(progress), &m_shuttingDown};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    const CURLcode protocolResult = curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    const CURLcode redirectProtocolResult =
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    const long allowedProtocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    const CURLcode protocolResult = curl_easy_setopt(curl, CURLOPT_PROTOCOLS, allowedProtocols);
    const CURLcode redirectProtocolResult =
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, allowedProtocols);
#endif
    if (protocolResult != CURLE_OK || redirectProtocolResult != CURLE_OK) {
        LOG_ERROR("Failed to restrict download protocols: %s / %s",
                  curl_easy_strerror(protocolResult), curl_easy_strerror(redirectProtocolResult));
        curl_easy_cleanup(curl);
        file.close();
        deleteCacheFile(partPath);
        return false;
    }

    const CURLcode result = curl_easy_perform(curl);
    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_easy_cleanup(curl);
    file.close();

    if (result != CURLE_OK || responseCode < 200 || responseCode >= 300 || m_shuttingDown.load()) {
        LOG_ERROR("Video download failed: %s (%s, HTTP %ld)", url.c_str(),
                  curl_easy_strerror(result), responseCode);
        deleteCacheFile(partPath);
        return false;
    }

    std::error_code error;
    std::filesystem::rename(partPath, localPath, error);
    if (error) {
        deleteCacheFile(partPath);
        return false;
    }
    return true;
}

bool VideoCacheManager::copyLocalFile(const std::string& source,
                                      const std::string& destination,
                                      DownloadProgressCallback progress) {
    std::error_code error;
    const std::filesystem::path sourcePath(source);
    if (!std::filesystem::is_regular_file(sourcePath, error)) {
        LOG_ERROR("Local video does not exist or is not a regular file: %s", source.c_str());
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(std::filesystem::file_size(sourcePath, error));
    if (error || total == 0 || total > m_maxCacheSize) {
        return false;
    }

    const std::string partPath = destination + ".part";
    std::ifstream input(sourcePath, std::ios::binary);
    std::ofstream output(partPath, std::ios::binary | std::ios::trunc);
    if (!input.is_open() || !output.is_open()) {
        deleteCacheFile(partPath);
        return false;
    }

    std::array<char, 1024 * 1024> buffer{};
    uint64_t copied = 0;
    while (input && !m_shuttingDown.load()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        output.write(buffer.data(), count);
        if (!output.good()) {
            break;
        }
        copied += static_cast<uint64_t>(count);
        if (progress) {
            progress(copied, total);
        }
    }
    input.close();
    output.close();
    if (m_shuttingDown.load() || copied != total) {
        deleteCacheFile(partPath);
        return false;
    }

    std::filesystem::rename(partPath, destination, error);
    if (error) {
        deleteCacheFile(partPath);
        return false;
    }
    return true;
}

bool VideoCacheManager::isCached(const std::string& url) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    const auto found = m_cache.find(url);
    return found != m_cache.end() && found->second.isComplete &&
           std::filesystem::is_regular_file(found->second.localPath);
}

void VideoCacheManager::clearCache() {
    std::unique_lock<std::mutex> lock(m_cacheMutex);
    m_cacheCondition.wait(lock, [this] {
        return std::none_of(m_cache.begin(), m_cache.end(), [](const auto& item) {
            return item.second.isDownloading;
        });
    });
    for (const auto& item : m_cache) {
        if (item.second.isComplete) {
            deleteCacheFile(item.second.localPath);
        }
    }
    m_cache.clear();
    m_currentCacheSize.store(0);
}

size_t VideoCacheManager::getCacheEntryCount() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cache.size();
}

std::string VideoCacheManager::generateLocalPath(const std::string& url) const {
    std::string extension = ".bin";
    const std::string withoutQuery = url.substr(0, url.find_first_of("?#"));
    const auto slash = withoutQuery.find_last_of("/\\");
    const auto dot = withoutQuery.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string candidate = withoutQuery.substr(dot);
        if (candidate.size() >= 2 && candidate.size() <= 12 &&
            std::all_of(candidate.begin() + 1, candidate.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0;
            })) {
            extension = candidate;
        }
    }
    return (std::filesystem::path(m_cacheDir) / (hashURL(url) + extension)).string();
}

void VideoCacheManager::evictLRU(uint64_t requiredSize, const std::string& protectedUrl) {
    if (requiredSize > m_maxCacheSize) {
        return;
    }
    std::vector<std::pair<std::string, uint64_t>> candidates;
    for (const auto& item : m_cache) {
        if (item.first != protectedUrl && item.second.isComplete && !item.second.isDownloading) {
            candidates.emplace_back(item.first, item.second.lastAccessTime);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });

    for (const auto& candidate : candidates) {
        const uint64_t current = m_currentCacheSize.load();
        if (current <= m_maxCacheSize - requiredSize) {
            break;
        }
        const auto found = m_cache.find(candidate.first);
        if (found == m_cache.end()) {
            continue;
        }
        const uint64_t size = found->second.fileSize;
        if (deleteCacheFile(found->second.localPath)) {
            m_currentCacheSize.fetch_sub(std::min(size, m_currentCacheSize.load()));
            m_cache.erase(found);
        }
    }
}

bool VideoCacheManager::deleteCacheFile(const std::string& localPath) const {
    if (localPath.empty()) {
        return false;
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(localPath, error);
    return removed || (!error && !std::filesystem::exists(localPath));
}

std::string VideoCacheManager::hashURL(const std::string& url) const {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : url) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

uint64_t VideoCacheManager::getFileSize(const std::string& path) const {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return 0;
    }
    return static_cast<uint64_t>(std::filesystem::file_size(path, error));
}

} // namespace dms
