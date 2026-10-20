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
 * tear check; we only set PP_AUTOREFRESH_CONFIG and kick CTL_START once.
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
#define PP_LINE_COUNT		0x2c
#define PP_AUTOREFRESH_CONFIG	0x30	/* bit 31 enable, [15:0] TE pulses per frame */

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

int fajita_disp_autorefresh(struct fajita_disp *d)
{
	d->gdsc = rd(DISPCC_BASE + DISPCC_MDSS_GDSCR);
	d->mdp_cbcr = rd(DISPCC_BASE + DISPCC_MDP_CBCR);
	d->ahb_cbcr = rd(DISPCC_BASE + DISPCC_AHB_CBCR);
	d->pp = d->ctl = -1;
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
	if (d->ctl < 0)
		return FAJITA_DISP_NO_CMD_CTL;	/* video mode (or nothing lit): nothing to do */
	if (d->pp < 0)
		return FAJITA_DISP_NO_TE;

	/* One frame per TE pulse (~60 Hz), then start the pipe once. */
	wr(PP(d->pp) + PP_AUTOREFRESH_CONFIG, (1U << 31) | 1);
	__asm__ volatile("dsb sy" ::: "memory");
	if (!(rd(CTL(d->ctl) + CTL_START) & 1))
		wr(CTL(d->ctl) + CTL_START, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	d->ar_after = rd(PP(d->pp) + PP_AUTOREFRESH_CONFIG);
	return (d->ar_after & (1U << 31)) ? FAJITA_DISP_OK : FAJITA_DISP_NOT_STICKY;
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
