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
 * Module linkage for misc/pci_boot, the boot-time PCI enumeration library.
 *
 * This module contains no policy of its own: it exports the enumeration and
 * resource programming machinery (see <sys/pci_boot.h>) and is driven by
 * whichever platform module depends on it.  What it does own is the platform
 * resource discovery (PRD) session, initialized here because we need to provide
 * the upcalls the PRD is given.
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/modctl.h>
#include <sys/pci_boot.h>
#include <sys/plat/pci_prd.h>

/*
 * Unloading this module would strand the device tree it created, so we do not
 * permit it unless someone has gone out of their way with mdb -kw.
 */
uint_t pci_boot_detach = 0;

static struct modlmisc modlmisc = {
	&mod_miscops, "PCI boot enumeration"
};

static struct modlinkage modlinkage = {
	MODREV_1, (void *)&modlmisc, NULL
};

static pci_prd_upcalls_t pci_boot_upcalls = {
	.pru_bus2dip_f = pci_boot_bus_to_dip,
	.pru_register_fix_f = pci_boot_register_fix
};

int
_init(void)
{
	int err;

	/*
	 * Ready anything the platform may reach through the upcalls before
	 * giving it them, since pci_prd_init() is where it registers.
	 */
	pci_boot_fix_init();

	if ((err = pci_prd_init(&pci_boot_upcalls)) != 0)
		return (err);

	if ((err = mod_install(&modlinkage)) != 0) {
		pci_prd_fini();
		return (err);
	}

	pci_boot_maxbus = pci_prd_max_bus();

	return (0);
}

int
_fini(void)
{
	int err;

	if (pci_boot_detach == 0)
		return (EBUSY);

	if ((err = mod_remove(&modlinkage)) != 0)
		return (err);

	pci_prd_fini();
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
