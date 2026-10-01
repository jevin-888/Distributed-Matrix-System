/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef DMS_OLED_GUARD_H
#define DMS_OLED_GUARD_H

/* RK3566/3568 register layout is defined by grf_rk3568.h, gpio.h (GPIO_V2),
 * cru_rk3568.h and the kernel clk-rk3568.c PCLK_GPIO0 gate definition.
 * Use masked MMIO before driver-model initialization: gpio_request() is too
 * late for this job. Only GPIO0_B6 and its bus clocks are changed.
 * The vendor DDR/SPL loader runs BEFORE this code and is not changed here.
 */
#define DMS_OLED_PMU_BUS_GATE ((void *)0xfdd00180UL)
#define DMS_OLED_GPIO_GATE    ((void *)0xfdd00184UL)
#define DMS_OLED_GPIO_DR      ((void *)0xfdd60000UL)
#define DMS_OLED_GPIO_DDR     ((void *)0xfdd60008UL)
#define DMS_OLED_GPIO_EXT     ((void *)0xfdd60070UL)
#define DMS_OLED_GPIO_MUX     ((void *)0xfdc2000cUL)

static void dms_oled_early_reset(void)
{
	/* Enable PCLK_PDPMU and PCLK_GPIO0, preserving all other gate bits. */
	writel(0x00040000, DMS_OLED_PMU_BUS_GATE);
	writel(0x02000000, DMS_OLED_GPIO_GATE);
	/* Preload low, enable output, then select GPIO: never pulse RES# high. */
	writel(0x40000000, DMS_OLED_GPIO_DR);
	writel(0x40004000, DMS_OLED_GPIO_DDR);
	writel(0x0f000000, DMS_OLED_GPIO_MUX);
	(void)readl(DMS_OLED_GPIO_MUX);
}

static int dms_oled_reset_is_held(void)
{
	return !(readl(DMS_OLED_GPIO_MUX) & 0x0f00) &&
	       (readl(DMS_OLED_GPIO_DDR) & 0x4000) &&
	       !(readl(DMS_OLED_GPIO_DR) & 0x4000) &&
	       !(readl(DMS_OLED_GPIO_EXT) & 0x4000);
}

static void dms_oled_boot_guard(void)
{
	/* Report only after the console is ready; never release reset here. */
	if (!dms_oled_reset_is_held()) {
		printf("DMS OLED: early reset not held; reasserting GPIO14\n");
		dms_oled_early_reset();
	}
	if (dms_oled_reset_is_held())
		printf("DMS OLED: GPIO14 held low; early guard in arch_cpu_init\n");
	else
		printf("DMS OLED: GPIO14 reset readback failed\n");
}

#endif
