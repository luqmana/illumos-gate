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

#ifndef	_SYS_IO_ZEN_IOMS_H
#define	_SYS_IO_ZEN_IOMS_H

/*
 * Definitions shared between the ioms(4D) nexus driver and the df(4D)
 * nexus, whose bus_config creates the ioms device nodes.
 */

#ifdef	__cplusplus
extern "C" {
#endif

#define	IOMS_NODENAME		"ioms"

#define	IOMS_PROP_IOMS		"ioms"
#define	IOMS_PROP_NBIO		"nbio"
#define	IOMS_PROP_IOHUB		"iohub"
#define	IOMS_PROP_IOHC		"iohc"
#define	IOMS_PROP_IOHC_TYPE	"iohc-type"
#define	IOMS_PROP_NODE_ID	"node-id"
#define	IOMS_PROP_PCI_BUS	"pci-bus"
#define	IOMS_PROP_FABRIC_ID	"fabric-id"

/*
 * The properties by which an ioms(4D) instance grants its PCIe root complex
 * child the address space that child may hand out.  IOMS_PROP_PCI_GRANT is an
 * array of pci_regspec_t, the same 1275 form a PCI nexus already uses for its
 * "ranges" and "available".  IOMS_PROP_BUS_GRANT is a pair of bus numbers, as
 * "bus-range" is.  Both describe what the root bus was given, before
 * enumeration has assigned any of it which is why neither reuses the name of
 * the property enumeration itself writes when it is done.
 */
#define	IOMS_PROP_PCI_GRANT	"pci-grant"
#define	IOMS_PROP_BUS_GRANT	"bus-grant"

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_IO_ZEN_IOMS_H */
