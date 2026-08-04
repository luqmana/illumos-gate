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

#ifndef _SYS_IO_FCH_RANGES_H
#define	_SYS_IO_FCH_RANGES_H

/*
 * The address space contract between fch(4D) and its parent nexus.
 *
 * The parent describes the address space it has routed to the FCH via a
 * "ranges" property on the fch node, an array of fch_rangespec_t; fch(4D)
 * consumes that grant and sub-allocates it to its children, whose "reg"
 * properties use the same format.  Both drivers must agree on these types
 * bit-for-bit, which is why they live here rather than in either driver.
 *
 * XXX fch_rangespec_t largely replicates pci_phys_spec but with different
 * addrsp semantics that could be made compatible if we really wanted to.  The
 * fr_addrsp member is really an fch_addrsp_t, but we define it as a uint32_t
 * to guarantee its size, which we rely upon for cramming these into DDI
 * properties.  Both the type and fch_addrsp_t should arguably be generic DDI
 * concepts; see notes in milan_fabric.c.
 */

#include <sys/types.h>
#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	FCH_PROPNAME_RANGES	"ranges"
#define	FCH_PROPNAME_REG	"reg"

typedef enum fch_addrsp {
	FA_NONE,
	FA_LEGACY,
	FA_MMIO,
	FA_INVALID	/* Keep this last; see assertion below. */
} fch_addrsp_t;

#define	FCH_NADDRSP	2

CTASSERT(FCH_NADDRSP == (FA_INVALID - 1));

/*
 * The bus types used in the regspec/regspec64 mapping requests sent toward
 * the root nexus, which interprets them using the fixed x86 convention
 * documented at rootnex_map(): 0 is memory, 1 is legacy I/O port space (and
 * anything greater with a zero address is x86-compatibility I/O, which we
 * never generate).
 *
 * XXX: Have this in a proper header somewhere?
 */
#define	FCH_REGSPEC_BUSTYPE_MMIO	0
#define	FCH_REGSPEC_BUSTYPE_IO		1

static inline uint64_t
fch_addrsp_to_bustype(const fch_addrsp_t addrsp)
{
	switch (addrsp) {
	case FA_LEGACY:
		return (FCH_REGSPEC_BUSTYPE_IO);
	case FA_MMIO:
		return (FCH_REGSPEC_BUSTYPE_MMIO);
	default:
		panic("invalid FCH address space %d cannot be translated",
		    addrsp);
	}
}

typedef struct fch_rangespec {
	uint32_t	fr_addrsp;
	uint32_t	fr_physhi;
	uint32_t	fr_physlo;
	uint32_t	fr_sizehi;
	uint32_t	fr_sizelo;
} fch_rangespec_t;

static const uint_t INTS_PER_RANGESPEC =
	(sizeof (fch_rangespec_t) / sizeof (uint32_t));

static inline uint64_t
fch_rangespec_addr(const fch_rangespec_t *const frp)
{
	uint64_t addr;

	addr = (uint64_t)frp->fr_physhi;
	addr <<= 32;
	addr |= (uint64_t)frp->fr_physlo;

	return (addr);
}

static inline uint64_t
fch_rangespec_size(const fch_rangespec_t *const frp)
{
	uint64_t size;

	size = (uint64_t)frp->fr_sizehi;
	size <<= 32;
	size |= (uint64_t)frp->fr_sizelo;

	return (size);
}

static inline char *
fch_rangespec_to_ndi_ra_type(const fch_rangespec_t *const frp)
{
	switch (frp->fr_addrsp) {
	case FA_LEGACY:
		return (NDI_RA_TYPE_IO);
	case FA_MMIO:
		return (NDI_RA_TYPE_MEM);
	default:
		return (NULL);
	}
}

/*
 * Reads a node's "reg" property as an array of fch_rangespec_t, returning the
 * number of complete rangespecs and, via frpp, the array itself, which the
 * caller must free with ddi_prop_free() when the count is non-zero.  Returns
 * 0 (and no array) if the property is absent or holds no complete rangespec.
 * This serves both sides of the contract: fch(4D) reading its children's
 * (and its own) registers, and its parent resolving mapping and sizing
 * requests against the property it authored.
 */
static inline uint_t
fch_get_regs(dev_info_t *dip, fch_rangespec_t **frpp)
{
	uint_t nint, nreg;

	*frpp = NULL;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    FCH_PROPNAME_REG, (int **)frpp, &nint) != DDI_SUCCESS) {
		nint = 0;
	}

	if (nint % INTS_PER_RANGESPEC != 0) {
		dev_err(dip, CE_WARN, "incomplete or extraneous '%s' entries",
		    FCH_PROPNAME_REG);
	}

	nreg = nint / INTS_PER_RANGESPEC;
	if (nreg == 0 && *frpp != NULL) {
		ddi_prop_free(*frpp);
		*frpp = NULL;
	}

	return (nreg);
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_RANGES_H */
