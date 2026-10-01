#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>

static std::map<uintptr_t, uint32_t> regs;
static unsigned writes;
static bool stuckHigh;

static void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

static uint32_t readl(const void* address) {
    const auto a = reinterpret_cast<uintptr_t>(address);
    if (a == 0xfdd60070) {
        return stuckHigh ? 0x4000 : regs.at(0xfdd60000);
    }
    return regs.at(a);
}

static void writel(uint32_t value, void* address) {
    const auto a = reinterpret_cast<uintptr_t>(address);
    const uint32_t mask = value >> 16;
    require(regs.count(a) == 1, "write outside OLED reset registers");
    if (a == 0xfdd60008) {
        require(!(regs.at(0xfdd60000) & 0x4000), "output enabled before low preload");
        require(!(regs.at(0xfdd00180) & 4) && !(regs.at(0xfdd00184) & 0x200),
                "GPIO bus clocks still gated");
    }
    if (a == 0xfdc2000c) {
        require((regs.at(0xfdd60008) & 0x4000) && !(regs.at(0xfdd60000) & 0x4000),
                "pin mux selected before reset output ready");
    }
    regs[a] = (regs[a] & ~mask) | (value & mask);
    ++writes;
}

#include "../system/firefly-rk356x/uboot/dms-oled-guard.h"

int main() {
    try {
        for (uint32_t initial : {0u, 0xffffu, 0xaaaau, 0x5555u}) {
            regs = {{0xfdd00180, initial}, {0xfdd00184, initial},
                    {0xfdd60000, initial}, {0xfdd60008, initial}, {0xfdc2000c, initial}};
            stuckHigh = false;
            writes = 0;
            dms_oled_early_reset();
            require(dms_oled_reset_is_held(), "reset not asserted");
            require(regs.at(0xfdd00180) == (initial & ~4u), "unrelated PMU clock changed");
            require(regs.at(0xfdd00184) == (initial & ~0x200u), "unrelated GPIO clock changed");
            require(regs.at(0xfdd60000) == (initial & ~0x4000u), "unrelated GPIO output changed");
            require(regs.at(0xfdd60008) == (initial | 0x4000u), "unrelated GPIO direction changed");
            require(regs.at(0xfdc2000c) == (initial & ~0x0f00u), "unrelated pin mux changed");
            require(writes == 5, "unexpected early writes");
            const auto held = regs;
            dms_oled_early_reset();
            require(regs == held, "reassertion changes the held state");
            writes = 0;
            dms_oled_boot_guard();
            require(writes == 0, "late report should preserve early reset");
            stuckHigh = true;
            require(!dms_oled_reset_is_held(), "stuck-high pad reported as reset");
            stuckHigh = false;
            regs[0xfdd60008] &= ~0x4000u;
            require(!dms_oled_reset_is_held(), "input pin reported as driven reset");
            dms_oled_boot_guard();
            require(dms_oled_reset_is_held(), "late guard failed to reassert reset");
        }
        std::puts("OLED early reset: masked writes, clock/order, pin isolation and readback passed");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return EXIT_FAILURE;
    }
}
