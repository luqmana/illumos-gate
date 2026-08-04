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

#ifndef	_SYS_IO_ZEN_FCH_H
#define	_SYS_IO_ZEN_FCH_H

/*
 * Definitions for the fch(4D) nexus driver.
 */

#include <sys/amdzen/fch.h>
#include <sys/cpuvar.h>
#include <sys/io/zen/df.h>
#include <sys/io/zen/fabric.h>
#include <sys/io/zen/ioms.h>
#include <sys/x86_archext.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Returns a string literal name for the given FCH kind or NULL if the kind is
 * unknown.  This is used as the devinfo node name for the fch(4D) nexus.
 */
static inline const char *
fch_kind_name(fch_kind_t kind)
{
	switch (kind) {
	case FK_TAISHAN:
		return ("taishan");
	case FK_HUASHAN:
		return ("huashan");
	case FK_SONGSHAN:
		return ("songshan");
	case FK_KUNLUN:
		return ("kunlun");
	default:
		return (NULL);
	}
}

/*
 * Formats the devinfo path of the primary FCH's device node (e.g.
 * "/df@0/ioms@3/huashan") into the provided buffer, deriving every component
 * from the fabric: which IOMS hosts the FCH varies by microarchitecture
 * (IOMS 3 on Milan, 4 on Turin) and is never assumed.  This is for early boot
 * consumers (console and SP paths) that must name FCH peripherals before
 * general device enumeration.  The return value follows snprintf(9F) semantics
 * and 0 is returned if there is no primary FCH or we don't recognize its kind.
 */
static inline size_t
fch_dev_path(char *buf, size_t len)
{
	uint16_t node_id;
	uint8_t ioms_num;
	const char *nodename;

	nodename = fch_kind_name(chiprev_fch_kind(cpuid_getchiprev(CPU)));
	if (nodename == NULL)
		return (0);

	if (!zen_fabric_primary_fch(&node_id, &ioms_num))
		return (0);

	return (snprintf(buf, len,
	    "/" DF_NODENAME "@%x/" IOMS_NODENAME "@%x/%s",
	    node_id, ioms_num, nodename));
}

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_IO_ZEN_FCH_H */
