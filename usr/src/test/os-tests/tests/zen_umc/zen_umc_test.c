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
 * Test the memory decoding and normalization features at the heart of the
 * zen_umc(4D) driver.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <err.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

#include "zen_umc_test.h"


/*
 * Every successful forward decode should be reversible: given the channel and
 * normalized address that we ended up with, we should be able to get back to
 * the original physical address.
 */
static boolean_t
zen_umc_test_roundtrip(const umc_decode_test_t *test,
    const zen_umc_decoder_t *dec)
{
	zen_umc_decoder_t rev;

	memset(&rev, '\0', sizeof (rev));
	if (!zen_umc_decode_norm_addr(test->udt_umc, dec->dec_umc_chan,
	    dec->dec_norm_addr, &rev)) {
		(void) printf("\tround trip of normal address 0x%" PRIx64
		    " failed with error '%s' (%s/0x%x), data 0x%" PRIx64 "\n",
		    dec->dec_norm_addr, zen_umc_decode_strerror(rev.dec_fail),
		    zen_umc_decode_strenum(rev.dec_fail), rev.dec_fail,
		    rev.dec_fail_data);
		return (B_FALSE);
	}

	if (rev.dec_pa != test->udt_pa) {
		(void) printf("\tround trip physical address mismatch\n"
		    "\t\texpected 0x%" PRIx64 "\n\t\tfound    0x%" PRIx64 "\n",
		    test->udt_pa, rev.dec_pa);
		return (B_FALSE);
	}

	(void) printf("\tTEST PASSED: Round-trip normal address\n");
	return (B_TRUE);
}

static boolean_t
zen_umc_test_fabric_one(const umc_fabric_test_t *test)
{
	boolean_t ret = B_TRUE;

	(void) printf("Running test: %s\n", test->uft_desc);
	if (test->uft_compose) {
		uint32_t fab, sock, die, comp;
		boolean_t rtt = B_TRUE;
		boolean_t valid;

		valid = zen_fabric_id_valid_parts(test->uft_decomp,
		    test->uft_sock_id, test->uft_die_id, test->uft_comp_id);
		if (!valid) {
			if (test->uft_valid) {
				(void) printf("\tInvalid fabric ID parts "
				    "found\n");
				return (B_FALSE);
			}

			(void) printf("\tTEST PASSED: Invalid Fabric parts "
			    "detected\n");
			return (B_TRUE);
		} else {
			if (!test->uft_valid) {
				(void) printf("\tFabric ID parts validated, "
				    "but expected failure\n");
				return (B_FALSE);
			}
		}
		zen_fabric_id_compose(test->uft_decomp, test->uft_sock_id,
		    test->uft_die_id, test->uft_comp_id, &fab);
		if (fab != test->uft_fabric_id) {
			(void) printf("\tFabric ID mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->uft_fabric_id, fab);
			ret = B_FALSE;
		} else {
			(void) printf("\tTEST PASSED: Fabric ID composition\n");
		}

		zen_fabric_id_decompose(test->uft_decomp, fab, &sock, &die,
		    &comp);
		if (sock != test->uft_sock_id) {
			(void) printf("\tRound-trip socket mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_sock_id, sock);
			ret = rtt = B_FALSE;
		}

		if (die != test->uft_die_id) {
			(void) printf("\tRound-trip die mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_die_id, die);
			ret = rtt = B_FALSE;
		}

		if (comp != test->uft_comp_id) {
			(void) printf("\tRound-trip comp mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_comp_id, comp);
			ret = rtt = B_FALSE;
		}

		if (rtt) {
			(void) printf("\tTEST PASSED: Round-trip Fabric ID "
			    "decomposition\n");
		}
	} else {
		uint32_t fab, sock, die, comp;
		boolean_t valid;

		valid = zen_fabric_id_valid_fabid(test->uft_decomp,
		    test->uft_fabric_id);
		if (!valid) {
			if (test->uft_valid) {
				(void) printf("\tInvalid fabric ID found\n");
				return (B_FALSE);
			}

			(void) printf("\tTEST PASSED: Successfully found "
			    "invalid fabric ID\n");
			return (B_TRUE);
		} else {
			if (!test->uft_valid) {
				(void) printf("\tFabric ID validated, "
				    "but expected to find an invalid one\n");
				return (B_FALSE);
			}
		}
		zen_fabric_id_decompose(test->uft_decomp, test->uft_fabric_id,
		    &sock, &die, &comp);
		if (sock != test->uft_sock_id) {
			(void) printf("\tsocket mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_sock_id, sock);
			ret = B_FALSE;
		}

		if (die != test->uft_die_id) {
			(void) printf("\tdie mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_die_id, die);
			ret = B_FALSE;
		}

		if (comp != test->uft_comp_id) {
			(void) printf("\tcomp mismatch\n"
			    "\t\texpected %u\n\t\tfound %u\n",
			    test->uft_comp_id, comp);
			ret = B_FALSE;
		}

		if (ret) {
			(void) printf("\tTEST PASSED: Fabric ID "
			    "Decomposition\n");
		}

		zen_fabric_id_compose(test->uft_decomp, sock, die, comp, &fab);
		if (fab != test->uft_fabric_id) {
			(void) printf("\tFabric ID mismatch on round trip\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->uft_fabric_id, fab);
			ret = B_FALSE;
		} else {
			(void) printf("\tTEST PASSED: Round-trip Fabric ID "
			    "composition\n");
		}
	}

	return (ret);
}

static boolean_t
zen_umc_test_decode_one(const umc_decode_test_t *test)
{
	boolean_t pass;
	zen_umc_decoder_t dec;

	(void) printf("Running test: %s\n", test->udt_desc);
	(void) printf("\tDecoding address: 0x%" PRIx64 "\n", test->udt_pa);
	memset(&dec, '\0', sizeof (dec));

	pass = zen_umc_decode_pa(test->udt_umc, test->udt_pa, &dec);
	if (pass && !test->udt_pass) {
		uint32_t sock, die, comp;

		zen_fabric_id_decompose(&test->udt_umc->umc_decomp,
		    dec.dec_targ_fabid, &sock, &die, &comp);

		(void) printf("\tdecode unexpectedly succeeded\n");
		(void) printf("\texpected error '%s' (%s/0x%x)\n",
		    zen_umc_decode_strerror(test->udt_fail),
		    zen_umc_decode_strenum(test->udt_fail),
		    test->udt_fail);
		(void) printf("\t\tdecoded socket: 0x%x\n", sock);
		(void) printf("\t\tdecoded die: 0x%x\n", die);
		(void) printf("\t\tdecoded component: 0x%x\n", comp);
		(void) printf("\t\tnormal address: 0x%" PRIx64 "\n",
		    dec.dec_norm_addr);
		(void) printf("\t\tdecoded dimm: 0x%x\n", dec.dec_dimm_no);
		(void) printf("\t\tdecoded row: 0x%x\n", dec.dec_dimm_row);
		(void) printf("\t\tdecoded column: 0x%x\n", dec.dec_dimm_col);
		(void) printf("\t\tdecoded bank: 0x%x\n", dec.dec_dimm_bank);
		(void) printf("\t\tdecoded bank group: 0x%x\n",
		    dec.dec_dimm_bank_group);
		(void) printf("\t\tdecoded rm: 0x%x\n", dec.dec_dimm_rm);
		(void) printf("\t\tdecoded cs: 0x%x\n", dec.dec_dimm_csno);
		(void) printf("\ttest failed\n");
		return (B_FALSE);
	} else if (pass) {
		uint32_t sock, die, comp;
		boolean_t success = B_TRUE;

		zen_fabric_id_decompose(&test->udt_umc->umc_decomp,
		    dec.dec_targ_fabid, &sock, &die, &comp);
		if (test->udt_sock != UINT8_MAX &&
		    test->udt_sock != sock) {
			(void) printf("\tsocket mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_sock, sock);
			success = B_FALSE;
		}

		if (test->udt_die != UINT8_MAX &&
		    test->udt_die != die) {
			(void) printf("\tdie mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_die, die);
			success = B_FALSE;
		}

		if (test->udt_comp != UINT8_MAX &&
		    test->udt_comp != comp) {
			(void) printf("\tcomp mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_comp, comp);
			success = B_FALSE;
		}

		if (test->udt_norm_addr != UINT64_MAX &&
		    test->udt_norm_addr != dec.dec_norm_addr) {
			(void) printf("\tnormalized address mismatch\n"
			    "\t\texpected 0x%" PRIx64 "\n"
			    "\t\tfound    0x%" PRIx64 "\n",
			    test->udt_norm_addr, dec.dec_norm_addr);
			success = B_FALSE;
		}

		if (test->udt_dimm_no != UINT32_MAX &&
		    test->udt_dimm_no != dec.dec_dimm_no) {
			(void) printf("\tDIMM number mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_no, dec.dec_dimm_no);
			success = B_FALSE;
		}

		if (test->udt_dimm_col != UINT32_MAX &&
		    test->udt_dimm_col != dec.dec_dimm_col) {
			(void) printf("\tcolumn mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_col, dec.dec_dimm_col);
			success = B_FALSE;
		}

		if (test->udt_dimm_row != UINT32_MAX &&
		    test->udt_dimm_row != dec.dec_dimm_row) {
			(void) printf("\trow mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_row, dec.dec_dimm_row);
			success = B_FALSE;
		}

		if (test->udt_dimm_bank != UINT8_MAX &&
		    test->udt_dimm_bank != dec.dec_dimm_bank) {
			(void) printf("\tbank mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_bank, dec.dec_dimm_bank);
			success = B_FALSE;
		}

		if (test->udt_dimm_bank_group != UINT8_MAX &&
		    test->udt_dimm_bank_group != dec.dec_dimm_bank_group) {
			(void) printf("\tbank group mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_bank_group, dec.dec_dimm_bank_group);
			success = B_FALSE;
		}

		if (test->udt_dimm_subchan != UINT8_MAX &&
		    test->udt_dimm_subchan != dec.dec_dimm_subchan) {
			(void) printf("\tsub-channel mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_subchan, dec.dec_dimm_subchan);
			success = B_FALSE;
		}

		if (test->udt_dimm_rm != UINT8_MAX &&
		    test->udt_dimm_rm != dec.dec_dimm_rm) {
			(void) printf("\tRM mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_rm, dec.dec_dimm_rm);
			success = B_FALSE;
		}

		if (test->udt_dimm_cs != UINT8_MAX &&
		    test->udt_dimm_cs != dec.dec_dimm_csno) {
			(void) printf("\tCS mismatch\n"
			    "\t\texpected 0x%x\n\t\tfound    0x%x\n",
			    test->udt_dimm_cs, dec.dec_dimm_csno);
			success = B_FALSE;
		}

		if (success) {
			(void) printf("\tTEST PASSED: Successfully decoded "
			    "PA\n");
			success = zen_umc_test_roundtrip(test, &dec);
		}

		if (!success) {
			(void) printf("\tTEST FAILED!\n");
		}
		return (success);
	} else if (!pass && !test->udt_pass) {
		if (dec.dec_fail != test->udt_fail) {
			(void) printf("\terror mismatch\n"
			    "\t\texpected '%s' (%s/0x%x)\n"
			    "\t\tfound '%s' (%s/0x%x)\n",
			    zen_umc_decode_strerror(test->udt_fail),
			    zen_umc_decode_strenum(test->udt_fail),
			    test->udt_fail,
			    zen_umc_decode_strerror(dec.dec_fail),
			    zen_umc_decode_strenum(dec.dec_fail),
			    dec.dec_fail);
			return (B_FALSE);
		}

		(void) printf("\tTEST PASSED: Correct error generated\n");
		return (B_TRUE);
	} else {
		(void) printf("\tdecode failed with error '%s' (%s/0x%x)\n",
		    zen_umc_decode_strerror(dec.dec_fail),
		    zen_umc_decode_strenum(dec.dec_fail),
		    dec.dec_fail);

		if (test->udt_norm_addr != UINT64_MAX) {
			(void) printf("\t\texpected normal address: "
			    "0x%" PRIx64 "\n", test->udt_norm_addr);
		}

		if (test->udt_sock != UINT8_MAX) {
			(void) printf("\t\texpected socket: 0x%x\n",
			    test->udt_sock);
		}

		if (test->udt_die != UINT8_MAX) {
			(void) printf("\t\texpected die: 0x%x\n",
			    test->udt_die);
		}

		if (test->udt_comp != UINT8_MAX) {
			(void) printf("\t\texpected comp: 0x%x\n",
			    test->udt_comp);
		}

		if (test->udt_dimm_no != UINT32_MAX) {
			(void) printf("\t\texpected DIMM number: 0x%x\n",
			    test->udt_dimm_no);
		}

		if (test->udt_dimm_col != UINT32_MAX) {
			(void) printf("\t\texpected column: 0x%x\n",
			    test->udt_dimm_col);
		}

		if (test->udt_dimm_row != UINT32_MAX) {
			(void) printf("\t\texpected row: 0x%x\n",
			    test->udt_dimm_row);
		}

		if (test->udt_dimm_bank != UINT8_MAX) {
			(void) printf("\t\texpected bank: 0x%x\n",
			    test->udt_dimm_bank);
		}

		if (test->udt_dimm_bank_group != UINT8_MAX) {
			(void) printf("\t\texpected bank group: 0x%x\n",
			    test->udt_dimm_bank_group);
		}

		if (test->udt_dimm_subchan != UINT8_MAX) {
			(void) printf("\t\texpected sub-channel: 0x%x\n",
			    test->udt_dimm_subchan);
		}

		if (test->udt_dimm_rm != UINT8_MAX) {
			(void) printf("\t\texpected RM: 0x%x\n",
			    test->udt_dimm_rm);
		}

		if (test->udt_dimm_cs != UINT8_MAX) {
			(void) printf("\t\texpected CS: 0x%x\n",
			    test->udt_dimm_cs);
		}

		return (B_FALSE);
	}
}

static void
zen_umc_test_fabric(const umc_fabric_test_t *tests, uint_t *ntests,
    uint_t *nfail)
{
	for (uint_t i = 0; tests[i].uft_desc != NULL; i++) {
		if (!zen_umc_test_fabric_one(&tests[i]))
			*nfail += 1;
		*ntests += 1;
	}
}

static void
zen_umc_test_decode(const umc_decode_test_t *tests, uint_t *ntests,
    uint_t *nfail)
{
	for (uint_t i = 0; tests[i].udt_desc != NULL; i++) {
		if (!zen_umc_test_decode_one(&tests[i]))
			*nfail += 1;
		*ntests += 1;
	}
}

static boolean_t
zen_umc_test_norm_one(const umc_norm_test_t *test)
{
	boolean_t pass;
	zen_umc_decoder_t dec;
	const zen_umc_chan_t *chan;

	(void) printf("Running test: %s\n", test->unt_desc);
	(void) printf("\tDecoding normal address: 0x%" PRIx64 " on %u/%u/%u\n",
	    test->unt_norm, test->unt_sock, test->unt_die, test->unt_chan);

	chan = zen_umc_find_chan_by_id(test->unt_umc, test->unt_sock,
	    test->unt_die, test->unt_chan);
	if (chan == NULL) {
		if (test->unt_pass ||
		    test->unt_fail != ZEN_UMC_DECODE_F_CANNOT_MAP_FABID) {
			(void) printf("\tfailed to find channel\n");
			return (B_FALSE);
		}

		(void) printf("\tTEST PASSED: Correctly failed to find "
		    "channel\n");
		return (B_TRUE);
	} else if (!test->unt_pass &&
	    test->unt_fail == ZEN_UMC_DECODE_F_CANNOT_MAP_FABID) {
		(void) printf("\tunexpectedly found channel\n");
		return (B_FALSE);
	}

	memset(&dec, '\0', sizeof (dec));
	pass = zen_umc_decode_norm_addr(test->unt_umc, chan, test->unt_norm,
	    &dec);
	if (pass && !test->unt_pass) {
		(void) printf("\tdecode unexpectedly succeeded\n");
		(void) printf("\texpected error '%s' (%s/0x%x)\n",
		    zen_umc_decode_strerror(test->unt_fail),
		    zen_umc_decode_strenum(test->unt_fail),
		    test->unt_fail);
		(void) printf("\t\tdecoded physical address: 0x%" PRIx64 "\n",
		    dec.dec_pa);
		return (B_FALSE);
	} else if (!pass && test->unt_pass) {
		(void) printf("\tdecode failed with error '%s' (%s/0x%x), "
		    "data 0x%" PRIx64 "\n",
		    zen_umc_decode_strerror(dec.dec_fail),
		    zen_umc_decode_strenum(dec.dec_fail), dec.dec_fail,
		    dec.dec_fail_data);
		return (B_FALSE);
	} else if (!pass && dec.dec_fail != test->unt_fail) {
		(void) printf("\terror mismatch\n"
		    "\t\texpected '%s' (%s/0x%x)\n"
		    "\t\tfound '%s' (%s/0x%x)\n",
		    zen_umc_decode_strerror(test->unt_fail),
		    zen_umc_decode_strenum(test->unt_fail),
		    test->unt_fail,
		    zen_umc_decode_strerror(dec.dec_fail),
		    zen_umc_decode_strenum(dec.dec_fail),
		    dec.dec_fail);
		return (B_FALSE);
	}

	if (test->unt_pa != UINT64_MAX && test->unt_pa != dec.dec_pa) {
		(void) printf("\tphysical address mismatch\n"
		    "\t\texpected 0x%" PRIx64 "\n\t\tfound    0x%" PRIx64 "\n",
		    test->unt_pa, dec.dec_pa);
		return (B_FALSE);
	}

	if (pass) {
		(void) printf("\tTEST PASSED: Successfully decoded normal "
		    "address\n");
	} else {
		(void) printf("\tTEST PASSED: Correct error generated\n");
	}
	return (B_TRUE);
}

static void
zen_umc_test_norm_run(const umc_norm_test_t *tests, uint_t *ntests,
    uint_t *nfail)
{
	for (uint_t i = 0; tests[i].unt_desc != NULL; i++) {
		if (!zen_umc_test_norm_one(&tests[i]))
			*nfail += 1;
		*ntests += 1;
	}
}

typedef struct zen_umc_test_set {
	const char *set_name;
	const umc_decode_test_t *set_test;
} zen_umc_test_set_t;

static const zen_umc_test_set_t zen_umc_test_set[] = {
	{ "basic", zen_umc_test_basics },
	{ "channel", zen_umc_test_chans },
	{ "cod", zen_umc_test_cod },
	{ "errors", zen_umc_test_errors },
	{ "hole", zen_umc_test_hole },
	{ "ilv", zen_umc_test_ilv },
	{ "multi", zen_umc_test_multi },
	{ "nps", zen_umc_test_nps },
	{ "remap", zen_umc_test_remap },
	{ "nps_k", zen_umc_test_nps_k },
	{ "np2_k", zen_umc_test_np2_k },
};

static void
zen_umc_test_selected(int argc, char *argv[], uint_t *ntests, uint_t *nfail)
{
	for (int i = 1; i < argc; i++) {
		boolean_t ran = B_FALSE;

		if (strcmp(argv[i], "fabric_ids") == 0) {
			zen_umc_test_fabric(zen_umc_test_fabric_ids, ntests,
			    nfail);
			continue;
		}

		if (strcmp(argv[i], "norm") == 0) {
			zen_umc_test_norm_run(zen_umc_test_norm, ntests,
			    nfail);
			continue;
		}

		for (uint_t t = 0; t < ARRAY_SIZE(zen_umc_test_set); t++) {
			const zen_umc_test_set_t *s = &zen_umc_test_set[t];

			if (strcmp(s->set_name, argv[i]) == 0) {
				zen_umc_test_decode(s->set_test, ntests, nfail);
				ran = B_TRUE;
				break;
			}
		}

		if (!ran) {
			errx(EXIT_FAILURE, "Unknown test suite: %s", argv[i]);
		}
	}
}

int
main(int argc, char *argv[])
{
	uint_t ntests = 0, nfail = 0;

	if (argc > 1) {
		zen_umc_test_selected(argc, argv, &ntests, &nfail);
	} else {
		zen_umc_test_fabric(zen_umc_test_fabric_ids, &ntests, &nfail);
		for (uint_t i = 0; i < ARRAY_SIZE(zen_umc_test_set); i++) {
			zen_umc_test_decode(zen_umc_test_set[i].set_test,
			    &ntests, &nfail);
		}
		zen_umc_test_norm_run(zen_umc_test_norm, &ntests, &nfail);
	}
	(void) printf("%u/%u tests passed\n", ntests - nfail, ntests);
	return (nfail > 0);
}
