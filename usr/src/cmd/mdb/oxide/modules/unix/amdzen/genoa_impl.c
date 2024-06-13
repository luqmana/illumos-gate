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
 * Shared Genoa DF and UMC data.
 */

#include "genoa_impl.h"


const char *genoa_chan_ileaves[32] = {
	"1", "2", "Reserved", "4",
	"Reserved", "8", "Reserved", "16",
	"32", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"NPS-4 2", "NPS-2 4", "NPS-1 8", "NPS-4 3",
	"NPS-2 6", "NPS-1 12", "NPS-2 5", "NPS-1 10",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
};

const char *genoa_chan_map[12] = {
	[0] = "C",
	[1] = "E",
	[2] = "F",
	[3] = "A",
	[4] = "B",
	[5] = "D",
	[6] = "I",
	[7] = "K",
	[8] = "L",
	[9] = "G",
	[10] = "H",
	[11] = "J",
};

const uint8_t genoa_chan_umc_order[12] = {
    3, 4, 0, 5, 1, 2, 9, 10, 6, 11, 7, 8
};

df_comp_t genoa_comps[97] = {
	{ 0, "UMC0", 4 },
	{ 1, "UMC1", 4 },
	{ 2, "UMC2", 4 },
	{ 3, "UMC3", 4 },
	{ 4, "UMC4", 4 },
	{ 5, "UMC5", 4 },
	{ 6, "UMC6", 4 },
	{ 7, "UMC7", 4 },
	{ 8, "UMC8", 4 },
	{ 9, "UMC9", 4 },
	{ 10, "UMC10", 4 },
	{ 11, "UMC11", 4 },
	{ 12, "CMP0", 4 },
	{ 13, "CMP1", 4 },
	{ 14, "CMP2", 4 },
	{ 15, "CMP3", 4 },
	{ 16, "CCM0", 20 },
	{ 17, "CCM1", 20 },
	{ 18, "CCM2", 20 },
	{ 19, "CCM3", 20 },
	{ 20, "CCM4", 20 },
	{ 21, "CCM5", 20 },
	{ 22, "CCM6", 20 },
	{ 23, "CCM7", 20 },
	{ 24, "ACM0", 20 },
	{ 25, "ACM1", 20 },
	{ 26, "ACM2", 20 },
	{ 27, "ACM3", 20 },
	{ 28, "NCM0_IOMMU0", 20 },
	{ 29, "NCM1_IOMMU1", 20 },
	{ 30, "NCM2_IOMMU2", 20 },
	{ 31, "NCM3_IOMMU3", 20 },
	{ 32, "IOM0_IOHUBM0", 20 },
	{ 33, "IOM1_IOHUBM1", 20 },
	{ 34, "IOM2_IOHUBM2", 20 },
	{ 35, "IOM3_IOHUBM3", 20 },
	{ 36, "IOHUBS0", 1 },
	{ 37, "IOHUBS1", 1 },
	{ 38, "IOHUBS2", 1 },
	{ 39, "IOHUBS3", 1 },
	{ 40, "ICNG0" },
	{ 41, "ICNG1" },
	{ 42, "ICNG2" },
	{ 43, "ICNG3" },
	{ 44, "PIE0", 20 },
	{ 45, "CAKE0", 0, B_TRUE },
	{ 46, "CAKE1", 0, B_TRUE },
	{ 47, "CAKE2", 0, B_TRUE },
	{ 48, "CAKE3", 0, B_TRUE },
	{ 49, "CAKE4", 0, B_TRUE },
	{ 50, "CAKE5", 0, B_TRUE },
	{ 51, "CAKE6", 0, B_TRUE },
	{ 52, "CAKE7", 0, B_TRUE },
	{ 53, "CNLI0", 0, B_TRUE },
	{ 54, "CNLI1", 0, B_TRUE },
	{ 55, "CNLI2", 0, B_TRUE },
	{ 56, "CNLI3", 0, B_TRUE },
	{ 57, "PFX0", 0, B_TRUE },
	{ 58, "PFX1", 0, B_TRUE },
	{ 59, "PFX2", 0, B_TRUE },
	{ 60, "PFX3", 0, B_TRUE },
	{ 61, "PFX4", 0, B_TRUE },
	{ 62, "PFX5", 0, B_TRUE },
	{ 63, "PFX6", 0, B_TRUE },
	{ 64, "PFX7", 0, B_TRUE },
	{ 65, "SPF0", 8, B_TRUE },
	{ 66, "SPF1", 8, B_TRUE },
	{ 67, "SPF2", 8, B_TRUE },
	{ 68, "SPF3", 8, B_TRUE },
	{ 69, "SPF4", 8, B_TRUE },
	{ 70, "SPF5", 8, B_TRUE },
	{ 71, "SPF6", 8, B_TRUE },
	{ 72, "SPF7", 8, B_TRUE },
	{ 73, "SPF8", 8, B_TRUE },
	{ 74, "SPF9", 8, B_TRUE },
	{ 75, "SPF10", 8, B_TRUE },
	{ 76, "SPF11", 8, B_TRUE },
	{ 77, "SPF12", 8, B_TRUE },
	{ 78, "SPF13", 8, B_TRUE },
	{ 79, "SPF14", 8, B_TRUE },
	{ 80, "SPF15", 8, B_TRUE },
	{ 81, "TCDX0", 0, B_TRUE },
	{ 82, "TCDX1", 0, B_TRUE },
	{ 83, "TCDX2", 0, B_TRUE },
	{ 84, "TCDX3", 0, B_TRUE },
	{ 85, "TCDX4", 0, B_TRUE },
	{ 86, "TCDX5", 0, B_TRUE },
	{ 87, "TCDX6", 0, B_TRUE },
	{ 88, "TCDX7", 0, B_TRUE },
	{ 89, "TCDX8", 0, B_TRUE },
	{ 90, "TCDX9", 0, B_TRUE },
	{ 91, "TCDX10", 0, B_TRUE },
	{ 92, "TCDX11", 0, B_TRUE },
	{ 93, "TCDX12", 0, B_TRUE },
	{ 94, "TCDX13", 0, B_TRUE },
	{ 95, "TCDX14", 0, B_TRUE },
	{ 96, "TCDX15", 0, B_TRUE }
};
