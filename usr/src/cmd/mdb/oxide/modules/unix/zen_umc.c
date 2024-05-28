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
 * Shared Zen UMC data.
 */

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
