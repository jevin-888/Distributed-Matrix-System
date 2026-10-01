// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Rockchip Electronics Co. Ltd.
 *
 * Author: Shunqing Chen <csq@rock-chips.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/soc/rockchip/rk_vendor_storage.h>
#include <linux/slab.h>

#include "rk628.h"
#include "rk628_combrxphy.h"
#include "rk628_cru.h"
#include "rk628_hdmirx.h"

#define INIT_FIFO_STATE			64
#define RK628_AUDIO_FIFO_START		64
#define RK628_AUDIO_FIFO_MAX		8
#define RK628_AUDIO_FIFO_MIN		8
#define HDMI_RX_AUD_SPARE		(HDMI_RX_BASE + 0x0268)
#define AUDS_MAS_SAMPLE_FLAT		GENMASK(7, 4)
#define DEFAULT_AUDIO_CLK		5644800

#define is_validfs(x) (x == 32000 || \
			x == 44100 || \
			x == 48000 || \
			x == 88200 || \
			x == 96000 || \
			x == 176400 || \
			x == 192000 || \
			x == 768000)

struct rk628_audiostate {
	u32 hdmirx_aud_clkrate;
	u32 fs_audio;
	u32 ctsn_flag;
	u32 fifo_flag;
	int init_state;
	int pre_state;
	bool fifo_int;
	bool audio_enable;
};

struct rk628_audioinfo {
	struct delayed_work delayed_work_audio_rate_change;
	struct delayed_work delayed_work_audio;
	struct mutex *confctl_mutex;
	struct rk628 *rk628;
	struct rk628_audiostate audio_state;
	bool i2s_enabled_default;
	bool i2s_enabled;
	bool sample_flat;
	int debug;
	bool fifo_ints_en;
	bool ctsn_ints_en;
	bool audio_present;
	int stablelimit;
	int stablecount;
	struct device *dev;
};

struct hdmirx_tmdsclk_cnt {
	u32 tmds_cnt;
	u8  cnt;
};

#define HDMIRX_GET_TMDSCLK_TIME		21

static int hdcp_load_keys_cb(struct rk628 *rk628, struct rk628_hdcp *hdcp)
{
	int size;
	u8 hdcp_vendor_data[320];

	hdcp->keys = kmalloc(HDCP_KEY_SIZE, GFP_KERNEL);
	if (!hdcp->keys)
		return -ENOMEM;

	hdcp->seeds = kmalloc(HDCP_KEY_SEED_SIZE, GFP_KERNEL);
	if (!hdcp->seeds) {
		kfree(hdcp->keys);
		hdcp->keys = NULL;
		return -ENOMEM;
	}

	size = rk_vendor_read(HDMIRX_HDCP1X_ID, hdcp_vendor_data, 314);
	if (size < (HDCP_KEY_SIZE + HDCP_KEY_SEED_SIZE)) {
		dev_dbg(rk628->dev, "HDCP: read size %d\n", size);
		kfree(hdcp->keys);
		hdcp->keys = NULL;
		kfree(hdcp->seeds);
		hdcp->seeds = NULL;
		return -EINVAL;
	}
	memcpy(hdcp->keys, hdcp_vendor_data, HDCP_KEY_SIZE);
	memcpy(hdcp->seeds, hdcp_vendor_data + HDCP_KEY_SIZE,
	       HDCP_KEY_SEED_SIZE);

	return 0;
}

static int rk628_hdmi_hdcp_load_key(struct rk628 *rk628, struct rk628_hdcp *hdcp)
{
	int i;
	int ret;
	struct hdcp_keys *hdcp_keys;
	u32 seeds = 0;

	if (!hdcp->keys) {
		ret = hdcp_load_keys_cb(rk628, hdcp);
		if (ret) {
			dev_err(rk628->dev, "HDCP: load key failed\n");
			return ret;
		}
	}
	hdcp_keys = hdcp->keys;

	rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_CTRL,
			HDCP_ENABLE_MASK |
			HDCP_ENC_EN_MASK,
			HDCP_ENABLE(0) |
			HDCP_ENC_EN(0));
	rk628_i2c_update_bits(rk628, GRF_SYSTEM_CON0,
			SW_ADAPTER_I2CSLADR_MASK |
			SW_EFUSE_HDCP_EN_MASK,
			SW_ADAPTER_I2CSLADR(0) |
			SW_EFUSE_HDCP_EN(1));
	/* The useful data in ksv should be 5 byte */
	for (i = 0; i < KSV_LEN; i++)
		rk628_i2c_write(rk628, HDCP_KEY_KSV0 + i * 4,
					hdcp_keys->KSV[i]);

	for (i = 0; i < HDCP_PRIVATE_KEY_SIZE; i++)
		rk628_i2c_write(rk628, HDCP_KEY_DPK0 + i * 4,
				hdcp_keys->devicekey[i]);

	rk628_i2c_update_bits(rk628, GRF_SYSTEM_CON0,
			SW_ADAPTER_I2CSLADR_MASK |
			SW_EFUSE_HDCP_EN_MASK,
			SW_ADAPTER_I2CSLADR(0) |
			SW_EFUSE_HDCP_EN(0));
	rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_CTRL,
			HDCP_ENABLE_MASK |
			HDCP_ENC_EN_MASK,
			HDCP_ENABLE(1) |
			HDCP_ENC_EN(1));

	/* Enable decryption logic */
	if (hdcp->seeds) {
		seeds = (hdcp->seeds[0] & 0xff) << 8;
		seeds |= (hdcp->seeds[1] & 0xff);
	}
	if (seeds) {
		rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_CTRL,
				   KEY_DECRIPT_ENABLE_MASK,
				   KEY_DECRIPT_ENABLE(1));
		rk628_i2c_write(rk628, HDMI_RX_HDCP_SEED, seeds);
	} else {
		rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_CTRL,
				   KEY_DECRIPT_ENABLE_MASK,
				   KEY_DECRIPT_ENABLE(0));
	}

	return 0;
}

void rk628_hdmirx_set_hdcp(struct rk628 *rk628, struct rk628_hdcp *hdcp, bool en)
{
	dev_dbg(rk628->dev, "%s: %sable\n", __func__, en ? "en" : "dis");

	if (en) {
		rk628_hdmi_hdcp_load_key(rk628, hdcp);
	} else {
		rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_CTRL,
				      HDCP_ENABLE_MASK |
				      HDCP_ENC_EN_MASK,
				      HDCP_ENABLE(0) |
				      HDCP_ENC_EN(0));
	}
}
EXPORT_SYMBOL(rk628_hdmirx_set_hdcp);

void rk628_hdmirx_controller_setup(struct rk628 *rk628)
{
	rk628_i2c_write(rk628, HDMI_RX_HDMI20_CONTROL, 0x10000f10);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_MODE_RECOVER, 0x00000021);
	rk628_i2c_write(rk628, HDMI_RX_PDEC_CTRL, 0xbfff8011);
	rk628_i2c_write(rk628, HDMI_RX_PDEC_ASP_CTRL, 0x00000040);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_RESMPL_CTRL, 0x00000001);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_SYNC_CTRL, 0x00000014);
	rk628_i2c_write(rk628, HDMI_RX_PDEC_ERR_FILTER, 0x00000008);
	rk628_i2c_write(rk628, HDMI_RX_SCDC_I2CCONFIG, 0x01000000);
	rk628_i2c_write(rk628, HDMI_RX_SCDC_CONFIG, 0x00000001);
	rk628_i2c_write(rk628, HDMI_RX_SCDC_WRDATA0, 0xabcdef01);
	rk628_i2c_write(rk628, HDMI_RX_CHLOCK_CONFIG, 0x0030c15c);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_ERROR_PROTECT, 0x000d0c98);
	rk628_i2c_write(rk628, HDMI_RX_MD_HCTRL1, 0x00000010);
	rk628_i2c_write(rk628, HDMI_RX_MD_HCTRL2, 0x00001738);
	rk628_i2c_write(rk628, HDMI_RX_MD_VCTRL, 0x00000002);
	rk628_i2c_write(rk628, HDMI_RX_MD_VTH, 0x0000073a);
	rk628_i2c_write(rk628, HDMI_RX_MD_IL_POL, 0x00000004);
	rk628_i2c_write(rk628, HDMI_RX_PDEC_ACRM_CTRL, 0x00000000);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_DCM_CTRL, 0x00040414);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_CKM_EVLTM, 0x00103e70);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_CKM_F, 0x0c1c0b54);
	rk628_i2c_write(rk628, HDMI_RX_HDMI_RESMPL_CTRL, 0x00000001);
	rk628_i2c_update_bits(rk628, HDMI_RX_HDMI_TIMER_CTRL, VIDEO_PERIOD_MASK, VIDEO_PERIOD(1));

	rk628_i2c_update_bits(rk628, HDMI_RX_HDCP_SETTINGS,
			      HDMI_RESERVED_MASK |
			      FAST_I2C_MASK |
			      ONE_DOT_ONE_MASK |
			      FAST_REAUTH_MASK,
			      HDMI_RESERVED(1) |
			      FAST_I2C(0) |
			      ONE_DOT_ONE(0) |
			      FAST_REAUTH(0));
}
EXPORT_SYMBOL(rk628_hdmirx_controller_setup);

static void rk628_hdmirx_audio_fifo_init(struct rk628_audioinfo *aif)
{
	int init_fifo_state = INIT_FIFO_STATE;

	dev_dbg(aif->dev, "%s initial fifo\n", __func__);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_ICLR, 0x1f);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10001);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10000);
	aif->audio_state.pre_state = aif->audio_state.init_state = init_fifo_state * 4;
}

static void rk628_hdmirx_audio_fifo_initd(struct rk628_audioinfo *aif)
{
	int init_fifo_state = INIT_FIFO_STATE;

	dev_dbg(aif->dev, "%s double initial fifo\n", __func__);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_ICLR, 0x1f);
	rk628_i2c_update_bits(aif->rk628, HDMI_RX_AUD_FIFO_TH,
			   AFIF_TH_START_MASK,
			   AFIF_TH_START(192));
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10001);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10000);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10001);
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_CTRL, 0x10000);
	rk628_i2c_update_bits(aif->rk628, HDMI_RX_AUD_FIFO_TH,
			   AFIF_TH_START_MASK,
			   AFIF_TH_START(init_fifo_state));
	aif->audio_state.pre_state = aif->audio_state.init_state = init_fifo_state * 4;
}

static int _rk628_hdmirx_audio_fs(struct rk628_audioinfo *aif,
					u32 *fs_audio)
{
	u64 tmdsclk = 0;
	u32 clkrate = 0, cts_decoded = 0, n_decoded = 0;
	int ret;

	*fs_audio = 0;
	/* fout=128*fs=ftmds*N/CTS */
	ret = rk628_i2c_read(aif->rk628, HDMI_RX_HDMI_CKM_RESULT, &clkrate);
	if (ret)
		return ret;
	clkrate &= 0xffff;
	/* tmdsclk = (clkrate/1000) * 49500000 */
	tmdsclk = clkrate * (49500000 / 1000);
	ret = rk628_i2c_read(aif->rk628, HDMI_RX_PDEC_ACR_CTS, &cts_decoded);
	if (ret)
		return ret;
	ret = rk628_i2c_read(aif->rk628, HDMI_RX_PDEC_ACR_N, &n_decoded);
	if (ret)
		return ret;
	if (cts_decoded != 0) {
		*fs_audio = div_u64((tmdsclk * n_decoded), cts_decoded);
		*fs_audio /= 128;
		*fs_audio = div_u64(*fs_audio + 50, 100);
		*fs_audio *= 100;
	}
	dev_dbg(aif->dev,
		"%s: clkrate:%u tmdsclk:%llu, n_decoded:%u, cts_decoded:%u, fs_audio:%u\n",
		__func__, clkrate, tmdsclk, n_decoded, cts_decoded, *fs_audio);
	if (!is_validfs(*fs_audio))
		return -EINVAL;
	return 0;
}

static void rk628_hdmirx_audio_clk_set_rate(struct rk628_audioinfo *aif, u32 rate)
{

	dev_dbg(aif->dev, "%s: %u to %u\n",
		 __func__, aif->audio_state.hdmirx_aud_clkrate, rate);
	rk628_clk_set_rate(aif->rk628, CGU_CLK_HDMIRX_AUD, rate);
	aif->audio_state.hdmirx_aud_clkrate = rate;
}

static void rk628_hdmirx_audio_clk_inc_rate(struct rk628_audioinfo *aif, int dis)
{
	u32 hdmirx_aud_clkrate = aif->audio_state.hdmirx_aud_clkrate + dis;

	dev_dbg(aif->dev, "%s: %u to %u\n",
		 __func__, aif->audio_state.hdmirx_aud_clkrate, hdmirx_aud_clkrate);
	rk628_clk_set_rate(aif->rk628, CGU_CLK_HDMIRX_AUD, hdmirx_aud_clkrate);
	aif->audio_state.hdmirx_aud_clkrate = hdmirx_aud_clkrate;
}

static void rk628_hdmirx_audio_clk_ppm_inc(struct rk628_audioinfo *aif, int ppm)
{
	int delta;
	u32 rate = aif->audio_state.hdmirx_aud_clkrate;
	bool decrease = ppm < 0;

	if (decrease)
		ppm = -ppm;
	delta = div_u64((u64)rate * ppm + 500000, 1000000);
	if (decrease)
		delta = -delta;
	rate += delta;
	dev_dbg(aif->dev, "%s: %u to %u (delta:%d ppm:%d)\n",
		__func__, aif->audio_state.hdmirx_aud_clkrate, rate,
		delta, decrease ? -ppm : ppm);
	rk628_clk_set_rate(aif->rk628, CGU_CLK_HDMIRX_AUD, rate);
	aif->audio_state.hdmirx_aud_clkrate = rate;
}

static void rk628_hdmirx_audio_set_fs(struct rk628_audioinfo *aif, u32 fs_audio)
{
	u32 hdmirx_aud_clkrate_t = fs_audio*128;

	dev_dbg(aif->dev, "%s: %u to %u with fs %u\n", __func__,
		 aif->audio_state.hdmirx_aud_clkrate, hdmirx_aud_clkrate_t,
		 fs_audio);
	rk628_clk_set_rate(aif->rk628, CGU_CLK_HDMIRX_AUD, hdmirx_aud_clkrate_t);
	aif->audio_state.hdmirx_aud_clkrate = hdmirx_aud_clkrate_t;
	aif->audio_state.fs_audio = fs_audio;
}


static void rk628_hdmirx_audio_enable(struct rk628_audioinfo *aif)
{
	u32 fifo_mask = AFIF_OVERFL_ISTS | AFIF_UNDERFL_ISTS;
	u32 fifo_ints = 0;

	/*
	 * RK628F powers up with stale FIFO status (commonly 0x1f).  Use the
	 * vendor double-reset sequence when both sticky xrun bits are set.
	 */
	rk628_i2c_read(aif->rk628, HDMI_RX_AUD_FIFO_ISTS, &fifo_ints);
	if ((fifo_ints & fifo_mask) == fifo_mask)
		rk628_hdmirx_audio_fifo_initd(aif);
	else if (fifo_ints & fifo_mask)
		rk628_hdmirx_audio_fifo_init(aif);

	rk628_i2c_update_bits(aif->rk628, HDMI_RX_DMI_DISABLE_IF,
			      AUD_ENABLE_MASK, AUD_ENABLE(1));
	aif->audio_state.audio_enable = true;

	/*
	 * RK628F/H uses delayed_work_audio_v2 as the sole FIFO owner; its CSI
	 * ISR deliberately does not service audio FIFO interrupts.  Do not
	 * enable an IRQ source with no consumer.  Older revisions retain the
	 * vendor interrupt-driven recovery path.
	 */
	if (aif->rk628->version >= RK628F_VERSION) {
		aif->fifo_ints_en = false;
		rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_IEN_CLR,
				fifo_mask);
	} else {
		aif->fifo_ints_en = true;
		rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_IEN_SET,
				fifo_mask);
	}
}


static int rk628_hdmirx_audio_clk_adjust(struct rk628_audioinfo *aif,
					 int total_offset, int single_offset)
{
	int delay_ms = 500;
	int ppm = 10;
	u32 offset_abs = abs(total_offset);

	if (offset_abs > 200) {
		ppm += 200;
		delay_ms -= 100;
	}
	if (offset_abs > 100) {
		ppm += 200;
		delay_ms -= 100;
	}
	if (offset_abs > 32) {
		ppm += 20;
		delay_ms -= 100;
	}
	if (offset_abs > 16)
		ppm += 20;

	if (total_offset > 16 && single_offset > 0)
		rk628_hdmirx_audio_clk_ppm_inc(aif, ppm);
	else if (total_offset < -16 && single_offset < 0)
		rk628_hdmirx_audio_clk_ppm_inc(aif, -ppm);

	if (!aif->audio_present)
		delay_ms = 50;
	return delay_ms;
}

static void rk628_hdmirx_audio_state_change(struct rk628_audioinfo *aif, bool on)
{
	if (on) {
		if (aif->stablecount < aif->stablelimit) {
			aif->stablecount++;
			return;
		}
		if (!aif->audio_present) {
			aif->audio_present = true;
			dev_info(aif->dev, "audio on\n");
		}
	} else {
		aif->stablecount = 0;
		if (aif->audio_present) {
			aif->audio_present = false;
			dev_info(aif->dev, "audio off\n");
		}
	}
}



/* RK628F/H audio uses a polled FIFO recovery loop.  The older worker only
 * follows FIFO fill level and can remain permanently stuck after the first
 * underflow/overflow.  This mirrors the state machine used by the known-good
 * Android device while retaining this SDK's public audio API. */
static void rk628_csi_delayed_work_audio_v2(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct rk628_audioinfo *aif = container_of(dwork,
						   struct rk628_audioinfo,
						   delayed_work_audio);
	struct rk628_audiostate *audio_state = &aif->audio_state;
	struct rk628 *rk628 = aif->rk628;
	u32 fs_audio;
	u32 fifo_ints;
	u32 sample_flat;
	int ret;
	u32 fifo_fill;
	int single_offset;
	int total_offset;
	unsigned long delay_ms = 500;
	static int consecutive_resets = 0;
	static unsigned long last_reset_time = 0;

	/* The CSI/video worker owns confctl_mutex during hotplug and mode changes.
	 * Never wait for that lock here: those paths synchronously cancel this
	 * worker while holding it. A failed trylock lets the worker exit cleanly
	 * and be rescheduled after the mode transition. */
	if (!mutex_trylock(aif->confctl_mutex)) {
		schedule_delayed_work(&aif->delayed_work_audio, msecs_to_jiffies(100));
		return;
	}
	ret = _rk628_hdmirx_audio_fs(aif, &fs_audio);
	if (ret) {
		dev_dbg(rk628->dev, "%s: RK628 audio rate read failed: %d\n",
			__func__, ret);
		delay_ms = 1000;
		goto unlock_exit;
	}
	/*
	 * FIFO status is sticky across HDMI link/clock transitions. At boot it
	 * commonly reads 0x1f before the receiver has been enabled. Recovering
	 * before the first enable creates a closed loop: audio is reset every
	 * 50 ms, AUD_ENABLE is never asserted, and I2S3 consequently receives no
	 * BCLK/LRCK or DMA data. Establish the I2S link before inspecting sticky
	 * FIFO status; the IRQ path remains the single recovery owner.
	 */
	if (!is_validfs(fs_audio)) {
		dev_dbg(rk628->dev, "%s: unsupported fs %u\n",
			__func__, fs_audio);
		rk628_hdmirx_audio_state_change(aif, false);
		delay_ms = 1000;
		goto exit;
	}

	if (!audio_state->audio_enable) {
		rk628_hdmirx_audio_set_fs(aif, fs_audio);
		rk628_hdmirx_audio_enable(aif);
		rk628_hdmirx_audio_state_change(aif, false);
		delay_ms = 1000;
		goto exit;
	}

	ret = rk628_i2c_read(rk628, HDMI_RX_AUD_FIFO_ISTS, &fifo_ints);
	if (ret) {
		dev_dbg(rk628->dev, "%s: RK628 FIFO status read failed: %d\n",
			__func__, ret);
		delay_ms = 1000;
		goto unlock_exit;
	}
	/* Android tolerates the 0x9 startup state; all other xruns recover. */
	if (fifo_ints != 0x9 &&
	    (fifo_ints & (AFIF_UNDERFL_ISTS | AFIF_OVERFL_ISTS))) {
		/* RK628F/H has FIFO IRQ recovery disabled; this polled worker is
		 * therefore the sole owner of xrun recovery. Clearing ISTS alone
		 * leaves the FIFO stopped and produces no I2S/DMA data. Match the
		 * known-good Android sequence: clear all sticky bits, reset/re-prime
		 * the FIFO, then restore the 256-word target fill level. */

		/* Track consecutive resets to prevent infinite loops when hardware
		 * clock cannot lock. If we've reset 5+ times in the last 10 seconds,
		 * back off significantly to allow PLL time to stabilize. */
		unsigned long now = jiffies;
		if (time_after(now, last_reset_time + msecs_to_jiffies(10000))) {
			consecutive_resets = 0;
		}
		consecutive_resets++;
		last_reset_time = now;

		if (consecutive_resets > 5) {
			dev_warn(rk628->dev, "%s: %d consecutive FIFO resets (status %#x), "
				 "extending delay for PLL stabilization\n",
				 __func__, consecutive_resets, fifo_ints);
			delay_ms = 3000;
		} else if (consecutive_resets > 2) {
			delay_ms = 2000;
		} else {
			delay_ms = 1500;
		}

		if ((fifo_ints & (AFIF_UNDERFL_ISTS | AFIF_OVERFL_ISTS)) ==
		    (AFIF_UNDERFL_ISTS | AFIF_OVERFL_ISTS))
			rk628_hdmirx_audio_fifo_initd(aif);
		else
			rk628_hdmirx_audio_fifo_init(aif);
		rk628_hdmirx_audio_state_change(aif, false);
		dev_warn(rk628->dev, "%s: reset audio FIFO after status %#x (delay %lums)\n",
			 __func__, fifo_ints, delay_ms);
		goto exit;
	} else {
		/* FIFO is healthy, reset the consecutive counter */
		if (consecutive_resets > 0) {
			dev_info(rk628->dev, "%s: FIFO recovered after %d resets\n",
				 __func__, consecutive_resets);
			consecutive_resets = 0;
		}
	}

	if (abs((int)fs_audio - (int)audio_state->fs_audio) > 1000) {
		dev_info(rk628->dev, "%s: restart audio fs %u -> %u\n",
			 __func__, audio_state->fs_audio, fs_audio);
		rk628_hdmirx_audio_set_fs(aif, fs_audio);
		rk628_hdmirx_audio_fifo_init(aif);
		rk628_hdmirx_audio_state_change(aif, false);
		delay_ms = 1000;
		goto exit;
	}

	ret = rk628_i2c_read(rk628, HDMI_RX_AUD_FIFO_FILLSTS1, &fifo_fill);
	if (ret) {
		delay_ms = 1000;
		goto unlock_exit;
	}
	single_offset = (int)fifo_fill - audio_state->pre_state;
	total_offset = (int)fifo_fill - audio_state->init_state;
	dev_dbg(rk628->dev,
		 "%s: FIFO_FILLSTS1:%#x single:%d total:%d\n",
		 __func__, fifo_fill, single_offset, total_offset);

	if (fifo_fill) {
		rk628_hdmirx_audio_state_change(aif, true);
		delay_ms = rk628_hdmirx_audio_clk_adjust(aif,
						      total_offset, single_offset);
	} else {
		rk628_hdmirx_audio_state_change(aif, false);
		delay_ms = 50;
	}
	audio_state->pre_state = fifo_fill;
	if (aif->i2s_enabled) {
		ret = rk628_i2c_read(rk628, HDMI_RX_AUD_SPARE, &sample_flat);
		if (!ret) {
			sample_flat = !!(sample_flat & AUDS_MAS_SAMPLE_FLAT);
			if (sample_flat != aif->sample_flat) {
				dev_info(rk628->dev,
					 "audio sample flat change to %d\n", sample_flat);
				rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL,
						I2S_LPCM_BPCUV(0) |
						I2S_32_16(1) |
						(sample_flat ?
						 I2S_DATA_ENABLE_BITS(0xf) :
						 I2S_DATA_ENABLE_BITS(0)));
				aif->sample_flat = sample_flat;
			}
		}
	}

exit:
unlock_exit:
	mutex_unlock(aif->confctl_mutex);
	schedule_delayed_work(&aif->delayed_work_audio,
			      msecs_to_jiffies(delay_ms));
}

static void rk628_csi_delayed_work_audio(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct rk628_audioinfo *aif = container_of(dwork, struct rk628_audioinfo,
						   delayed_work_audio);
	struct rk628_audiostate *audio_state = &aif->audio_state;
	u32 fs_audio;
	int cur_state, init_state, pre_state;

	init_state = audio_state->init_state;
	pre_state = audio_state->pre_state;
	if (_rk628_hdmirx_audio_fs(aif, &fs_audio)) {
		dev_dbg(aif->dev, "%s: no readable audio rate\n", __func__);
		goto exit;
	}
	if (!is_validfs(fs_audio)) {
		dev_dbg(aif->dev, "%s: no supported fs(%u)\n", __func__, fs_audio);
		goto exit;
	}
	if (!audio_state->audio_enable) {
		rk628_hdmirx_audio_set_fs(aif, fs_audio);
		rk628_hdmirx_audio_enable(aif);
		goto exit;
	}
	if (abs(fs_audio - audio_state->fs_audio) > 1000)
		rk628_hdmirx_audio_set_fs(aif, fs_audio);
	rk628_i2c_read(aif->rk628, HDMI_RX_AUD_FIFO_FILLSTS1, &cur_state);
	dev_dbg(aif->dev, "%s: HDMI_RX_AUD_FIFO_FILLSTS1:%#x, single offset:%d, total offset:%d\n",
		 __func__, cur_state, cur_state - pre_state, cur_state - init_state);
	if (cur_state != 0)
		aif->audio_present = true;
	else
		aif->audio_present = false;

	if ((cur_state - init_state) > 16 && (cur_state - pre_state) > 0)
		rk628_hdmirx_audio_clk_inc_rate(aif, 10);
	else if ((cur_state != 0) && (cur_state - init_state) < -16 && (cur_state - pre_state) < 0)
		rk628_hdmirx_audio_clk_inc_rate(aif, -10);
	audio_state->pre_state = cur_state;
exit:
	schedule_delayed_work(&aif->delayed_work_audio, msecs_to_jiffies(1000));

}

static void rk628_csi_delayed_work_audio_rate_change(struct work_struct *work)
{
	u32 fifo_fillsts;
	u32 fs_audio;
	struct delayed_work *dwork = to_delayed_work(work);
	struct rk628_audioinfo *aif = container_of(dwork, struct rk628_audioinfo,
						   delayed_work_audio_rate_change);

	mutex_lock(aif->confctl_mutex);
	if (_rk628_hdmirx_audio_fs(aif, &fs_audio)) {
		dev_dbg(aif->dev, "%s: cannot read audio fs\n", __func__);
		mutex_unlock(aif->confctl_mutex);
		return;
	}
	dev_dbg(aif->dev, "%s get audio fs %u\n", __func__, fs_audio);
	if (aif->audio_state.ctsn_flag == (ACR_N_CHG_ICLR | ACR_CTS_CHG_ICLR)) {
		aif->audio_state.ctsn_flag = 0;
		if (is_validfs(fs_audio)) {
			rk628_hdmirx_audio_set_fs(aif, fs_audio);
			/* We start audio work after recieveing cts n interrupt */
			rk628_hdmirx_audio_enable(aif);
		} else {
			dev_dbg(aif->dev, "%s invalid fs when ctsn updating\n", __func__);
		}
		schedule_delayed_work(&aif->delayed_work_audio, msecs_to_jiffies(1000));
	}
	if (aif->audio_state.fifo_int) {
		aif->audio_state.fifo_int = false;
		if (is_validfs(fs_audio))
			rk628_hdmirx_audio_set_fs(aif, fs_audio);
		rk628_i2c_read(aif->rk628, HDMI_RX_AUD_FIFO_FILLSTS1, &fifo_fillsts);
		if (!fifo_fillsts) {
			dev_dbg(aif->dev, "%s underflow after overflow\n", __func__);
			rk628_hdmirx_audio_fifo_initd(aif);
		} else {
			dev_dbg(aif->dev, "%s overflow after underflow\n", __func__);
			rk628_hdmirx_audio_fifo_initd(aif);
		}
	}
	mutex_unlock(aif->confctl_mutex);
}

HAUDINFO rk628_hdmirx_audioinfo_alloc(struct device *dev,
				      struct mutex *confctl_mutex,
				      struct rk628 *rk628,
				      bool en)
{
	struct rk628_audioinfo *aif;

	aif = devm_kzalloc(dev, sizeof(*aif), GFP_KERNEL);
	if (!aif)
		return NULL;
	if (rk628->version >= RK628F_VERSION) {
		INIT_DELAYED_WORK(&aif->delayed_work_audio,
				  rk628_csi_delayed_work_audio_v2);
	} else {
		INIT_DELAYED_WORK(&aif->delayed_work_audio,
				  rk628_csi_delayed_work_audio);
		INIT_DELAYED_WORK(&aif->delayed_work_audio_rate_change,
				  rk628_csi_delayed_work_audio_rate_change);
	}
	aif->confctl_mutex = confctl_mutex;
	aif->rk628 = rk628;
	aif->i2s_enabled_default = en;
	aif->dev = dev;
	return aif;
}

EXPORT_SYMBOL(rk628_hdmirx_audioinfo_alloc);

void rk628_hdmirx_audio_cancel_work_audio(HAUDINFO info, bool sync)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	if (sync)
		cancel_delayed_work_sync(&aif->delayed_work_audio);
	else
		cancel_delayed_work(&aif->delayed_work_audio);
}
EXPORT_SYMBOL(rk628_hdmirx_audio_cancel_work_audio);

void rk628_hdmirx_audio_cancel_work_rate_change(HAUDINFO info, bool sync)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	if (sync)
		cancel_delayed_work_sync(&aif->delayed_work_audio_rate_change);
	else
		cancel_delayed_work(&aif->delayed_work_audio_rate_change);
}
EXPORT_SYMBOL(rk628_hdmirx_audio_cancel_work_rate_change);

void rk628_hdmirx_audio_destroy(HAUDINFO info)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	if (!aif)
		return;
	rk628_hdmirx_audio_cancel_work_audio(aif, true);
	if (aif->rk628->version < RK628F_VERSION)
		rk628_hdmirx_audio_cancel_work_rate_change(aif, true);
	aif->confctl_mutex = NULL;
	aif->rk628 = NULL;
}
EXPORT_SYMBOL(rk628_hdmirx_audio_destroy);

bool rk628_hdmirx_audio_present(HAUDINFO info)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	if (!aif)
		return false;
	return aif->audio_present;
}
EXPORT_SYMBOL(rk628_hdmirx_audio_present);

int rk628_hdmirx_audio_fs(HAUDINFO info)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	if (!aif)
		return 0;
	return aif->audio_state.fs_audio;
}
EXPORT_SYMBOL(rk628_hdmirx_audio_fs);

void rk628_hdmirx_audio_i2s_ctrl(HAUDINFO info, bool enable)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;

	/*
	 * RK628 is the external I2S clock master.  The Android 131 driver
	 * mutes only the four data lanes when the video stream is stopped;
	 * BCLK/LRCK must keep running.  The default-enabled mode owns this
	 * register elsewhere and must not be toggled here.
	 */
	if (enable == aif->i2s_enabled || aif->i2s_enabled_default)
		return;

	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_SAO_CTRL,
			I2S_LPCM_BPCUV(0) |
			I2S_32_16(1) |
			(enable && !aif->sample_flat ?
			 I2S_DATA_ENABLE_BITS(0) : I2S_DATA_ENABLE_BITS(0xf)));
	aif->i2s_enabled = enable;
}
EXPORT_SYMBOL(rk628_hdmirx_audio_i2s_ctrl);


void rk628_hdmirx_audio_setup(HAUDINFO info)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;
	struct rk628 *rk628 = aif->rk628;
	u32 audio_pll_n = 5644;
	u32 audio_pll_cts = 148500;

	dev_dbg(aif->dev, "%s: setup audio\n", __func__);
	aif->audio_state.ctsn_flag = 0;
	aif->audio_state.fs_audio = 0;
	aif->audio_state.pre_state = 0;
	aif->audio_state.init_state = INIT_FIFO_STATE * 4;
	aif->audio_state.fifo_int = false;
	aif->audio_state.audio_enable = false;
	aif->sample_flat = false;
	aif->fifo_ints_en = false;
	aif->ctsn_ints_en = false;
	aif->i2s_enabled = false;
	aif->audio_present = false;
	aif->stablelimit = 3;
	aif->stablecount = 0;

	/* RK628F APLL must leave deep/slow mode before its audio clock is set. */
	if (rk628->version >= RK628F_VERSION)
		rk628_i2c_write(rk628, CRU_MODE_CON00,
				HIWORD_UPDATE(1, 4, 4));

	/* Android enables clk_hdmirx_aud before programming its rate. */
	rk628_clk_enable(rk628, CGU_CLK_HDMIRX_AUD);

	/* Configure audio control register (Android: 0x00000006) */
	rk628_i2c_write(rk628, HDMI_RX_AUD_CTRL, 0x00000006);

	rk628_hdmirx_audio_clk_set_rate(aif, DEFAULT_AUDIO_CLK);
	rk628_i2c_write(rk628, HDMI_RX_AUDPLL_GEN_CTS, audio_pll_cts);
	rk628_i2c_write(rk628, HDMI_RX_AUDPLL_GEN_N, audio_pll_n);
	rk628_i2c_update_bits(rk628, HDMI_RX_AUD_CLK_CTRL,
			      CTS_N_REF_MASK, CTS_N_REF(1));
	rk628_i2c_update_bits(rk628, HDMI_RX_AUD_PLL_CTRL,
			      PLL_LOCK_TOGGLE_DIV_MASK, PLL_LOCK_TOGGLE_DIV(0));
	/* Match the live Android 131 AUD_FIFO_TH value 0x01001008. */
	rk628_i2c_update_bits(rk628, HDMI_RX_AUD_FIFO_TH,
			      AFIF_TH_START_MASK |
			      AFIF_TH_MAX_MASK |
			      AFIF_TH_MIN_MASK,
			      AFIF_TH_START(RK628_AUDIO_FIFO_START) |
			      AFIF_TH_MAX(RK628_AUDIO_FIFO_MAX) |
			      AFIF_TH_MIN(RK628_AUDIO_FIFO_MIN));

	/* Select one HDMI audio subpacket, matching the Android 4.19 driver. */
	rk628_i2c_update_bits(rk628, HDMI_RX_AUD_FIFO_CTRL,
			      AFIF_SUBPACKET_DESEL_MASK |
			      AFIF_SUBPACKETS_MASK,
			      AFIF_SUBPACKET_DESEL(0) |
			      AFIF_SUBPACKETS(1));

	rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL,
			I2S_LPCM_BPCUV(0) |
			I2S_32_16(1) |
			(aif->i2s_enabled_default ? 0 : I2S_DATA_ENABLE_BITS(0xf)));
	aif->i2s_enabled = aif->i2s_enabled_default;

	rk628_i2c_write(rk628, HDMI_RX_AUD_MUTE_CTRL,
			APPLY_INT_MUTE(0) |
			APORT_SHDW_CTRL(3) |
			AUTO_ACLK_MUTE(2) |
			AUD_MUTE_SPEED(1) |
			AUD_AVMUTE_EN(1) |
			AUD_MUTE_SEL(0) |
			AUD_MUTE_MODE(1));

	rk628_i2c_write(rk628, HDMI_RX_AUD_PAO_CTRL, PAO_RATE(0));
	rk628_i2c_write(rk628, HDMI_RX_AUD_CHEXTR_CTRL, AUD_LAYOUT_CTRL(1));

	rk628_i2c_write(rk628, HDMI_RX_PDEC_AUDIODET_CTRL,
			AUDIODET_THRESHOLD(0));

	if (rk628->version >= RK628F_VERSION) {
		schedule_delayed_work(&aif->delayed_work_audio,
				      msecs_to_jiffies(1000));
	} else {
		aif->ctsn_ints_en = true;
		rk628_i2c_write(rk628, HDMI_RX_PDEC_IEN_SET,
				ACR_N_CHG_ICLR | ACR_CTS_CHG_ICLR);
		rk628_i2c_write(rk628, HDMI_RX_PDEC_AUDIODET_CTRL,
				AUDIODET_THRESHOLD(0));
	}
}

EXPORT_SYMBOL(rk628_hdmirx_audio_setup);

bool rk628_audio_fifoints_enabled(HAUDINFO info)
{
	return ((struct rk628_audioinfo *)info)->fifo_ints_en;
}
EXPORT_SYMBOL(rk628_audio_fifoints_enabled);

bool rk628_audio_ctsnints_enabled(HAUDINFO info)
{
	return ((struct rk628_audioinfo *)info)->ctsn_ints_en;
}
EXPORT_SYMBOL(rk628_audio_ctsnints_enabled);

void rk628_csi_isr_ctsn(HAUDINFO info, u32 pdec_ints)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;
	u32 ctsn_mask = ACR_N_CHG_ICLR | ACR_CTS_CHG_ICLR;

	dev_dbg(aif->dev, "%s: pdec_ints:%#x\n", __func__, pdec_ints);
	/* cts & n both need update but maybe come diff int */
	if (pdec_ints & ACR_N_CHG_ICLR)
		aif->audio_state.ctsn_flag |= ACR_N_CHG_ICLR;
	if (pdec_ints & ACR_CTS_CHG_ICLR)
		aif->audio_state.ctsn_flag |= ACR_CTS_CHG_ICLR;
	if (aif->audio_state.ctsn_flag == ctsn_mask) {
		dev_dbg(aif->dev, "%s: ctsn updated, disable ctsn int\n", __func__);
		rk628_i2c_write(aif->rk628, HDMI_RX_PDEC_IEN_CLR, ctsn_mask);
		aif->ctsn_ints_en = false;
		schedule_delayed_work(&aif->delayed_work_audio_rate_change, 0);
	}
	rk628_i2c_write(aif->rk628, HDMI_RX_PDEC_ICLR, pdec_ints & ctsn_mask);
}
EXPORT_SYMBOL(rk628_csi_isr_ctsn);

void rk628_csi_isr_fifoints(HAUDINFO info, u32 fifo_ints)
{
	struct rk628_audioinfo *aif = (struct rk628_audioinfo *)info;
	u32 fifo_mask = AFIF_OVERFL_ISTS | AFIF_UNDERFL_ISTS;

	dev_dbg(aif->dev, "%s: fifo_ints:%#x\n", __func__, fifo_ints);
	/* cts & n both need update but maybe come diff int */
	if (fifo_ints & AFIF_OVERFL_ISTS) {
		dev_dbg(aif->dev, "%s: Audio FIFO overflow\n", __func__);
		aif->audio_state.fifo_flag |= AFIF_OVERFL_ISTS;
	}
	if (fifo_ints & AFIF_UNDERFL_ISTS) {
		dev_dbg(aif->dev, "%s: Audio FIFO underflow\n", __func__);
		aif->audio_state.fifo_flag |= AFIF_UNDERFL_ISTS;
	}
	if (aif->audio_state.fifo_flag == fifo_mask) {
		aif->audio_state.fifo_int = true;
		aif->audio_state.fifo_flag = 0;
		schedule_delayed_work(&aif->delayed_work_audio_rate_change, 0);
	}
	rk628_i2c_write(aif->rk628, HDMI_RX_AUD_FIFO_ICLR, fifo_ints & fifo_mask);
}
EXPORT_SYMBOL(rk628_csi_isr_fifoints);

int rk628_is_avi_ready(struct rk628 *rk628, bool avi_rcv_rdy)
{
	u8 i;
	u32 val, avi_pb = 0;
	u8 cnt = 0, max_cnt = 2;
	u32 hdcp_ctrl_val = 0;

	if (rk628->version == RK628F_VERSION)
		return 1;

	rk628_i2c_read(rk628, HDMI_RX_HDCP_CTRL, &val);
	if ((val & HDCP_ENABLE_MASK))
		max_cnt = 5;

	for (i = 0; i < 100; i++) {
		rk628_i2c_read(rk628, HDMI_RX_PDEC_AVI_PB, &val);
		dev_info(rk628->dev, "%s PDEC_AVI_PB:%#x, avi_rcv_rdy:%d\n",
			 __func__, val, avi_rcv_rdy);
		if (i > 30 && !(hdcp_ctrl_val & 0x400)) {
			rk628_i2c_read(rk628, HDMI_RX_HDCP_CTRL, &hdcp_ctrl_val);
			/* force hdcp avmute */
			hdcp_ctrl_val |= 0x400;
			rk628_i2c_write(rk628, HDMI_RX_HDCP_CTRL, hdcp_ctrl_val);
		}

		if (val && val == avi_pb && avi_rcv_rdy) {
			if (++cnt >= max_cnt)
				break;
		} else {
			cnt = 0;
			avi_pb = val;
		}
		msleep(30);
	}
	if (cnt < max_cnt)
		return 0;

	return 1;
}
EXPORT_SYMBOL(rk628_is_avi_ready);

static void hdmirxphy_write(struct rk628 *rk628,  u32 offset, u32 val)
{
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_ADDRESS, offset);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_DATAO, val);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_OPERATION, 1);
}

u32 hdmirxphy_read(struct rk628 *rk628,  u32 offset)
{
	u32 val;

	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_ADDRESS, offset);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_OPERATION, 2);
	rk628_i2c_read(rk628, HDMI_RX_I2CM_PHYG3_DATAI, &val);

	return val;
}

static void rk628_hdmirxphy_enable(struct rk628 *rk628, bool is_hdmi2)
{
	hdmirxphy_write(rk628, 0x02, 0x1860);
	hdmirxphy_write(rk628, 0x03, 0x0060);
	hdmirxphy_write(rk628, 0x0d, 0x00c0);
	hdmirxphy_write(rk628, 0x0d, 0x00c0);
	hdmirxphy_write(rk628, 0x27, 0x1c94);
	hdmirxphy_write(rk628, 0x28, 0x3713);
	hdmirxphy_write(rk628, 0x29, 0x24da);
	hdmirxphy_write(rk628, 0x2a, 0x5492);
	hdmirxphy_write(rk628, 0x2b, 0x4b0d);
	hdmirxphy_write(rk628, 0x2d, 0x008c);
	hdmirxphy_write(rk628, 0x2e, 0x0001);

	if (is_hdmi2)
		hdmirxphy_write(rk628, 0x0e, 0x0108);
	else
		hdmirxphy_write(rk628, 0x0e, 0x0008);

}

static void rk628_hdmirxphy_set_clrdpt(struct rk628 *rk628, bool is_8bit)
{
	if (is_8bit)
		hdmirxphy_write(rk628, 0x03, 0x0000);
	else
		hdmirxphy_write(rk628, 0x03, 0x0060);
}

void rk628_hdmirx_verisyno_phy_power_on(struct rk628 *rk628)
{
	bool is_hdmi2 = false;
	u32 val;
	int i;

	/* wait tx to write scdc tmds ratio */
	for (i = 0; i < 50; i++) {
		rk628_i2c_read(rk628, HDMI_RX_SCDC_REGS0, &val);
		if (val & SCDC_TMDSBITCLKRATIO)
			break;
		msleep(20);
	}

	if (val & SCDC_TMDSBITCLKRATIO)
		is_hdmi2 = true;

	dev_info(rk628->dev, "%s: %s\n", __func__, is_hdmi2 ? "hdmi2.0" : "hdmi1.4");
	/* power down phy */
	rk628_i2c_write(rk628, GRF_SW_HDMIRXPHY_CRTL, 0x17);
	usleep_range(20, 30);
	rk628_i2c_write(rk628, GRF_SW_HDMIRXPHY_CRTL, 0x15);
	/* init phy i2c */
	rk628_i2c_write(rk628, HDMI_RX_SNPS_PHYG3_CTRL, 0);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_SS_CNTS, 0x018c01d2);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_FS_HCNT, 0x003c0081);
	rk628_i2c_write(rk628, HDMI_RX_I2CM_PHYG3_MODE, 1);
	rk628_i2c_write(rk628, GRF_SW_HDMIRXPHY_CRTL, 0x11);
	/* enable rx phy */
	rk628_hdmirxphy_enable(rk628, is_hdmi2);
	rk628_i2c_write(rk628, GRF_SW_HDMIRXPHY_CRTL, 0x14);
	msleep(20);
}
EXPORT_SYMBOL(rk628_hdmirx_verisyno_phy_power_on);

void rk628_hdmirx_phy_prepclk_cfg(struct rk628 *rk628)
{
	u32 format;
	bool is_clrdpt_8bit = false;

	usleep_range(20 * 1000, 30 * 1000);
	rk628_i2c_read(rk628, HDMI_RX_PDEC_AVI_PB, &format);
	format = (format & VIDEO_FORMAT_MASK) >> 5;
	dev_info(rk628->dev, "%s: format = %d from AVI\n", __func__, format);

	/* yuv420 should set phy color depth 8bit */
	if (format == 3)
		is_clrdpt_8bit = true;

	rk628_i2c_read(rk628, HDMI_RX_PDEC_GCP_AVMUTE, &format);
	format = (format & PKTDEC_GCP_CD_MASK) >> 4;
	dev_info(rk628->dev, "%s: format = %d from GCP\n", __func__, format);

	/* 10bit color depth should set phy color depth 8bit */
	if (format == 5)
		is_clrdpt_8bit = true;

	rk628_hdmirxphy_set_clrdpt(rk628, is_clrdpt_8bit);
}
EXPORT_SYMBOL(rk628_hdmirx_phy_prepclk_cfg);

static const char * const bus_format_str[] = {
	"RGB",
	"YUV422",
	"YUV444",
	"YUV420",
	"UNKNOWN",
};

u8 rk628_hdmirx_get_format(struct rk628 *rk628)
{
	u32 val;
	u8 video_fmt;

	rk628_i2c_read(rk628, HDMI_RX_PDEC_AVI_PB, &val);
	video_fmt = (val & VIDEO_FORMAT_MASK) >> 5;
	if (video_fmt > BUS_FMT_UNKNOWN)
		video_fmt = BUS_FMT_UNKNOWN;
	dev_info(rk628->dev, "%s: format = %s\n", __func__, bus_format_str[video_fmt]);

	return video_fmt;
}
EXPORT_SYMBOL(rk628_hdmirx_get_format);

u32 rk628_hdmirx_get_tmdsclk_cnt(struct rk628 *rk628)
{
	int i, j;
	u32 val, tmdsclk_cnt = 0;
	struct hdmirx_tmdsclk_cnt tmdsclk[HDMIRX_GET_TMDSCLK_TIME] = {0};

	for (i = 0; i < HDMIRX_GET_TMDSCLK_TIME; i++) {
		rk628_i2c_read(rk628, HDMI_RX_HDMI_CKM_RESULT, &val);
		tmdsclk_cnt = val & 0xffff;
		for (j = 0; j < HDMIRX_GET_TMDSCLK_TIME; j++) {
			if (tmdsclk_cnt == tmdsclk[j].tmds_cnt || !tmdsclk[j].tmds_cnt) {
				tmdsclk[j].tmds_cnt = tmdsclk_cnt;
				tmdsclk[j].cnt++;
				break;
			}
		}
	}

	for (i = 0; i < HDMIRX_GET_TMDSCLK_TIME; i++) {
		if (!tmdsclk[i].tmds_cnt)
			return tmdsclk_cnt;

		dev_info(rk628->dev, "tmdsclk_cnt: %d, cnt: %d\n",
			 tmdsclk[i].tmds_cnt, tmdsclk[i].cnt);
		if (!i)
			tmdsclk_cnt = tmdsclk[i].tmds_cnt;
		else if (tmdsclk[i].cnt > tmdsclk[i - 1].cnt)
			tmdsclk_cnt = tmdsclk[i].tmds_cnt;
	}

	return tmdsclk_cnt;
}
EXPORT_SYMBOL(rk628_hdmirx_get_tmdsclk_cnt);
