#include "common/VideoCacheManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    if (!output) throw std::runtime_error("cannot write test file");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}

int main() {
    using namespace dms;
    const auto base = std::filesystem::temp_directory_path() /
        ("dms-cache-tests-" + std::to_string(static_cast<long long>(getpid())));
    const auto sourceDir = base / "source";
    const auto cacheDir = base / "cache";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(sourceDir);
    writeFile(sourceDir / "one.mp4", "abcdef");
    writeFile(sourceDir / "two.mp4", "ghijkl");
    writeFile(sourceDir / "large.mp4", "01234567890");
    writeFile(sourceDir / "empty.mp4", "");

    try {
        VideoCacheManager cache;
        require(!cache.initialize(nullptr, 10), "null cache directory rejection");
        require(!cache.initialize("", 10), "empty cache directory rejection");
        require(!cache.initialize(cacheDir.string().c_str(), 0), "zero cache capacity rejection");
        require(cache.initialize(cacheDir.string().c_str(), 10), "cache initialization");

        std::string cachedPath;
        const std::string sourceOne = (sourceDir / "one.mp4").string();
        require(cache.getVideo(sourceOne, cachedPath), "local file cache copy");
        require(cachedPath != sourceOne && readFile(cachedPath) == "abcdef", "cached local file content");
        require(cache.isCached(sourceOne), "local file cached state");

        std::string concurrentA;
        std::string concurrentB;
        auto first = std::async(std::launch::async, [&] { return cache.getVideo(sourceOne, concurrentA); });
        auto second = std::async(std::launch::async, [&] { return cache.getVideo(sourceOne, concurrentB); });
        require(first.get() && second.get() && concurrentA == concurrentB, "same URL concurrent cache access");

        cache.clearCache();
        require(cache.getCacheEntryCount() == 0 && cache.getCurrentCacheSize() == 0,
                "clear cache state");
        const std::string fileUrl = "file://" + sourceOne;
        require(cache.getVideo(fileUrl, cachedPath) && readFile(cachedPath) == "abcdef", "file URL cache copy");
        cache.clearCache();

        require(cache.preloadVideo(sourceOne), "preload submission");
        for (int attempt = 0; attempt < 100 && !cache.isCached(sourceOne); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(cache.isCached(sourceOne), "preload completion");
        require(cache.getCurrentCacheSize() == 6, "preload cache size");

        const std::string sourceTwo = (sourceDir / "two.mp4").string();
        require(cache.getVideo(sourceTwo, cachedPath), "second local file cache copy");
        require(cache.isCached(sourceTwo) && !cache.isCached(sourceOne), "LRU eviction");
        require(cache.getCurrentCacheSize() <= 10, "cache capacity enforcement");
        require(!cache.getVideo((sourceDir / "large.mp4").string(), cachedPath), "oversize file rejection");
        require(!cache.getVideo((sourceDir / "empty.mp4").string(), cachedPath), "empty file rejection");
        require(!cache.getVideo((sourceDir / "missing.mp4").string(), cachedPath), "missing file rejection");
        require(!cache.getVideo("", cachedPath), "empty URL rejection");
        cache.shutdown();

        VideoCacheManager restarted;
        require(restarted.initialize(cacheDir.string().c_str(), 10), "cache restart initialization");
        require(restarted.getVideo(sourceTwo, cachedPath) && readFile(cachedPath) == "ghijkl",
                "cache restart URL reassociation");
        restarted.shutdown();
    } catch (...) {
        std::filesystem::remove_all(base);
        throw;
    }

    std::filesystem::remove_all(base);
    std::cout << "cache_tests passed\n";
}
