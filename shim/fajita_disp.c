/*
 * fajita (OnePlus 6T) display bring-up for the shim.
 *
 * The 6T's Samsung s6e3fc2x01 (AMS641RW) is a MIPI DSI *command-mode*
 * panel: it keeps its own frame RAM and the display engine (SDM845 DPU 4.0)
 * only transfers a frame when it is triggered. ABL pushes the splash once
 * and stops, so anything later written into the splash buffer would never
 * reach the glass. sweet's port relies on a video-mode panel that scans the
 * buffer continuously.
 *
 * Fix: enable the ping-pong block's hardware autorefresh. With it the DPU
 * re-sends the frame from the same source buffer on every tear-effect (TE)
 * pulse, so the panel behaves like video mode for everything that follows
 * (this console, XNU's early console, fbcon, CoreAnimation frames). ABL
 * already configured the whole pipe (SSPP -> LM -> PP -> INTF -> DSI) and
 * tear check; we only set PP_AUTOREFRESH_CONFIG. As downstream SDE
 * (sde_encoder_phys_cmd.c) does, CTL_START is never written while
 * autorefresh is on: the panel's TE pulses drive the transfers.
 *
 * Register offsets: Linux drivers/gpu/drm/msm/disp/dpu1 (dpu_4_0_sdm845.h,
 * dpu_hw_pingpong.c, dpu_hw_ctl.c) and drivers/clk/qcom/dispcc-sdm845.c.
 * MMU off: every access is Device-nGnRnE, 32-bit aligned.
 */
#include <stdint.h>
#include "fajita_disp.h"

#define DISPCC_BASE		0x0af00000UL
#define DISPCC_MDSS_GDSCR	0x3000	/* bit 31: PWR_ON */
#define DISPCC_MDP_CBCR		0x200c	/* bit 31: CLK_OFF */
#define DISPCC_AHB_CBCR		0x4004

#define MDP_BASE		0x0ae01000UL	/* mdss_mdp "mdp" region */
#define CTL(n)			(MDP_BASE + 0x1000 + 0x200 * (n))
#define CTL_TOP			0x14	/* bit 17 cmd mode, [7:4] intf */
#define CTL_FLUSH		0x18
#define CTL_START		0x1c
#define PP(n)			(MDP_BASE + 0x70000 + 0x800 * (n))
#define PP_TEAR_CHECK_EN	0x00
#define PP_SYNC_CONFIG_VSYNC	0x04	/* bit 19: vsync counter enable */
#define PP_SYNC_CONFIG_HEIGHT	0x08
#define PP_LINE_COUNT		0x2c
#define PP_AUTOREFRESH_CONFIG	0x30	/* bit 31 enable, [15:0] TE pulses per frame */
/* source pipes (dpu_4_0_sdm845.h): VIG0-3, DMA0-3 */
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define DISPCC_MDSS_CFG_GDSCR	0x3004	/* bit 16: power-on status (POLL_CFG_GDSCR) */

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

int fajita_disp_autorefresh(struct fajita_disp *d)
{
	d->gdsc = rd(DISPCC_BASE + DISPCC_MDSS_GDSCR);
	d->mdp_cbcr = rd(DISPCC_BASE + DISPCC_MDP_CBCR);
	d->ahb_cbcr = rd(DISPCC_BASE + DISPCC_AHB_CBCR);
	d->cfg_gdsc = rd(DISPCC_BASE + DISPCC_MDSS_CFG_GDSCR);
	d->pp = d->ctl = d->pipe = -1;
	/* An access to a powered-down or unclocked block hangs the bus. */
	if (!(d->gdsc & (1U << 31)) || (d->mdp_cbcr & (1U << 31)) || (d->ahb_cbcr & (1U << 31)))
		return FAJITA_DISP_UNPOWERED;

	for (int n = 0; n < 5; n++) {
		uint32_t top = rd(CTL(n) + CTL_TOP);
		if (d->ctl < 0 && (top & (1U << 17)) && ((top >> 4) & 0xf)) {
			d->ctl = n;
			d->ctl_top = top;
		}
	}
	for (int n = 0; n < 4; n++) {
		uint32_t te = rd(PP(n) + PP_TEAR_CHECK_EN);
		if (d->pp < 0 && (te & 1)) {
			d->pp = n;
			d->sync_cfg = rd(PP(n) + PP_SYNC_CONFIG_VSYNC);
			d->ar_before = rd(PP(n) + PP_AUTOREFRESH_CONFIG);
		}
	}
	/*
	 * fajita ABL (seen 2026-10-23): PP0 has its internal vsync counter
	 * (SYNC_CONFIG_VSYNC bit 19), the frame height and even autorefresh
	 * programmed, but TEAR_CHECK_EN is 0, so the sync engine never runs
	 * and no frame is sent. Switch tear check on for such a PP.
	 */
	for (int n = 0; n < 4 && d->pp < 0; n++) {
		uint32_t sync = rd(PP(n) + PP_SYNC_CONFIG_VSYNC);
		if ((sync & (1U << 19)) && rd(PP(n) + PP_SYNC_CONFIG_HEIGHT) != 0) {
			d->pp = n;
			d->sync_cfg = sync;
			d->ar_before = rd(PP(n) + PP_AUTOREFRESH_CONFIG);
			wr(PP(n) + PP_TEAR_CHECK_EN, 1);
			__asm__ volatile("dsb sy" ::: "memory");
			d->te_set = rd(PP(n) + PP_TEAR_CHECK_EN) | 0x100;
		}
	}
	/*
	 * Which pipe fetches the splash? If none, ABL's screen (e.g. the
	 * fastboot menu) scans another buffer and our pixels stay invisible.
	 */
	static const uint32_t sspp[8] = {0x4000, 0x6000, 0x8000, 0xa000,
		0x24000, 0x26000, 0x28000, 0x2a000};
	for (int n = 0; n < 8; n++) {
		uint32_t src = rd(MDP_BASE + sspp[n] + SSPP_SRC0_ADDR);
		if (d->pipe < 0 && src == FAJITA_SPLASH) {
			d->pipe = n;
			d->src = src;
			d->ystride = rd(MDP_BASE + sspp[n] + SSPP_SRC_YSTRIDE0) & 0xffff;
		}
	}
	if (d->ctl < 0)
		return FAJITA_DISP_NO_CMD_CTL;	/* video mode (or nothing lit): nothing to do */
	if (d->pp < 0)
		return FAJITA_DISP_NO_TE;

	/* One frame per TE pulse (~60 Hz); no CTL_START (downstream SDE). */
	wr(PP(d->pp) + PP_AUTOREFRESH_CONFIG, (1U << 31) | 1);
	__asm__ volatile("dsb sy" ::: "memory");
	d->ar_after = rd(PP(d->pp) + PP_AUTOREFRESH_CONFIG);
	if (!(d->ar_after & (1U << 31)))
		return FAJITA_DISP_NOT_STICKY;
	return d->pipe < 0 ? FAJITA_DISP_OTHER_BUFFER : FAJITA_DISP_OK;
}

/*
 * Manual fallback: one frame transfer now (used by the shim's own screens
 * when autorefresh did not stick). Does nothing while a transfer runs.
 */
void fajita_disp_kick(const struct fajita_disp *d)
{
	if (d->ctl < 0 || (rd(CTL(d->ctl) + CTL_START) & 1))
		return;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(CTL(d->ctl) + CTL_START, 1);
}

uint32_t fajita_disp_line(const struct fajita_disp *d)
{
	return d->pp < 0 ? 0 : rd(PP(d->pp) + PP_LINE_COUNT);
}
