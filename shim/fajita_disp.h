#pragma once
#include <stdint.h>

enum {
	FAJITA_DISP_OK = 0,		/* autorefresh on: panel follows the buffer */
	FAJITA_DISP_UNPOWERED,		/* MDSS GDSC/clocks off: DPU not touched */
	FAJITA_DISP_NO_CMD_CTL,		/* no command-mode CTL (video mode panel?) */
	FAJITA_DISP_NO_TE,		/* no ping-pong with tear check enabled */
	FAJITA_DISP_NOT_STICKY,		/* write did not read back */
	FAJITA_DISP_OTHER_BUFFER,	/* autorefresh on, but no pipe fetches the splash */
};

#define FAJITA_SPLASH	0x9d400000U

struct fajita_disp {
	uint32_t gdsc, cfg_gdsc, mdp_cbcr, ahb_cbcr;
	int ctl, pp, pipe;
	uint32_t src, ystride;
	uint32_t ctl_top, sync_cfg, ar_before, ar_after;
};

int fajita_disp_autorefresh(struct fajita_disp *d);
void fajita_disp_kick(const struct fajita_disp *d);
uint32_t fajita_disp_line(const struct fajita_disp *d);
