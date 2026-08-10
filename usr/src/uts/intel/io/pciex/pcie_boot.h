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

#ifndef	_PCIEX_PCIE_BOOT_H
#define	_PCIEX_PCIE_BOOT_H

/*
 * Private to the boot-time PCI enumeration in misc/pci_boot; see pcie_boot.c.
 */

#include <sys/types.h>
#include <sys/dditypes.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Determines whether the given root bus has any PCI Express device beneath it
 * and, if so, marks its device node as a PCI Express root complex.  Returns
 * whether it did so.
 */
extern boolean_t create_pcie_root_bus(uchar_t, dev_info_t *);

#ifdef	__cplusplus
}
#endif

#endif	/* _PCIEX_PCIE_BOOT_H */
