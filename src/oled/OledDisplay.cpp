#include "oled/OledDisplay.h"
#include "common/Logger.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dms {

namespace {

// HVIDEO J2: 1=GND, 2=SCL, 3=SDA, 4=POWER_EN, 5=LCD_RST, 6=VCC_3V3.
// J2 connects to the motherboard I2C_TP header, not the DSI panel GPIOs.
// LCD_RST uses I2C_TP RST = GPIO0_B6, verified by a GPIO-only reset test.
// I2C_TP INT is not a verified controllable VBAT enable: do not drive it.
constexpr int OLED_RESET_GPIO = 14;
constexpr useconds_t OLED_RESET_HOLD_US = 200000;
constexpr useconds_t OLED_RESET_RELEASE_SETTLE_US = 100000;

bool writeSysfs(const char* path, const char* value) {
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const size_t length = std::strlen(value);
    const bool written = ::write(fd, value, length) == static_cast<ssize_t>(length);
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    return written;
}

bool exportGpio(int gpio, char* gpioPath, size_t gpioPathSize) {
    std::snprintf(gpioPath, gpioPathSize, "/sys/class/gpio/gpio%d", gpio);
    if (::access(gpioPath, F_OK) == 0) {
        return true;
    }

    char number[16];
    std::snprintf(number, sizeof(number), "%d", gpio);
    if (!writeSysfs("/sys/class/gpio/export", number) && errno != EBUSY) {
        return false;
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::access(gpioPath, F_OK) == 0) {
            return true;
        }
        usleep(10000);
    }
    errno = ENOENT;
    return false;
}

bool setGpioDirection(int gpio, const char* direction) {
    char gpioPath[64];
    if (!exportGpio(gpio, gpioPath, sizeof(gpioPath))) {
        return false;
    }
    char directionPath[80];
    std::snprintf(directionPath, sizeof(directionPath), "%s/direction", gpioPath);
    return writeSysfs(directionPath, direction);
}

bool setGpioValue(int gpio, bool high) {
    char gpioPath[64];
    if (!exportGpio(gpio, gpioPath, sizeof(gpioPath))) {
        return false;
    }
    char valuePath[80];
    std::snprintf(valuePath, sizeof(valuePath), "%s/value", gpioPath);
    return writeSysfs(valuePath, high ? "1" : "0");
}

bool prepareHardware() {
    bool ready = true;
    // Hold the actual panel reset while its externally supplied rails settle.
    if (!setGpioDirection(OLED_RESET_GPIO, "low") ||
        !setGpioValue(OLED_RESET_GPIO, false)) {
        LOG_WARNING("OledDisplay: cannot assert LCD_RST on GPIO%d: %s",
                    OLED_RESET_GPIO, std::strerror(errno));
        ready = false;
    }

    // Hardware reset disables emission; keep display off until RAM is cleared.
    usleep(OLED_RESET_HOLD_US);
    if (ready && !setGpioValue(OLED_RESET_GPIO, true)) {
        LOG_WARNING("OledDisplay: cannot release LCD_RST on GPIO%d: %s",
                    OLED_RESET_GPIO, std::strerror(errno));
        ready = false;
    }
    usleep(OLED_RESET_RELEASE_SETTLE_US);
    return ready;
}

} // namespace

// 6x8 ASCII source font, characters 0x20-0x7E.
static const uint8_t FONT6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // #
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // $
    {0x23,0x13,0x08,0x64,0x62,0x00}, // %
    {0x36,0x49,0x55,0x22,0x50,0x00}, // &
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // *
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // +
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08,0x00}, // -
    {0x00,0x60,0x60,0x00,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02,0x00}, // /
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46,0x00}, // 2
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // 3
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // 4
    {0x27,0x45,0x45,0x45,0x39,0x00}, // 5
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // 6
    {0x01,0x71,0x09,0x05,0x03,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 8
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // 9
    {0x00,0x36,0x36,0x00,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14,0x00}, // =
    {0x41,0x22,0x14,0x08,0x00,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06,0x00}, // ?
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // @
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // A
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // B
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // C
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // D
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // E
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // F
    {0x3E,0x41,0x41,0x49,0x7A,0x00}, // G
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // H
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // J
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // K
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // L
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, // M
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // N
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // O
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // P
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // Q
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // R
    {0x46,0x49,0x49,0x49,0x31,0x00}, // S
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // T
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // U
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // V
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // W
    {0x63,0x14,0x08,0x14,0x63,0x00}, // X
    {0x07,0x08,0x70,0x08,0x07,0x00}, // Y
    {0x61,0x51,0x49,0x45,0x43,0x00}, // Z
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20,0x00}, // backslash
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04,0x00}, // ^
    {0x40,0x40,0x40,0x40,0x40,0x00}, // _
    {0x00,0x01,0x02,0x04,0x00,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78,0x00}, // a
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // b
    {0x38,0x44,0x44,0x44,0x20,0x00}, // c
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // d
    {0x38,0x54,0x54,0x54,0x18,0x00}, // e
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // f
    {0x08,0x54,0x54,0x54,0x3C,0x00}, // g
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // h
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // m
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // n
    {0x38,0x44,0x44,0x44,0x38,0x00}, // o
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // p
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // q
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // r
    {0x48,0x54,0x54,0x54,0x20,0x00}, // s
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // t
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // u
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // v
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // w
    {0x44,0x28,0x10,0x28,0x44,0x00}, // x
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // y
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // z
    {0x00,0x08,0x36,0x41,0x00,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00,0x00}, // }
    {0x08,0x04,0x08,0x10,0x08,0x00}, // ~
};

// SZCLX091-2832TSWFG02-H14 initialization for its internal DC/DC circuit.
// Hardware reset and display-off keep emission disabled during setup and clear.
static const uint8_t SSD1306_INIT_CMDS[] = {
    0xAE,       // display off
    0xD5,0x80,  // set display clock div
    0xA8,0x1F,  // set multiplex (32 rows)
    0xD3,0x00,  // set display offset
    0x40,       // set start line 0
    0x8D,0x14,  // charge pump on
    0x20,0x00,  // horizontal addressing with explicit column/page windows
    0xA1,       // segment remap
    0xC8,       // COM scan direction
    0xDA,0x00,  // sequential COM pins specified by the panel vendor
    0x81,0x8F,  // panel vendor contrast
    0xD9,0x1F,  // panel vendor pre-charge period
    0xDB,0x40,  // VCOMH deselect
    0x2E,       // deactivate scrolling before writing the frame
    0xA4,       // display RAM
    0xA7,       // inverse display: white background, black pixels
};

OledDisplay::OledDisplay(int bus, uint8_t addr)
    : m_fd(-1), m_addr(addr), m_bus(bus) {}

OledDisplay::~OledDisplay() {
    close();
}

bool OledDisplay::open() {
    if (m_fd >= 0) return true;
    m_pageValid.fill(false);
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", m_bus);
    m_fd = ::open(path, O_RDWR);
    if (m_fd < 0) {
        LOG_WARNING("OledDisplay: cannot open %s: %s", path, std::strerror(errno));
        return false;
    }
    if (ioctl(m_fd, I2C_SLAVE, m_addr) < 0) {
        LOG_WARNING("OledDisplay: ioctl I2C_SLAVE 0x%02X failed: %s", m_addr, std::strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    // Retry initialization because the panel may not acknowledge immediately at boot.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!prepareHardware()) {
            LOG_WARNING("OledDisplay: GPIO preparation failed; keeping display disabled");
        } else if (init()) {
            LOG_INFO("OledDisplay: opened /dev/i2c-%d addr=0x%02X", m_bus, m_addr);
            return true;
        }
        // A partial init must never survive the retry delay with emission on.
        setGpioDirection(OLED_RESET_GPIO, "low");
        usleep(500000);
    }
    LOG_WARNING("OledDisplay: init failed after retries on /dev/i2c-%d", m_bus);
    ::close(m_fd);
    m_fd = -1;
    return false;
}

void OledDisplay::close() {
    m_pageValid.fill(false);
    if (m_fd >= 0) {
        sendCommand(0xAE);
        const uint8_t pumpOff[] = {0x8D, 0x10};
        sendCommands(pumpOff, sizeof(pumpOff));
        usleep(100000);
        setGpioDirection(OLED_RESET_GPIO, "low");
        ::close(m_fd);
        m_fd = -1;
    }
}

void OledDisplay::handoffBootDisplay() {
    // The early boot invocation and Slave use the same implementation and run
    // sequentially. Leave a fully initialized boot screen visible until Slave
    // takes ownership; no second daemon or raw I2C writer remains running.
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_pageValid.fill(false);
}

bool OledDisplay::sendCommand(uint8_t cmd) {
    return sendCommands(&cmd, 1);
}

bool OledDisplay::sendCommands(const uint8_t* commands, size_t count) {
    if (m_fd < 0 || commands == nullptr || count == 0 || count > 31) return false;
    // Keep each command and its parameters in the same I2C transaction.
    uint8_t packet[32] = {0x00}; // Co=0, D/C#=0
    std::memcpy(packet + 1, commands, count);
    return write(m_fd, packet, count + 1) == static_cast<ssize_t>(count + 1);
}

bool OledDisplay::setWindow(uint8_t firstPage, uint8_t lastPage) {
    const uint8_t commands[] = {0x21, 0, 127, 0x22, firstPage, lastPage};
    return sendCommands(commands, sizeof(commands));
}

bool OledDisplay::sendData(const uint8_t* buf, int len) {
    if (m_fd < 0 || buf == nullptr || len <= 0) return false;

    // Keep each transfer below the small FIFO limit of RK3566 I2C adapters.
    // The SSD1306 keeps its page/column cursor across I2C transactions, so
    // splitting a page write is transparent to the controller.
    constexpr int MAX_TRANSFER_DATA = 16;
    while (len > 0) {
        const int chunk = std::min(len, MAX_TRANSFER_DATA);
        uint8_t packet[MAX_TRANSFER_DATA + 1];
        packet[0] = 0x40; // Co=0, D/C#=1
        std::memcpy(packet + 1, buf, static_cast<size_t>(chunk));
        if (write(m_fd, packet, static_cast<size_t>(chunk + 1)) != chunk + 1) {
            return false;
        }
        buf += chunk;
        len -= chunk;
    }
    return true;
}

bool OledDisplay::init() {
    if (!sendCommands(SSD1306_INIT_CMDS, sizeof(SSD1306_INIT_CMDS))) {
        LOG_WARNING("OledDisplay: init command sequence failed: %s", std::strerror(errno));
        return false;
    }
    if (!clear()) {
        return false;
    }
    usleep(100000); // allow the charge pump to settle before enabling emission
    if (!sendCommand(0xAF)) {
        LOG_WARNING("OledDisplay: display-on command failed: %s", std::strerror(errno));
        return false;
    }
    usleep(100000); // allow the first clean white frame to settle
    LOG_INFO("OledDisplay: GPIO14 reset complete; RAM cleared before display-on");
    return true;
}

bool OledDisplay::clear() {
    if (m_fd < 0) return false;
    static const uint8_t zeros[128] = {};
    // The controller resets in 128x64 mode. Clear all eight RAM pages before
    // enabling emission, including pages outside this panel's 32 visible rows.
    if (!setWindow(0, 7)) return false;
    for (uint8_t page = 0; page < 8; ++page) {
        if (!sendData(zeros, 128)) {
            LOG_WARNING("OledDisplay: clear failed on page %u: %s", page, std::strerror(errno));
            return false;
        }
    }
    for (auto& page : m_pages) page.fill(0);
    m_pageValid.fill(true);
    return true;
}

bool OledDisplay::showText(uint8_t row, uint8_t col, const std::string& text) {
    constexpr uint8_t GLYPH_WIDTH = 6;
    if (m_fd < 0 || row >= 4 || col >= 128) return false;

    uint8_t line[128] = {};
    uint8_t x = col;
    for (char c : text) {
        if (x + GLYPH_WIDTH > 128) break;
        uint8_t idx = static_cast<uint8_t>(c);
        if (idx < 0x20 || idx > 0x7E) idx = 0x20;
        const uint8_t* glyph = FONT6x8[idx - 0x20];
        for (uint8_t glyphColumn = 0; glyphColumn < GLYPH_WIDTH; ++glyphColumn) {
            line[x + glyphColumn] = glyph[glyphColumn];
        }
        x = static_cast<uint8_t>(x + GLYPH_WIDTH);
    }

    if (m_pageValid[row] && std::equal(std::begin(line), std::end(line),
                                     m_pages[row].begin())) return true;
    // Cache only successful writes. On an I2C error, hide the partial frame
    // immediately and require the complete reset/init sequence on the retry.
    if (!setWindow(row, row) || !sendData(line, 128)) {
        LOG_WARNING("OledDisplay: page update failed; disabling incomplete display");
        close();
        return false;
    }
    std::copy(std::begin(line), std::end(line), m_pages[row].begin());
    m_pageValid[row] = true;
    return true;
}

bool OledDisplay::showSystemInfo(const std::string& sn,
                                 const std::string& ip, const std::string& txMbps,
                                 const std::string& rxMbps,
                                 uint32_t nodeId,
                                 const std::string& nodeRole) {
    if (m_fd < 0) return false;
    char nodeIdText[16];
    std::snprintf(nodeIdText, sizeof(nodeIdText), "%03u", nodeId);
    if (!showText(0, 0, "SN:" + sn)) return false;
    if (!showText(1, 0, "ID:" + std::string(nodeIdText) + " " + nodeRole)) return false;
    if (!showText(2, 0, "IP:" + ip)) return false;
    return showText(3, 0, "TX:" + txMbps + "RX:" + rxMbps + "Mbps");
}

} // namespace dms
