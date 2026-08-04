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

#ifndef _SYS_IO_FCH_PROPS_H
#define	_SYS_IO_FCH_PROPS_H

/*
 * Properties shared between fch(4D), the parent nexus that creates its device
 * node, and its children.  The parent tells the FCH whether it is the primary
 * one (decoding the fixed legacy/compatibility address space) or a secondary
 * one (decoding only a small relocatable window) via the fabric-role property.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define	FCH_PROPNAME_FABRIC_ROLE	"fabric-role"
#define	FCH_FABRIC_ROLE_PRI		"primary"
#define	FCH_FABRIC_ROLE_SEC		"secondary"

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_PROPS_H */
