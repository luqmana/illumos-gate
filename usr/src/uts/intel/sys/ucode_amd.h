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
 * Copyright 2021 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2022 Joyent, Inc.
 * Copyright 2025 Oxide Computer Company
 */

#ifndef	_SYS_UCODE_AMD_H
#define	_SYS_UCODE_AMD_H

#include <sys/types.h>
#include <ucode/ucode_errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AMD Microcode file information
 */
typedef struct ucode_header_amd {
	uint32_t uh_date;
	uint32_t uh_patch_id;
	uint32_t uh_internal; /* patch data id & length, init flag */
	uint32_t uh_cksum;
	uint32_t uh_nb_id;
	uint32_t uh_sb_id;
	uint16_t uh_cpu_rev;
	uint8_t  uh_nb_rev;
	uint8_t  uh_sb_rev;
	uint32_t uh_bios_rev;
	uint32_t uh_match[8];
} ucode_header_amd_t;

/*
 * We currently only support microcode patches of at most 16KiB (16384 bytes).
 */
#define	UCODE_AMD_MAX_PATCH_SIZE	(16384)
#define	UCODE_AMD_PAD_SIZE \
	(UCODE_AMD_MAX_PATCH_SIZE - sizeof (ucode_header_amd_t))

typedef struct ucode_file_amd {
	ucode_header_amd_t uf_header;
	union {
		struct {
			uint8_t uf_data[896];
			uint8_t uf_resv[896];
			uint8_t uf_code_present;
			uint8_t uf_code[191];
			uint8_t uf_encr[];
		};
		uint8_t uf_pad[UCODE_AMD_PAD_SIZE];
	};
} ucode_file_amd_t;

typedef struct ucode_eqtbl_amd {
	uint32_t ue_inst_cpu;
	uint32_t ue_fixed_mask;
	uint32_t ue_fixed_comp;
	uint16_t ue_equiv_cpu;
	uint16_t ue_reserved;
} ucode_eqtbl_amd_t;

#define	UCODE_AMD_EQUIVALENCE_TABLE_NAME "equivalence-table"
#define	UCODE_MAX_NAME_LEN_AMD (sizeof (UCODE_AMD_EQUIVALENCE_TABLE_NAME))

#ifdef __cplusplus
}
#endif

#endif	/* _SYS_UCODE_AMD_H */
