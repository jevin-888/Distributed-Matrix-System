#include "oled/OledDisplay.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
constexpr int PANEL_FD = 900;
std::map<int, std::string> paths;
int nextFd = 1000;
bool resetHigh = false;
bool visible = false;
bool failGpio = false;
int failAfter = -1;
int writes = 0;
int clearedBytes = 0;
int page = 0;
int column = 0;
int firstColumn = 0, lastColumn = 127, firstPage = 0, lastPage = 7;
bool horizontal = false, scrollDisabled = false, windowSet = false;
std::array<uint8_t, 1024> ram;
std::array<bool, 1024> initialized;
bool unsafeDisplay = false;
std::vector<unsigned> delays;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void resetSimulation() {
    paths.clear();
    resetHigh = visible = failGpio = unsafeDisplay = false;
    failAfter = -1;
    writes = clearedBytes = page = column = 0;
    horizontal = scrollDisabled = windowSet = false;
    ram.fill(0xA5);
    initialized.fill(false);
    delays.clear();
}
}

extern "C" {
int __real_open(const char*, int, ...);
ssize_t __real_write(int, const void*, size_t);
int __real_close(int);
int __real_access(const char*, int);

int __wrap_open(const char* path, int flags, ...) {
    if (std::strcmp(path, "/dev/i2c-1") == 0) return PANEL_FD;
    if (std::strncmp(path, "/sys/class/gpio/", 16) == 0) {
        const int fd = nextFd++;
        paths[fd] = path;
        return fd;
    }
    return __real_open(path, flags, 0600);
}

int __wrap_access(const char* path, int mode) {
    if (std::strncmp(path, "/sys/class/gpio/", 16) == 0) return 0;
    return __real_access(path, mode);
}

int __wrap_close(int fd) {
    if (fd == PANEL_FD) return 0;
    if (paths.erase(fd)) return 0;
    return __real_close(fd);
}

int __wrap_ioctl(int, unsigned long, ...) { return 0; }
int __wrap_usleep(useconds_t us) { delays.push_back(us); return 0; }

ssize_t __wrap_write(int fd, const void* data, size_t size) {
    const auto found = paths.find(fd);
    if (found != paths.end()) {
        if (failGpio) { errno = EIO; return -1; }
        const std::string value(static_cast<const char*>(data), size);
        if (found->second.find("gpio14/") != std::string::npos) {
            resetHigh = value == "high" || value == "1";
            if (!resetHigh) {
                visible = false;
                clearedBytes = 0;
                horizontal = scrollDisabled = windowSet = false;
                initialized.fill(false);
            }
        } else {
            require(found->second == "/sys/class/gpio/export", "unexpected GPIO: wrong connector mapping");
        }
        return static_cast<ssize_t>(size);
    }
    if (fd != PANEL_FD) return __real_write(fd, data, size);
    if (failAfter >= 0 && writes >= failAfter) { errno = EIO; return -1; }
    ++writes;
    const auto bytes = static_cast<const unsigned char*>(data);
    if (bytes[0] == 0x40) {
        require(horizontal && scrollDisabled && windowSet, "data sent without horizontal window");
        for (size_t i = 1; i < size; ++i) {
            const size_t address = page * 128 + column;
            ram[address] = bytes[i];
            if (!visible) {
                require(bytes[i] == 0, "initial frame contains random pixels");
                require(!initialized[address], "clear rewrote same address and missed RAM");
                initialized[address] = true;
                ++clearedBytes;
            }
            if (++column > lastColumn) {
                column = firstColumn;
                if (++page > lastPage) page = firstPage;
            }
        }
    } else if (bytes[0] == 0) {
        for (size_t i = 1; i < size; ++i) {
            const auto command = bytes[i];
            if (command == 0xAE) visible = false;
            if (command == 0xAF) {
                if (!resetHigh || clearedBytes != 1024 ||
                    !std::all_of(ram.begin(), ram.end(), [](uint8_t v) { return v == 0; }))
                    unsafeDisplay = true;
                visible = true;
            }
            if (command == 0x2E) scrollDisabled = true;
            int parameters = 0;
            if (command == 0x21 || command == 0x22) parameters = 2;
            else if (command == 0xD5 || command == 0xA8 || command == 0xD3 ||
                     command == 0x8D || command == 0x20 || command == 0xDA ||
                     command == 0x81 || command == 0xD9 || command == 0xDB) parameters = 1;
            require(i + parameters < size, "command parameters split across I2C transactions");
            if (command == 0x20) horizontal = bytes[i + 1] == 0;
            if (command == 0x21) {
                firstColumn = column = bytes[i + 1]; lastColumn = bytes[i + 2];
                require(firstColumn <= lastColumn && lastColumn < 128, "bad column window");
                windowSet = false;
            }
            if (command == 0x22) {
                firstPage = page = bytes[i + 1]; lastPage = bytes[i + 2];
                require(firstPage <= lastPage && lastPage < 8, "bad page window");
                windowSet = true;
            }
            require(command < 0xB0 || command > 0xB7, "legacy page addressing used");
            i += parameters;
        }
    }
    return static_cast<ssize_t>(size);
}
}

int main() {
    try {
        int initializationWrites = 0;
        resetSimulation();
        {
            dms::OledDisplay display;
            require(display.open(), "cold init failed");
            require(visible && resetHigh, "valid white frame not enabled");
            require(!unsafeDisplay, "display enabled before clear");
            require(clearedBytes == 1024 && std::all_of(initialized.begin(), initialized.end(), [](bool v) { return v; }), "full controller RAM not cleared");
            const int initialWrites = initializationWrites = writes;
            require(display.open() && writes == initialWrites, "open unnecessarily resets display");
            require(display.showSystemInfo("123", "192.168.2.107", "0", "0", 1, "OUT"), "status failed");
            const int frameWrites = writes;
            require(display.showSystemInfo("123", "192.168.2.107", "0", "0", 1, "OUT"), "cached status failed");
            require(writes == frameWrites, "unchanged screen rewritten");
            const auto previousRam = ram;
            require(display.showSystemInfo("123", "192.168.2.108", "0", "0", 1, "OUT"), "IP update failed");
            require(writes == frameWrites + 9, "IP update changed other rows");
            for (size_t i = 0; i < ram.size(); ++i) {
                if (i < 256 || i >= 384) require(ram[i] == previousRam[i], "IP update corrupted another row");
            }
            require(!std::equal(ram.begin() + 256, ram.begin() + 384, previousRam.begin() + 256), "IP row did not change");
            require(delays.size() >= 4 && delays[0] >= 200000 &&
                    delays[delays.size() - 1] >= 100000, "missing reset settling delay");
        }
        require(!visible && !resetHigh, "shutdown did not fail closed");

        // Exercise every command/data transfer in initialization. A permanent
        // transport error at any point must prevent panel emission on all retries.
        for (int failure = 0; failure < initializationWrites; ++failure) {
            resetSimulation();
            failAfter = failure;
            dms::OledDisplay display;
            require(!display.open(), "failed initialization incorrectly succeeded");
            require(!visible && !resetHigh, "failed init left panel enabled");
            require(!unsafeDisplay, "failure exposed uncleared RAM");
        }
        resetSimulation();
        {
            failGpio = true;
            dms::OledDisplay display;
            require(!display.open() && writes == 0, "GPIO failure still sent display commands");
        }
        resetSimulation();
        {
            dms::OledDisplay display;
            require(display.open(), "page failure setup failed");
            failAfter = writes + 4;
            require(!display.showText(0, 0, "Changed"), "partial write not reported");
            require(!display.isOpen() && !visible && !resetHigh,
                    "partial status frame remained visible");
            failAfter = -1;
            require(display.open() && display.showText(0, 0, "Changed"), "recovery failed");
        }
        resetSimulation();
        {
            dms::OledDisplay display;
            require(display.open() && display.showText(1, 28, "system login"), "boot display failed");
            require(std::all_of(ram.begin(), ram.begin() + 128, [](uint8_t v) { return v == 0; }), "boot text leaked into first row");
            require(std::any_of(ram.begin() + 156, ram.begin() + 228, [](uint8_t v) { return v != 0; }), "boot text missing from second row");
            require(std::all_of(ram.begin() + 128, ram.begin() + 156, [](uint8_t v) { return v == 0; }) &&
                    std::all_of(ram.begin() + 228, ram.begin() + 256, [](uint8_t v) { return v == 0; }), "boot text not centered");
            display.handoffBootDisplay();
        }
        require(visible, "boot screen was blanked before handoff");
        std::puts("OLED GPIO14 reset sequencing, fail-closed recovery, cached refresh and boot handoff passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "OLED test failed: %s\n", error.what());
        return 1;
    }
}
