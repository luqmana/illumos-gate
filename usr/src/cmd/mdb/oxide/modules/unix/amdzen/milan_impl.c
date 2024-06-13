/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2024 Oxide Computer Company
 */

/*
 * Shared Milan DF and UMC data.
 */

#include "milan_impl.h"


const char *milan_chan_ileaves[16] = {
	"1", "2", "Reserved", "4",
	"Reserved", "8", "6", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"COD-4 2", "COD-2 4", "COD-1 8", "Reserved"
};

const char *milan_chan_map[8] = {
	[0] = "A",
	[1] = "B",
	[2] = "D",
	[3] = "C",
	[4] = "H",
	[5] = "G",
	[6] = "E",
	[7] = "F",
};

const uint8_t milan_chan_umc_order[8] = { 0, 1, 3, 2, 6, 7, 5, 4 };

df_comp_t milan_comps[43] = {
	{ 0, "UMC0", 2 },
	{ 1, "UMC1", 2 },
	{ 2, "UMC2", 2 },
	{ 3, "UMC3", 2 },
	{ 4, "UMC4", 2 },
	{ 5, "UMC5", 2 },
	{ 6, "UMC6", 2 },
	{ 7, "UMC7", 2 },
	{ 8, "CCIX0", 2 },
	{ 9, "CCIX1", 2 },
	{ 10, "CCIX2", 2 },
	{ 11, "CCIX3", 2 },
	{ 16, "CCM0", 16 },
	{ 17, "CCM1", 16 },
	{ 18, "CCM2", 16 },
	{ 19, "CCM3", 16 },
	{ 20, "CCM4", 16 },
	{ 21, "CCM5", 16 },
	{ 22, "CCM6", 16 },
	{ 23, "CCM7", 16 },
	{ 24, "IOMS0", 16 },
	{ 25, "IOMS1", 16 },
	{ 26, "IOMS2", 16 },
	{ 27, "IOMS3", 16 },
	{ 30, "PIE0", 8 },
	{ 31, "CAKE0", 0, B_TRUE },
	{ 32, "CAKE1", 0, B_TRUE },
	{ 33, "CAKE2", 0, B_TRUE },
	{ 34, "CAKE3", 0, B_TRUE },
	{ 35, "CAKE4", 0, B_TRUE },
	{ 36, "CAKE5", 0, B_TRUE },
	{ 37, "TCDX0", 0, B_TRUE },
	{ 38, "TCDX1", 0, B_TRUE },
	{ 39, "TCDX2", 0, B_TRUE },
	{ 40, "TCDX3", 0, B_TRUE },
	{ 41, "TCDX4", 0, B_TRUE },
	{ 42, "TCDX5", 0, B_TRUE },
	{ 43, "TCDX6", 0, B_TRUE },
	{ 44, "TCDX7", 0, B_TRUE },
	{ 45, "TCDX8", 0, B_TRUE },
	{ 46, "TCDX9", 0, B_TRUE },
	{ 47, "TCDX10", 0, B_TRUE },
	{ 48, "TCDX11", 0, B_TRUE }
};
