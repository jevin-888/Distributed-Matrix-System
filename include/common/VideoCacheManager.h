#pragma once

#include "common/Protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dms {

using DownloadProgressCallback = std::function<void(uint64_t downloaded, uint64_t total)>;

struct CacheEntry {
    std::string url;
    std::string localPath;
    uint64_t fileSize = 0;
    uint64_t lastAccessTime = 0;
    bool isDownloading = false;
    bool isComplete = false;
};

class VideoCacheManager {
public:
    VideoCacheManager();
    ~VideoCacheManager();

    VideoCacheManager(const VideoCacheManager&) = delete;
    VideoCacheManager& operator=(const VideoCacheManager&) = delete;

    bool initialize(const char* cacheDir, uint64_t maxCacheSizeBytes);
    void shutdown();

    // 同步获取：命中缓存时立即返回，否则等待唯一下载任务完成。
    bool getVideo(const std::string& url, std::string& localPath);
    bool getVideoWithProgress(const std::string& url,
                              std::string& localPath,
                              DownloadProgressCallback progressCallback = nullptr);

    // 异步提交预加载；返回 true 表示已缓存或任务已被接受。
    bool preloadVideo(const std::string& url);

    bool isCached(const std::string& url) const;
    void clearCache();
    uint64_t getCurrentCacheSize() const { return m_currentCacheSize.load(); }
    size_t getCacheEntryCount() const;

private:
    bool acquireVideo(const std::string& url,
                      std::string& localPath,
                      DownloadProgressCallback progressCallback);
    bool transferToCache(const std::string& url,
                         const std::string& localPath,
                         DownloadProgressCallback progressCallback);
    bool copyLocalFile(const std::string& source,
                       const std::string& destination,
                       DownloadProgressCallback progressCallback);
    void finishTransfer(const std::string& url,
                        const std::string& localPath,
                        bool success);
    std::string generateLocalPath(const std::string& url) const;
    void evictLRU(uint64_t requiredSize, const std::string& protectedUrl = {});
    bool deleteCacheFile(const std::string& localPath) const;
    std::string hashURL(const std::string& url) const;
    uint64_t getFileSize(const std::string& path) const;

    std::string m_cacheDir;
    uint64_t m_maxCacheSize;
    std::atomic<uint64_t> m_currentCacheSize;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_shuttingDown;
    mutable std::mutex m_cacheMutex;
    std::condition_variable m_cacheCondition;
    std::unordered_map<std::string, CacheEntry> m_cache;
    std::vector<std::future<void>> m_asyncTasks;
};

} // namespace dms
