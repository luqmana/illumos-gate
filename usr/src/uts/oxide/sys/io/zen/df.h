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

#ifndef	_SYS_IO_ZEN_DF_H
#define	_SYS_IO_ZEN_DF_H

/*
 * Definitions shared between the df(4D) nexus driver and the root nexus,
 * whose bus_config creates the df device nodes.
 */

#ifdef	__cplusplus
extern "C" {
#endif

#define	DF_NODENAME		"df"

#define	DF_PROP_NODE_ID		"node-id"

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_IO_ZEN_DF_H */
