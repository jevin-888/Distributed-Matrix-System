#pragma once

#include <cstdint>
#include <array>
#include <string>

namespace dms {

// 128x32 SSD1306 OLED via Linux I2C (/dev/i2c-X), address 0x3C
class OledDisplay {
public:
    static constexpr int DEFAULT_I2C_BUS = 1;
    static constexpr uint8_t DEFAULT_I2C_ADDR = 0x3C;

    explicit OledDisplay(int bus = DEFAULT_I2C_BUS, uint8_t addr = DEFAULT_I2C_ADDR);
    ~OledDisplay();

    OledDisplay(const OledDisplay&) = delete;
    OledDisplay& operator=(const OledDisplay&) = delete;

    bool open();
    void close();
    // Only the early boot invocation transfers a complete, visible boot frame.
    void handoffBootDisplay();
    bool isOpen() const { return m_fd >= 0; }

    bool clear();
    // row 0..3 (each row = 8 px), col 0..127; text is rendered at 6x8.
    bool showText(uint8_t row, uint8_t col, const std::string& text);
    // Display system information on the four pages of the 128x32 panel.
    bool showSystemInfo(const std::string& sn,
                        const std::string& ip, const std::string& txMbps,
                        const std::string& rxMbps,
                        uint32_t nodeId = 0,
                        const std::string& nodeRole = "OUT");

private:
    bool sendCommand(uint8_t cmd);
    bool sendCommands(const uint8_t* commands, size_t count);
    bool setWindow(uint8_t firstPage, uint8_t lastPage);
    bool sendData(const uint8_t* buf, int len);
    bool init();

    int m_fd;
    uint8_t m_addr;
    int m_bus;
    std::array<std::array<uint8_t, 128>, 4> m_pages{};
    std::array<bool, 4> m_pageValid{};
};

} // namespace dms
