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
 * Copyright 2024 Oxide Computer Company
 */

/*
 * Genoa DF and UMC data shared among other files.
 */

#ifndef	_GENOA_IMPL_H
#define	_GENOA_IMPL_H

#include "../zen_kmdb_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const char *genoa_chan_ileaves[32];
extern const char *genoa_chan_map[12];
extern const uint8_t genoa_chan_umc_order[12];
extern df_comp_t genoa_comps[97];

#ifdef __cplusplus
}
#endif

#endif /* _GENOA_IMPL_H */
