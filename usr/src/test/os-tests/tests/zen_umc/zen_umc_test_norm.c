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
 * Copyright 2026 Oxide Computer Company
 */

/*
 * Tests for transforming a normalized (channel) address back into a system
 * address. The bulk of the coverage for this comes from the round-trip check
 * that the test harness performs after every successful forward decode in the
 * other test suites. The tests here focus on the things that can only happen
 * when starting from a channel address: channels that don't exist, channels
 * that aren't targets of a rule, addresses outside of any rule, and channel
 * addresses that don't correspond to a chip-select.
 *
 * All of the configurations here use a simple 2-channel non-hashed interleave
 * starting at bit 8 unless otherwise noted, with 4 GiB of DDR4 in each channel.
 */

#include "zen_umc_test.h"

#define	NORM_TEST_CS(mask)	{ \
	.ucs_flags = UMC_CS_F_DECODE_EN, \
	.ucs_base = { \
		.udb_base = 0, \
		.udb_valid = B_TRUE \
	}, \
	.ucs_base_mask = (mask), \
	.ucs_nbanks = 0x4, \
	.ucs_ncol = 0xa, \
	.ucs_nrow_lo = 0x11, \
	.ucs_nbank_groups = 0x2, \
	.ucs_row_hi_bit = 0x18, \
	.ucs_row_low_bit = 0x11, \
	.ucs_bank_bits = { 0xf, 0x10, 0xd, 0xe }, \
	.ucs_col_bits = { 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc } \
}

#define	NORM_TEST_DIMM(mask)	{ \
	.ud_flags = UMC_DIMM_F_VALID, \
	.ud_width = UMC_DIMM_W_X4, \
	.ud_kind = UMC_DIMM_K_RDIMM, \
	.ud_dimmno = 0, \
	.ud_cs = { NORM_TEST_CS(mask) } \
}

#define	NORM_TEST_RULE_2CH(fabid)	{ \
	.ddr_flags = DF_DRAM_F_VALID, \
	.ddr_base = 0, \
	.ddr_limit = 8ULL * 1024ULL * 1024ULL * 1024ULL, \
	.ddr_dest_fabid = (fabid), \
	.ddr_sock_ileave_bits = 0, \
	.ddr_die_ileave_bits = 0, \
	.ddr_addr_start = 8, \
	.ddr_chan_ileave = DF_CHAN_ILEAVE_2CH \
}

#define	NORM_TEST_CHAN_2CH(fabid, logid, mask)	{ \
	.chan_flags = UMC_CHAN_F_ECC_EN, \
	.chan_fabid = (fabid), \
	.chan_instid = (fabid), \
	.chan_logid = (logid), \
	.chan_nrules = 1, \
	.chan_type = UMC_DIMM_T_DDR4, \
	.chan_rules = { NORM_TEST_RULE_2CH(0) }, \
	.chan_dimms = { NORM_TEST_DIMM(mask) } \
}

#define	NORM_TEST_DECOMP	{ \
	.dfd_sock_mask = 0x01, \
	.dfd_die_mask = 0x00, \
	.dfd_node_mask = 0x20, \
	.dfd_comp_mask = 0x1f, \
	.dfd_sock_shift = 0, \
	.dfd_die_shift = 0, \
	.dfd_node_shift = 5, \
	.dfd_comp_shift = 0 \
}

/*
 * The basic 2-channel configuration.
 */
static const zen_umc_t zen_umc_norm_2ch = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 2,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH(0) },
		.zud_chan = {
			NORM_TEST_CHAN_2CH(0, 0, 0x3ffffffff),
			NORM_TEST_CHAN_2CH(1, 1, 0x3ffffffff)
		}
	} }
};

/*
 * Same as the above, but with a third channel that is not a target of the
 * 2-channel interleave.
 */
static const zen_umc_t zen_umc_norm_3ch = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 3,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH(0) },
		.zud_chan = {
			NORM_TEST_CHAN_2CH(0, 0, 0x3ffffffff),
			NORM_TEST_CHAN_2CH(1, 1, 0x3ffffffff),
			NORM_TEST_CHAN_2CH(2, 2, 0x3ffffffff)
		}
	} }
};

/*
 * Here the rule's destination fabric ID is above the only channel that exists,
 * so that channel can never be a target.
 */
static const zen_umc_t zen_umc_norm_dest = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 1,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH(1) },
		.zud_chan = { {
			.chan_flags = UMC_CHAN_F_ECC_EN,
			.chan_fabid = 0,
			.chan_instid = 0,
			.chan_logid = 0,
			.chan_nrules = 1,
			.chan_type = UMC_DIMM_T_DDR4,
			.chan_rules = { NORM_TEST_RULE_2CH(1) },
			.chan_dimms = { NORM_TEST_DIMM(0x3ffffffff) }
		} }
	} }
};

/*
 * A channel with chip-selects that only cover 256 MiB. Normal addresses beyond
 * that can be turned back into a system address, but the forward decode will
 * fail to find a chip-select.
 */
static const zen_umc_t zen_umc_norm_small_cs = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 2,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH(0) },
		.zud_chan = {
			NORM_TEST_CHAN_2CH(0, 0, 0xfffffff),
			NORM_TEST_CHAN_2CH(1, 1, 0xfffffff)
		}
	} }
};

/*
 * A single channel whose first UMC rule is disabled and whose second rule
 * (starting at 4 GiB in both system and normalized address space) is enabled.
 * The CCM's first rule points at a channel that does not exist.
 */
static const zen_umc_t zen_umc_norm_rule1 = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 2,
		.zud_nchan = 1,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { {
			.ddr_flags = DF_DRAM_F_VALID,
			.ddr_base = 0,
			.ddr_limit = 4ULL * 1024ULL * 1024ULL * 1024ULL,
			.ddr_dest_fabid = 5,
			.ddr_sock_ileave_bits = 0,
			.ddr_die_ileave_bits = 0,
			.ddr_addr_start = 8,
			.ddr_chan_ileave = DF_CHAN_ILEAVE_1CH
		}, {
			.ddr_flags = DF_DRAM_F_VALID,
			.ddr_base = 4ULL * 1024ULL * 1024ULL * 1024ULL,
			.ddr_limit = 8ULL * 1024ULL * 1024ULL * 1024ULL,
			.ddr_dest_fabid = 0,
			.ddr_sock_ileave_bits = 0,
			.ddr_die_ileave_bits = 0,
			.ddr_addr_start = 8,
			.ddr_chan_ileave = DF_CHAN_ILEAVE_1CH
		} },
		.zud_chan = { {
			.chan_flags = UMC_CHAN_F_ECC_EN,
			.chan_fabid = 0,
			.chan_instid = 0,
			.chan_logid = 0,
			.chan_nrules = 2,
			.chan_type = UMC_DIMM_T_DDR4,
			.chan_rules = { {
				.ddr_flags = 0
			}, {
				.ddr_flags = DF_DRAM_F_VALID,
				.ddr_base = 4ULL * 1024ULL * 1024ULL * 1024ULL,
				.ddr_limit = 8ULL * 1024ULL * 1024ULL * 1024ULL,
				.ddr_dest_fabid = 0,
				.ddr_sock_ileave_bits = 0,
				.ddr_die_ileave_bits = 0,
				.ddr_addr_start = 8,
				.ddr_chan_ileave = DF_CHAN_ILEAVE_1CH
			} },
			.chan_offsets = { {
				.cho_valid = B_TRUE,
				.cho_offset = 4ULL * 1024ULL * 1024ULL * 1024ULL
			} },
			.chan_dimms = { NORM_TEST_DIMM(0x3ffffffff) }
		} }
	} }
};

/*
 * A channel whose DRAM rule does not line up with any rule in the CCM.
 */
static const zen_umc_t zen_umc_norm_nodf = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_3,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 1,
		.zud_cs_nremap = 0,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH(0) },
		.zud_chan = { {
			.chan_flags = UMC_CHAN_F_ECC_EN,
			.chan_fabid = 0,
			.chan_instid = 0,
			.chan_logid = 0,
			.chan_nrules = 1,
			.chan_type = UMC_DIMM_T_DDR4,
			.chan_rules = { {
				.ddr_flags = DF_DRAM_F_VALID,
				.ddr_base = 0,
				.ddr_limit = 4ULL * 1024ULL * 1024ULL * 1024ULL,
				.ddr_dest_fabid = 0,
				.ddr_sock_ileave_bits = 0,
				.ddr_die_ileave_bits = 0,
				.ddr_addr_start = 8,
				.ddr_chan_ileave = DF_CHAN_ILEAVE_2CH
			} },
			.chan_dimms = { NORM_TEST_DIMM(0x3ffffffff) }
		} }
	} }
};

/*
 * A 2-channel interleave where the logical channels 0 and 1 are remapped to
 * components 5 and 6. A third channel exists at component 7 which nothing maps
 * to.
 */
#define	NORM_TEST_RULE_2CH_REMAP	{ \
	.ddr_flags = DF_DRAM_F_VALID | DF_DRAM_F_REMAP_EN, \
	.ddr_base = 0, \
	.ddr_limit = 8ULL * 1024ULL * 1024ULL * 1024ULL, \
	.ddr_dest_fabid = 0, \
	.ddr_sock_ileave_bits = 0, \
	.ddr_die_ileave_bits = 0, \
	.ddr_addr_start = 8, \
	.ddr_remap_ent = 0, \
	.ddr_chan_ileave = DF_CHAN_ILEAVE_2CH \
}

#define	NORM_TEST_CHAN_2CH_REMAP(fabid, logid)	{ \
	.chan_flags = UMC_CHAN_F_ECC_EN, \
	.chan_fabid = (fabid), \
	.chan_instid = (fabid), \
	.chan_logid = (logid), \
	.chan_nrules = 1, \
	.chan_type = UMC_DIMM_T_DDR4, \
	.chan_rules = { NORM_TEST_RULE_2CH_REMAP }, \
	.chan_dimms = { NORM_TEST_DIMM(0x3ffffffff) } \
}

static const zen_umc_t zen_umc_norm_remap = {
	.umc_tom = 4ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_tom2 = 8ULL * 1024ULL * 1024ULL * 1024ULL,
	.umc_df_rev = DF_REV_4,
	.umc_decomp = NORM_TEST_DECOMP,
	.umc_ndfs = 1,
	.umc_dfs = { {
		.zud_dfno = 0,
		.zud_ccm_inst = 0,
		.zud_dram_nrules = 1,
		.zud_nchan = 3,
		.zud_cs_nremap = 1,
		.zud_hole_base = 0,
		.zud_rules = { NORM_TEST_RULE_2CH_REMAP },
		.zud_remap = { {
			.csr_nremaps = 2,
			.csr_remaps = { 5, 6 }
		} },
		.zud_chan = {
			NORM_TEST_CHAN_2CH_REMAP(5, 0),
			NORM_TEST_CHAN_2CH_REMAP(6, 1),
			NORM_TEST_CHAN_2CH_REMAP(7, 2)
		}
	} }
};

const umc_norm_test_t zen_umc_test_norm[] = { {
	.unt_desc = "2ch: channel 0, bit above interleave",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x2000
}, {
	.unt_desc = "2ch: channel 1, bit above interleave",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 1,
	.unt_norm = 0x1000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x2100
}, {
	.unt_desc = "2ch: channel 1, bits below interleave unchanged",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 1,
	.unt_norm = 0x123,
	.unt_pass = B_TRUE,
	.unt_pa = 0x323
}, {
	.unt_desc = "2ch: channel 0, address zero",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0,
	.unt_pass = B_TRUE,
	.unt_pa = 0
}, {
	.unt_desc = "2ch: channel 1, top of channel",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 1,
	.unt_norm = 0xffffffff,
	.unt_pass = B_TRUE,
	.unt_pa = 0x1ffffffff
}, {
	.unt_desc = "2ch: non-existent channel",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 2,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_CANNOT_MAP_FABID,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "2ch: non-existent socket",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 1,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_CANNOT_MAP_FABID,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "2ch: non-existent die",
	.unt_umc = &zen_umc_norm_2ch,
	.unt_sock = 0,
	.unt_die = 1,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_CANNOT_MAP_FABID,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "3ch: channel beyond interleave",
	.unt_umc = &zen_umc_norm_3ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 2,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NORM_ILEAVE_RULE_MISMATCH,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "3ch: channel within interleave still works",
	.unt_umc = &zen_umc_norm_3ch,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 1,
	.unt_norm = 0x1000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x2100
}, {
	.unt_desc = "dest: channel below rule destination",
	.unt_umc = &zen_umc_norm_dest,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NORM_FABID_RULE_MISMATCH,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "small cs: address within chip-select",
	.unt_umc = &zen_umc_norm_small_cs,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0xfffffff,
	.unt_pass = B_TRUE,
	.unt_pa = 0x1ffffeff
}, {
	.unt_desc = "small cs: address beyond chip-select still has PA",
	.unt_umc = &zen_umc_norm_small_cs,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x10000000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NO_CS_BASE_MATCH,
	.unt_pa = 0x20000000
}, {
	.unt_desc = "rule 1: address below offset has no rule",
	.unt_umc = &zen_umc_norm_rule1,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NORM_NO_UMC_RULE,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "rule 1: address at offset",
	.unt_umc = &zen_umc_norm_rule1,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x100000000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x100000000
}, {
	.unt_desc = "rule 1: address above offset",
	.unt_umc = &zen_umc_norm_rule1,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x100001000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x100001000
}, {
	.unt_desc = "no df rule: UMC rule doesn't match CCM",
	.unt_umc = &zen_umc_norm_nodf,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NORM_NO_DF_RULE,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = "remap: logical channel 0 (component 5)",
	.unt_umc = &zen_umc_norm_remap,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 0,
	.unt_norm = 0x1000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x2000
}, {
	.unt_desc = "remap: logical channel 1 (component 6)",
	.unt_umc = &zen_umc_norm_remap,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 1,
	.unt_norm = 0x1000,
	.unt_pass = B_TRUE,
	.unt_pa = 0x2100
}, {
	.unt_desc = "remap: component 7 is not remapped to",
	.unt_umc = &zen_umc_norm_remap,
	.unt_sock = 0,
	.unt_die = 0,
	.unt_chan = 2,
	.unt_norm = 0x1000,
	.unt_pass = B_FALSE,
	.unt_fail = ZEN_UMC_DECODE_F_NORM_NO_REMAP_ENTRY,
	.unt_pa = UINT64_MAX
}, {
	.unt_desc = NULL
} };
