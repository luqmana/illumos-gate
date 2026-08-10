/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2020 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2026 Oxide Computer Company
 *
 */

#ifndef	_SYS_PCI_CFGACC_X86_H
#define	_SYS_PCI_CFGACC_X86_H

#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* AMD's northbridges vendor-id and device-ids */
#define	AMD_NTBRDIGE_VID		0x1022	/* AMD vendor-id */
#define	AMD_HT_NTBRIDGE_DID		0x1100	/* HT Configuration */
#define	AMD_AM_NTBRIDGE_DID		0x1101	/* Address Map */
#define	AMD_DC_NTBRIDGE_DID		0x1102	/* DRAM Controller */
#define	AMD_MC_NTBRIDGE_DID		0x1103	/* Misc Controller */

/* AMD's 8132 chipset vendor-id and device-ids */
#define	AMD_8132_BRIDGE_DID		0x7458	/* 8132 PCI-X bridge */
#define	AMD_8132_IOAPIC_DID		0x7459	/* 8132 IO APIC */

/*
 * Check if the given device is an AMD northbridge
 */
#define	IS_BAD_AMD_NTBRIDGE(vid, did) \
	    (((vid) == AMD_NTBRDIGE_VID) && \
	    (((did) == AMD_HT_NTBRIDGE_DID) || \
	    ((did) == AMD_AM_NTBRIDGE_DID) || \
	    ((did) == AMD_DC_NTBRIDGE_DID) || \
	    ((did) == AMD_MC_NTBRIDGE_DID)))

#define	IS_AMD_8132_CHIP(vid, did) \
	    (((vid) == AMD_NTBRDIGE_VID) && \
	    (((did) == AMD_8132_BRIDGE_DID) || \
	    ((did) == AMD_8132_IOAPIC_DID)))

/*
 * Note a bad device which doesn't support MMIO config space access.
 */
extern void pci_cfgacc_add_workaround(uint16_t, uchar_t, uchar_t);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PCI_CFGACC_X86_H */
