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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2016 Joyent, Inc.
 * Copyright 2019 Western Digital Corporation
 * Copyright 2020 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2026 Oxide Computer Company
 */

/*
 * This file contains the x86 PCI platform resource discovery backend. This uses
 * data from a combination of sources, preferring ACPI, if present, and if not,
 * falling back to either the PCI hot-plug resource table or the mps tables.
 *
 * Today, to get information from ACPI we need to start from a dev_info_t. This
 * is partly why the PRD interface has a callback for getting information about
 * a dev_info_t. It also means we cannot initialize the tables with information
 * until all devices have been initially scanned.
 *
 * Additionally, the boot-time PCI enumeration (misc/pci_boot in uts/intel) is
 * generic so any PC-specific errata or legacy behavior only applicable on i86pc
 * is also defined here.
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/memlist.h>
#include <sys/memlist_impl.h>
#include <sys/pci.h>
#include <sys/pci_impl.h>
#include <sys/pci_cfgspace_impl.h>
#include <sys/apic.h>
#include <sys/apic_common.h>
#include <sys/bootconf.h>
#include <sys/iommulib.h>
#include <sys/kmem.h>
#include <sys/pci_boot.h>
#include <sys/pci_cfgacc.h>
#include <sys/pci_cfgacc_x86.h>
#include <sys/pci_cfgspace.h>
#include <sys/pci_props.h>
#include <sys/pcie_impl.h>
#include <sys/sunndi.h>
#include <sys/sysmacros.h>
#include <io/pciex/pcie_nvidia.h>
#include <sys/systm.h>
#include <sys/cmn_err.h>
#include <sys/acpi/acpi.h>
#include <sys/acpica.h>
#include <sys/plat/pci_prd.h>
#include "mps_table.h"
#include "pcihrt.h"

extern int pci_bios_maxbus;

int pci_prd_debug = 0;
#define	dprintf	if (pci_prd_debug) printf
#define	dcmn_err	if (pci_prd_debug != 0) cmn_err

#define	pci_getb	(*pci_getb_func)
#define	pci_getw	(*pci_getw_func)
#define	pci_getl	(*pci_getl_func)
#define	pci_putb	(*pci_putb_func)

/* See AMD-8111 Datasheet Rev 3.03, Page 149: */
#define	LPC_IO_CONTROL_REG_1	0x40
#define	AMD8111_ENABLENMI	(uint8_t)0x80
#define	DEVID_AMD8111_LPC	0x7468

static int tbl_init = 0;
static uchar_t *mps_extp = NULL;
static uchar_t *mps_ext_endp = NULL;
static struct php_entry *hrt_hpep;
static uint_t hrt_entry_cnt = 0;
static int acpi_cb_cnt = 0;
static pci_prd_upcalls_t *prd_upcalls;

static void mps_probe(void);
static void acpi_pci_probe(void);
static int mps_find_bus_res(uint32_t, pci_prd_rsrc_t, struct memlist **);
static void hrt_probe(void);
static int hrt_find_bus_res(uint32_t, pci_prd_rsrc_t, struct memlist **);
static size_t acpi_find_bus_res(uint32_t, pci_prd_rsrc_t, struct memlist **);
static int legacy_find_bus0_res(pci_prd_rsrc_t, struct memlist **);
static uchar_t *find_sig(uchar_t *cp, int len, char *sig);
static int checksum(unsigned char *cp, int len);
static ACPI_STATUS acpi_wr_cb(ACPI_RESOURCE *rp, void *context);
static void acpi_trim_bus_ranges(void);

/*
 * -1 = attempt ACPI resource discovery
 *  0 = don't attempt ACPI resource discovery
 *  1 = ACPI resource discovery successful
 */
volatile int acpi_resource_discovery = -1;

struct memlist *acpi_io_res[PCI_MAX_BUS_NUM];
struct memlist *acpi_mem_res[PCI_MAX_BUS_NUM];
struct memlist *acpi_pmem_res[PCI_MAX_BUS_NUM];
struct memlist *acpi_bus_res[PCI_MAX_BUS_NUM];

/*
 * This indicates whether or not we have a traditional x86 BIOS present or not.
 */
static boolean_t pci_prd_have_bios = B_TRUE;

/*
 * This value is set up as part of PCI configuration space initialization.
 */
extern int pci_bios_maxbus;

/*
 * i86pc-specific boot-time PCI enumeration hooks.
 */
static void pci_prd_i86pc_devinit(dev_info_t *dip, uint8_t bus, uint8_t dev,
    uint8_t func, const pci_prop_data_t *prop);
static void pci_prd_i86pc_devdone(dev_info_t *dip, uint8_t bus, uint8_t dev,
    uint8_t func, const pci_prop_data_t *prop);
static boolean_t pci_prd_i86pc_claim(dev_info_t *dip, uint8_t bus, uint8_t dev,
    uint8_t func, const pci_prop_data_t *prop);
static void pci_prd_i86pc_claimed(dev_info_t *dip, uint8_t bus, uint8_t dev,
    uint8_t func, const pci_prop_data_t *prop);
static boolean_t pci_prd_i86pc_io_bar(dev_info_t *dip, uint8_t bus, uint8_t dev,
    uint8_t func, uint_t bar, uint32_t *basep, uint_t *lenp,
    boolean_t *hard_decodep);
static uint_t pci_prd_i86pc_aliases(uint8_t bus, uint8_t dev, uint8_t func,
    pci_boot_region_t *alias, uint_t nmax);
static boolean_t pci_prd_i86pc_legacy_range(uint64_t base, uint64_t len,
    boolean_t io);
static uint_t pci_prd_i86pc_bridge_regions(uint8_t bus, uint8_t dev,
    uint8_t func, pci_boot_region_t *ranges, uint_t nmax);
static boolean_t pci_prd_i86pc_apply_amd8111_fix(uint8_t bus, uint8_t dev,
    uint8_t fn, const pci_prop_data_t *prop);
static void pci_prd_i86pc_undo_amd8111_fix(uint8_t bus, uint8_t dev,
    uint8_t fn);

static const pci_boot_ops_t pci_prd_i86pc_boot_ops = {
	.pbo_devinit_f = pci_prd_i86pc_devinit,
	.pbo_devdone_f = pci_prd_i86pc_devdone,
	.pbo_claim_f = pci_prd_i86pc_claim,
	.pbo_claimed_f = pci_prd_i86pc_claimed,
	.pbo_io_bar_f = pci_prd_i86pc_io_bar,
	.pbo_aliases_f = pci_prd_i86pc_aliases,
	.pbo_legacy_range_f = pci_prd_i86pc_legacy_range,
	.pbo_bridge_regions_f = pci_prd_i86pc_bridge_regions
};


static void
acpi_pci_probe(void)
{
	ACPI_HANDLE ah;
	int bus;

	if (acpi_resource_discovery == 0)
		return;

	for (bus = 0; bus <= pci_bios_maxbus; bus++) {
		dev_info_t *dip;

		dip = prd_upcalls->pru_bus2dip_f(bus);
		if (dip == NULL ||
		    (ACPI_FAILURE(acpica_get_handle(dip, &ah))))
			continue;

		(void) AcpiWalkResources(ah, "_CRS", acpi_wr_cb,
		    (void *)(uintptr_t)bus);
	}

	if (acpi_cb_cnt > 0) {
		acpi_resource_discovery = 1;
		acpi_trim_bus_ranges();
	}
}

/*
 * Trim overlapping bus ranges in acpi_bus_res[]
 * Some BIOSes report root-bridges with bus ranges that
 * overlap, for example:"0..255" and "8..255". Lower-numbered
 * ranges are trimmed by upper-numbered ranges (so "0..255" would
 * be trimmed to "0..7", in the example).
 */
static void
acpi_trim_bus_ranges(void)
{
	struct memlist *ranges, *current;
	int bus;

	ranges = NULL;

	/*
	 * Assumptions:
	 *  - there exists at most 1 bus range entry for each bus number
	 *  - there are no (broken) ranges that start at the same bus number
	 */
	for (bus = 0; bus < PCI_MAX_BUS_NUM; bus++) {
		struct memlist *prev, *orig, *new;
		/* skip buses with no range entry */
		if ((orig = acpi_bus_res[bus]) == NULL)
			continue;

		/*
		 * create copy of existing range and overload
		 * 'prev' pointer to link existing to new copy
		 */
		new = xmemlist_get_one(&memlist_kmem_pool);
		new->ml_address = orig->ml_address;
		new->ml_size = orig->ml_size;
		new->ml_prev = orig;

		/* sorted insertion of 'new' into ranges list */
		for (current = ranges, prev = NULL; current != NULL;
		    prev = current, current = current->ml_next)
			if (new->ml_address < current->ml_address)
				break;

		if (prev == NULL) {
			/* place at beginning of (possibly) empty list */
			new->ml_next = ranges;
			ranges = new;
		} else {
			/* place in list (possibly at end) */
			new->ml_next = current;
			prev->ml_next = new;
		}
	}

	/* scan the list, perform trimming */
	current = ranges;
	while (current != NULL) {
		struct memlist *next = current->ml_next;

		/* done when no range above current */
		if (next == NULL)
			break;

		/*
		 * trim size in original range element
		 * (current->ml_prev points to the original range)
		 */
		if ((current->ml_address + current->ml_size) > next->ml_address)
			current->ml_prev->ml_size =
			    next->ml_address - current->ml_address;

		current = next;
	}

	/* discard the list */
	memlist_rsrc_free(&ranges);	/* OK if ranges == NULL */
}

static size_t
acpi_find_bus_res(uint32_t bus, pci_prd_rsrc_t type, struct memlist **res)
{
	ASSERT3U(bus, <, PCI_MAX_BUS_NUM);

	switch (type) {
	case PCI_PRD_R_IO:
		*res = acpi_io_res[bus];
		break;
	case PCI_PRD_R_MMIO:
		*res = acpi_mem_res[bus];
		break;
	case PCI_PRD_R_PREFETCH:
		*res = acpi_pmem_res[bus];
		break;
	case PCI_PRD_R_BUS:
		*res = acpi_bus_res[bus];
		break;
	default:
		*res = NULL;
		break;
	}

	/* memlist_count() treats NULL head as zero-length */
	return (memlist_count(*res));
}

static struct memlist **
rlistpp(UINT8 t, UINT8 caching, int bus)
{
	switch (t) {
	case ACPI_MEMORY_RANGE:
		if (caching == ACPI_PREFETCHABLE_MEMORY)
			return (&acpi_pmem_res[bus]);
		else
			return (&acpi_mem_res[bus]);
		break;

	case ACPI_IO_RANGE:
		return (&acpi_io_res[bus]);
		break;

	case ACPI_BUS_NUMBER_RANGE:
		return (&acpi_bus_res[bus]);
		break;
	}

	return (NULL);
}

static void
acpi_dbg(uint_t bus, uint64_t addr, uint64_t len, uint8_t caching, uint8_t type,
    char *tag)
{
	char *s;

	switch (type) {
	case ACPI_MEMORY_RANGE:
		s = "MEM";
		break;
	case ACPI_IO_RANGE:
		s = "IO";
		break;
	case ACPI_BUS_NUMBER_RANGE:
		s = "BUS";
		break;
	default:
		s = "???";
		break;
	}

	dprintf("ACPI: bus %x %s/%s %lx/%lx (Caching: %x)\n", bus,
	    tag, s, addr, len, caching);
}


static ACPI_STATUS
acpi_wr_cb(ACPI_RESOURCE *rp, void *context)
{
	int bus = (intptr_t)context;

	/* ignore consumed resources */
	if (rp->Data.Address.ProducerConsumer == 1)
		return (AE_OK);

	switch (rp->Type) {
	case ACPI_RESOURCE_TYPE_IRQ:
		/* never expect to see a PCI bus produce an Interrupt */
		dprintf("%s\n", "IRQ");
		break;

	case ACPI_RESOURCE_TYPE_DMA:
		/* never expect to see a PCI bus produce DMA */
		dprintf("%s\n", "DMA");
		break;

	case ACPI_RESOURCE_TYPE_START_DEPENDENT:
		dprintf("%s\n", "START_DEPENDENT");
		break;

	case ACPI_RESOURCE_TYPE_END_DEPENDENT:
		dprintf("%s\n", "END_DEPENDENT");
		break;

	case ACPI_RESOURCE_TYPE_IO:
		if (rp->Data.Io.AddressLength == 0)
			break;
		acpi_cb_cnt++;
		(void) memlist_rsrc_add(rp->Data.Io.Minimum,
		    rp->Data.Io.AddressLength, &acpi_io_res[bus]);
		if (pci_prd_debug != 0) {
			acpi_dbg(bus, rp->Data.Io.Minimum,
			    rp->Data.Io.AddressLength, 0, ACPI_IO_RANGE, "IO");
		}
		break;

	case ACPI_RESOURCE_TYPE_FIXED_IO:
		/* only expect to see this as a consumer */
		dprintf("%s\n", "FIXED_IO");
		break;

	case ACPI_RESOURCE_TYPE_VENDOR:
		dprintf("%s\n", "VENDOR");
		break;

	case ACPI_RESOURCE_TYPE_END_TAG:
		dprintf("%s\n", "END_TAG");
		break;

	case ACPI_RESOURCE_TYPE_MEMORY24:
		/* only expect to see this as a consumer */
		dprintf("%s\n", "MEMORY24");
		break;

	case ACPI_RESOURCE_TYPE_MEMORY32:
		/* only expect to see this as a consumer */
		dprintf("%s\n", "MEMORY32");
		break;

	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		/* only expect to see this as a consumer */
		dprintf("%s\n", "FIXED_MEMORY32");
		break;

	case ACPI_RESOURCE_TYPE_ADDRESS16:
		if (rp->Data.Address16.Address.AddressLength == 0)
			break;
		acpi_cb_cnt++;
		(void) memlist_rsrc_add(rp->Data.Address16.Address.Minimum,
		    rp->Data.Address16.Address.AddressLength,
		    rlistpp(rp->Data.Address16.ResourceType,
		    rp->Data.Address.Info.Mem.Caching, bus));
		if (pci_prd_debug != 0) {
			acpi_dbg(bus,
			    rp->Data.Address16.Address.Minimum,
			    rp->Data.Address16.Address.AddressLength,
			    rp->Data.Address.Info.Mem.Caching,
			    rp->Data.Address16.ResourceType, "ADDRESS16");
		}
		break;

	case ACPI_RESOURCE_TYPE_ADDRESS32:
		if (rp->Data.Address32.Address.AddressLength == 0)
			break;
		acpi_cb_cnt++;
		(void) memlist_rsrc_add(rp->Data.Address32.Address.Minimum,
		    rp->Data.Address32.Address.AddressLength,
		    rlistpp(rp->Data.Address32.ResourceType,
		    rp->Data.Address.Info.Mem.Caching, bus));
		if (pci_prd_debug != 0) {
			acpi_dbg(bus,
			    rp->Data.Address32.Address.Minimum,
			    rp->Data.Address32.Address.AddressLength,
			    rp->Data.Address.Info.Mem.Caching,
			    rp->Data.Address32.ResourceType, "ADDRESS32");
		}
		break;

	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if (rp->Data.Address64.Address.AddressLength == 0)
			break;

		acpi_cb_cnt++;
		(void) memlist_rsrc_add(rp->Data.Address64.Address.Minimum,
		    rp->Data.Address64.Address.AddressLength,
		    rlistpp(rp->Data.Address64.ResourceType,
		    rp->Data.Address.Info.Mem.Caching, bus));
		if (pci_prd_debug != 0) {
			acpi_dbg(bus,
			    rp->Data.Address64.Address.Minimum,
			    rp->Data.Address64.Address.AddressLength,
			    rp->Data.Address.Info.Mem.Caching,
			    rp->Data.Address64.ResourceType, "ADDRESS64");
		}
		break;

	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		if (rp->Data.ExtAddress64.Address.AddressLength == 0)
			break;
		acpi_cb_cnt++;
		(void) memlist_rsrc_add(rp->Data.ExtAddress64.Address.Minimum,
		    rp->Data.ExtAddress64.Address.AddressLength,
		    rlistpp(rp->Data.ExtAddress64.ResourceType,
		    rp->Data.Address.Info.Mem.Caching, bus));
		if (pci_prd_debug != 0) {
			acpi_dbg(bus,
			    rp->Data.ExtAddress64.Address.Minimum,
			    rp->Data.ExtAddress64.Address.AddressLength,
			    rp->Data.Address.Info.Mem.Caching,
			    rp->Data.ExtAddress64.ResourceType, "EXTADDRESS64");
		}
		break;

	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		/* never expect to see a PCI bus produce an Interrupt */
		dprintf("%s\n", "EXTENDED_IRQ");
		break;

	case ACPI_RESOURCE_TYPE_GENERIC_REGISTER:
		/* never expect to see a PCI bus produce an GAS */
		dprintf("%s\n", "GENERIC_REGISTER");
		break;
	}

	return (AE_OK);
}

static void
mps_probe(void)
{
	uchar_t *extp;
	struct mps_fps_hdr *fpp = NULL;
	struct mps_ct_hdr *ctp;
	uintptr_t ebda_start, base_end;
	ushort_t ebda_seg, base_size, ext_len, base_len, base_end_seg;

	base_size = *((ushort_t *)(0x413));
	ebda_seg = *((ushort_t *)(0x40e));
	ebda_start = ((uint32_t)ebda_seg) << 4;
	if (ebda_seg != 0) {
		fpp = (struct mps_fps_hdr *)find_sig(
		    (uchar_t *)ebda_start, 1024, "_MP_");
	}
	if (fpp == NULL) {
		base_end_seg = (base_size > 512) ? 0x9FC0 : 0x7FC0;
		if (base_end_seg != ebda_seg) {
			base_end = ((uintptr_t)base_end_seg) << 4;
			fpp = (struct mps_fps_hdr *)find_sig(
			    (uchar_t *)base_end, 1024, "_MP_");
		}
	}
	if (fpp == NULL) {
		fpp = (struct mps_fps_hdr *)find_sig(
		    (uchar_t *)0xF0000, 0x10000, "_MP_");
	}

	if (fpp == NULL) {
		dprintf("MP Spec table doesn't exist");
		return;
	} else {
		dprintf("Found MP Floating Pointer Structure at %p\n",
		    (void *)fpp);
	}

	if (checksum((uchar_t *)fpp, fpp->fps_len * 16) != 0) {
		dprintf("MP Floating Pointer Structure checksum error");
		return;
	}

	ctp = (struct mps_ct_hdr *)(uintptr_t)fpp->fps_mpct_paddr;
	if (ctp->ct_sig != 0x504d4350) { /* check "PCMP" signature */
		dprintf("MP Configuration Table signature is wrong");
		return;
	}

	base_len = ctp->ct_len;
	if (checksum((uchar_t *)ctp, base_len) != 0) {
		dprintf("MP Configuration Table checksum error");
		return;
	}
	if (ctp->ct_spec_rev != 4) { /* not MPSpec rev 1.4 */
		dprintf("MP Spec 1.1 found - extended table doesn't exist");
		return;
	}
	if ((ext_len = ctp->ct_ext_tbl_len) == 0) {
		dprintf("MP Spec 1.4 found - extended table doesn't exist");
		return;
	}
	extp = (uchar_t *)ctp + base_len;
	if (((checksum(extp, ext_len) + ctp->ct_ext_cksum) & 0xFF) != 0) {
		dprintf("MP Extended Table checksum error");
		return;
	}
	mps_extp = extp;
	mps_ext_endp = mps_extp + ext_len;
}


static int
mps_find_bus_res(uint32_t bus, pci_prd_rsrc_t rsrc, struct memlist **res)
{
	struct sasm *sasmp;
	uchar_t *extp;
	int res_cnt, type;

	ASSERT3U(bus, <, PCI_MAX_BUS_NUM);

	if (mps_extp == NULL)
		return (0);

	switch (rsrc) {
	case PCI_PRD_R_IO:
		type = IO_TYPE;
		break;
	case PCI_PRD_R_MMIO:
		type = MEM_TYPE;
		break;
	case PCI_PRD_R_PREFETCH:
		type = PREFETCH_TYPE;
		break;
	case PCI_PRD_R_BUS:
		type = BUSRANGE_TYPE;
		break;
	default:
		*res = NULL;
		return (0);
	}

	extp = mps_extp;
	res_cnt = 0;
	while (extp < mps_ext_endp) {
		switch (*extp) {
		case SYS_AS_MAPPING:
			sasmp = (struct sasm *)extp;
			if (sasmp->sasm_as_type == type &&
			    sasmp->sasm_bus_id == bus) {
				uint64_t base, len;

				base = (uint64_t)sasmp->sasm_as_base |
				    (uint64_t)sasmp->sasm_as_base_hi << 32;
				len = (uint64_t)sasmp->sasm_as_len |
				    (uint64_t)sasmp->sasm_as_len_hi << 32;
				(void) memlist_rsrc_add(base, len, res);
				res_cnt++;
			}
			extp += SYS_AS_MAPPING_SIZE;
			break;
		case BUS_HIERARCHY_DESC:
			extp += BUS_HIERARCHY_DESC_SIZE;
			break;
		case COMP_BUS_AS_MODIFIER:
			extp += COMP_BUS_AS_MODIFIER_SIZE;
			break;
		default:
			cmn_err(CE_WARN, "Unknown descriptor type %d"
			    " in BIOS Multiprocessor Spec table.",
			    *extp);
			memlist_rsrc_free(res);
			return (0);
		}
	}
	return (res_cnt);
}

static void
hrt_probe(void)
{
	struct hrt_hdr *hrtp;

	dprintf("search PCI Hot-Plug Resource Table starting at 0xF0000\n");
	if ((hrtp = (struct hrt_hdr *)find_sig((uchar_t *)0xF0000,
	    0x10000, "$HRT")) == NULL) {
		dprintf("NO PCI Hot-Plug Resource Table");
		return;
	}
	dprintf("Found PCI Hot-Plug Resource Table at %p\n", (void *)hrtp);
	if (hrtp->hrt_ver != 1) {
		dprintf("PCI Hot-Plug Resource Table version no. <> 1\n");
		return;
	}
	hrt_entry_cnt = (uint_t)hrtp->hrt_entry_cnt;
	dprintf("No. of PCI hot-plug slot entries = 0x%x\n", hrt_entry_cnt);
	hrt_hpep = (struct php_entry *)(hrtp + 1);
}

static int
hrt_find_bus_res(uint32_t bus, pci_prd_rsrc_t type, struct memlist **res)
{
	int res_cnt;
	struct php_entry *hpep;

	ASSERT3U(bus, <, PCI_MAX_BUS_NUM);

	if (hrt_hpep == NULL || hrt_entry_cnt == 0)
		return (0);
	hpep = hrt_hpep;
	res_cnt = 0;
	for (uint_t i = 0; i < hrt_entry_cnt; i++, hpep++) {
		if (hpep->php_pri_bus != bus)
			continue;
		if (type == PCI_PRD_R_IO) {
			if (hpep->php_io_start == 0 || hpep->php_io_size == 0)
				continue;
			(void) memlist_rsrc_add((uint64_t)hpep->php_io_start,
			    (uint64_t)hpep->php_io_size, res);
			res_cnt++;
		} else if (type == PCI_PRD_R_MMIO) {
			if (hpep->php_mem_start == 0 || hpep->php_mem_size == 0)
				continue;
			(void) memlist_rsrc_add(
			    ((uint64_t)hpep->php_mem_start) << 16,
			    ((uint64_t)hpep->php_mem_size) << 16, res);
			res_cnt++;
		} else if (type == PCI_PRD_R_PREFETCH) {
			if (hpep->php_pfmem_start == 0 ||
			    hpep->php_pfmem_size == 0)
				continue;
			(void) memlist_rsrc_add(
			    ((uint64_t)hpep->php_pfmem_start) << 16,
			    ((uint64_t)hpep->php_pfmem_size) << 16, res);
			res_cnt++;
		}
	}
	return (res_cnt);
}

static uchar_t *
find_sig(uchar_t *cp, int len, char *sig)
{
	long i;

	/* Search for the "_MP_"  or "$HRT" signature */
	for (i = 0; i < len; i += 16) {
		if (cp[0] == sig[0] && cp[1] == sig[1] &&
		    cp[2] == sig[2] && cp[3] == sig[3])
			return (cp);
		cp += 16;
	}
	return (NULL);
}

static int
checksum(unsigned char *cp, int len)
{
	int i;
	unsigned int cksum;

	for (i = cksum = 0; i < len; i++)
		cksum += (unsigned int) *cp++;

	return ((int)(cksum & 0xFF));
}

uint32_t
pci_prd_max_bus(void)
{
	return ((uint32_t)pci_bios_maxbus);
}

/*
 * What a PC's bus 0 may be assumed to route when none of the firmware tables
 * describe it.  This is the source of last resort, consulted only once the
 * others have come up empty, and it speaks only for bus 0.
 *
 * Unlike the tables above, whose lists we own and hand out as they are, both
 * answers here are freshly built for the caller to keep: the boot memlist
 * belongs to the boot subsystem and must not be given away.
 */
static int
legacy_find_bus0_res(pci_prd_rsrc_t rsrc, struct memlist **res)
{
	switch (rsrc) {
	case PCI_PRD_R_MMIO:
		/*
		 * Whatever the boot loader noted as possibly available for
		 * PCI MMIO.
		 */
		*res = memlist_rsrc_dup(bootops->boot_mem->pcimem);
		break;
	case PCI_PRD_R_IO:
		/*
		 * For I/O space [0x0,0xFFFF], the assumption is that the
		 * first 256 bytes [0x00,0xFF] are reserved for the system.
		 */
		(void) memlist_rsrc_add(0x100, 0xff00, res);
		break;
	default:
		return (0);
	}

	return (memlist_count(*res));
}

struct memlist *
pci_prd_find_resource(uint32_t bus, pci_prd_rsrc_t rsrc)
{
	struct memlist *res = NULL;

	if (bus > pci_bios_maxbus)
		return (NULL);

	if (tbl_init == 0) {
		tbl_init = 1;
		acpi_pci_probe();
		if (pci_prd_have_bios) {
			hrt_probe();
			mps_probe();
		}
	}

	if (acpi_find_bus_res(bus, rsrc, &res) > 0)
		return (res);

	if (pci_prd_have_bios && hrt_find_bus_res(bus, rsrc, &res) > 0)
		return (res);

	if (pci_prd_have_bios)
		(void) mps_find_bus_res(bus, rsrc, &res);

	if (res == NULL && bus == 0)
		(void) legacy_find_bus0_res(rsrc, &res);

	return (res);
}

typedef struct {
	pci_prd_root_complex_f	ppac_func;
	void			*ppac_arg;
} pci_prd_acpi_cb_t;

static ACPI_STATUS
pci_process_acpi_device(ACPI_HANDLE hdl, UINT32 level, void *ctx, void **rv)
{
	ACPI_DEVICE_INFO *adi;
	int busnum;
	pci_prd_acpi_cb_t *cb = ctx;

	/*
	 * Use AcpiGetObjectInfo() to find the device _HID
	 * If not a PCI root-bus, ignore this device and continue
	 * the walk
	 */
	if (ACPI_FAILURE(AcpiGetObjectInfo(hdl, &adi)))
		return (AE_OK);

	if (!(adi->Valid & ACPI_VALID_HID)) {
		AcpiOsFree(adi);
		return (AE_OK);
	}

	if (strncmp(adi->HardwareId.String, PCI_ROOT_HID_STRING,
	    sizeof (PCI_ROOT_HID_STRING)) &&
	    strncmp(adi->HardwareId.String, PCI_EXPRESS_ROOT_HID_STRING,
	    sizeof (PCI_EXPRESS_ROOT_HID_STRING))) {
		AcpiOsFree(adi);
		return (AE_OK);
	}

	AcpiOsFree(adi);

	/*
	 * acpica_get_busno() will check the presence of _BBN and
	 * fail if not present. It will then use the _CRS method to
	 * retrieve the actual bus number assigned, it will fall back
	 * to _BBN should the _CRS method fail.
	 */
	if (ACPI_SUCCESS(acpica_get_busno(hdl, &busnum))) {
		/*
		 * Ignore invalid _BBN return values here (rather
		 * than panic) and emit a warning; something else
		 * may suffer failure as a result of the broken BIOS.
		 */
		if (busnum < 0) {
			dcmn_err(CE_NOTE,
			    "pci_process_acpi_device: invalid _BBN 0x%x",
			    busnum);
			return (AE_CTRL_DEPTH);
		}

		if (cb->ppac_func((uint32_t)busnum, cb->ppac_arg))
			return (AE_CTRL_DEPTH);
		return (AE_CTRL_TERMINATE);
	}

	/* PCI and no _BBN, continue walk */
	return (AE_OK);
}

void
pci_prd_root_complex_iter(pci_prd_root_complex_f func, void *arg)
{
	void *rv;
	pci_prd_acpi_cb_t cb;

	cb.ppac_func = func;
	cb.ppac_arg = arg;

	/*
	 * First scan ACPI devices for anything that might be here. After that,
	 * go through and check the old BIOS IRQ routing table for additional
	 * buses. Note, slot naming from the IRQ table comes later.
	 */
	(void) AcpiGetDevices(NULL, pci_process_acpi_device, &cb, &rv);
	pci_bios_bus_iter(func, arg);

}


/*
 * If there is actually a PCI IRQ routing table present, then we want to use
 * this to go back and update the slot name. In particular, if we have no PCI
 * IRQ routing table, then we use the existing slot names that were already set
 * up for us in picex_slot_names_prop() from the capability register. Otherwise,
 * we actually delete all slot-names properties from buses and instead use
 * something from the IRQ routing table if it exists.
 *
 * Note, the property is always deleted regardless of whether or not it exists
 * in the IRQ routing table. Finally, we have traditionally kept "pcie0" names
 * as special as apparently that can't be represented in the IRQ routing table.
 */
void
pci_prd_slot_name(uint32_t bus, dev_info_t *dip)
{
	char slotprop[256];
	int len;
	char *slotcap_name;

	if (pci_irq_nroutes == 0)
		return;

	if (dip != NULL) {
		if (ddi_prop_lookup_string(DDI_DEV_T_ANY, pci_bus_res[bus].dip,
		    DDI_PROP_DONTPASS, "slot-names", &slotcap_name) !=
		    DDI_SUCCESS || strcmp(slotcap_name, "pcie0") != 0) {
			(void) ndi_prop_remove(DDI_DEV_T_NONE,
			    pci_bus_res[bus].dip, "slot-names");
		}
	}


	len = pci_slot_names_prop(bus, slotprop, sizeof (slotprop));
	if (len > 0) {
		if (dip != NULL) {
			ASSERT((len % sizeof (int)) == 0);
			(void) ndi_prop_update_int_array(DDI_DEV_T_NONE,
			    pci_bus_res[bus].dip, "slot-names",
			    (int *)slotprop, len / sizeof (int));
		} else {
			cmn_err(CE_NOTE, "!BIOS BUG: Invalid bus number in PCI "
			    "IRQ routing table; Not adding slot-names "
			    "property for incorrect bus %d", bus);
		}
	}
}

boolean_t
pci_prd_multi_root_ok(void)
{
	return (acpi_resource_discovery > 0);
}

/*
 * We generally expect that the system firmware will have set up bridges and
 * programmed BARs into devices before we get here, and we should honour those
 * settings.
 */
boolean_t
pci_prd_ignore_firmware(void)
{
	return (B_FALSE);
}

/*
 * These compatibility flags generally exist for i86pc. We need to still
 * enumerate ISA bridges and the naming of device nodes and aliases must be kept
 * consistent lest we break boot. See uts/common/io/pciex/pci_props.c theory
 * statement for more information.
 */
pci_prd_compat_flags_t
pci_prd_compat_flags(void)
{
	return (PCI_PRD_COMPAT_ISA | PCI_PRD_COMPAT_PCI_NODE_NAME |
	    PCI_PRD_COMPAT_SUBSYS);
}

int
pci_prd_init(pci_prd_upcalls_t *upcalls)
{
	if (ddi_prop_exists(DDI_DEV_T_ANY, ddi_root_node(), DDI_PROP_DONTPASS,
	    "efi-systab")) {
		pci_prd_have_bios = B_FALSE;
	}

	prd_upcalls = upcalls;

	/*
	 * The 8111's NMI-on-error behaviour has to be turned off before we go
	 * looking at anything else on the machine, and turned back on once we
	 * are done; see pci_prd_i86pc_apply_amd8111_fix().
	 */
	upcalls->pru_register_fix_f(pci_prd_i86pc_apply_amd8111_fix,
	    pci_prd_i86pc_undo_amd8111_fix);

	return (0);
}

/*
 * Retrieving LTSSM state, capturing link events, and retrieving or
 * programming link equalisation state are not currently implemented on i86pc.
 */
int
pci_prd_pcie_ltssm(dev_info_t *bridge __unused, pcie_ltssm_snap_t snap __unused,
    pcie_ltssm_snapshot_t *snapshot __unused)
{
	return (ENOTSUP);
}

void
pci_prd_pcie_link_event(dev_info_t *bridge __unused, boolean_t up __unused)
{
}

int
pci_prd_pcie_eq(dev_info_t *bridge __unused, pcie_link_speed_t speed __unused,
    uint32_t nlanes __unused, pcie_eq_t *eq __unused)
{
	return (ENOTSUP);
}

int
pci_prd_pcie_set_preset_mask(dev_info_t *bridge __unused,
    pcie_link_speed_t speed __unused, uint32_t mask __unused)
{
	return (ENOTSUP);
}

void
pci_prd_fini(void)
{
	int bus;

	for (bus = 0; bus <= pci_bios_maxbus; bus++) {
		memlist_rsrc_free(&acpi_io_res[bus]);
		memlist_rsrc_free(&acpi_mem_res[bus]);
		memlist_rsrc_free(&acpi_pmem_res[bus]);
		memlist_rsrc_free(&acpi_bus_res[bus]);
	}
}

const struct pci_boot_ops *
pci_prd_boot_ops(void)
{
	return (&pci_prd_i86pc_boot_ops);
}

static void
pci_prd_i86pc_undo_amd8111_fix(uint8_t bus, uint8_t dev, uint8_t fn)
{
	uint8_t val8;

	val8 = pci_getb(bus, dev, fn, LPC_IO_CONTROL_REG_1);
	/*
	 * The NMIONERR bit is turned back on to allow the SMM BIOS
	 * to handle more critical PCI errors (e.g. PERR#).
	 */
	val8 |= AMD8111_ENABLENMI;
	pci_putb(bus, dev, fn, LPC_IO_CONTROL_REG_1, val8);
}

static boolean_t
pci_prd_i86pc_apply_amd8111_fix(uint8_t bus, uint8_t dev, uint8_t fn,
    const pci_prop_data_t *prop)
{
	uint8_t val8;

	if (prop->ppd_vendid != VENID_AMD ||
	    prop->ppd_devid != DEVID_AMD8111_LPC) {
		return (B_FALSE);
	}

	val8 = pci_getb(bus, dev, fn, LPC_IO_CONTROL_REG_1);

	if ((val8 & AMD8111_ENABLENMI) == 0)
		return (B_FALSE);

	/*
	 * We reset NMIONERR in the LPC because master-abort on the PCI
	 * bridge side of the 8111 will cause NMI, which might cause SMI,
	 * which sometimes prevents all devices from being enumerated.
	 */
	val8 &= ~AMD8111_ENABLENMI;

	pci_putb(bus, dev, fn, LPC_IO_CONTROL_REG_1, val8);

	return (B_TRUE);
}

/*
 * Enable reporting of AER capability next pointer.
 * This needs to be done only for CK8-04 devices
 * by setting NV_XVR_VEND_CYA1 (offset 0xf40) bit 13
 * NOTE: BIOS is disabling this, it needs to be enabled temporarily
 *
 * This function is adapted from npe_ck804_fix_aer_ptr().
 */
static void
ck804_fix_aer_ptr(dev_info_t *dip, pcie_req_id_t bdf)
{
	dev_info_t *rcdip;
	ushort_t cya1;

	rcdip = pcie_get_rc_dip(dip);
	if (rcdip == NULL)
		return;

	if ((pci_cfgacc_get16(rcdip, bdf, PCI_CONF_VENID) ==
	    NVIDIA_VENDOR_ID) &&
	    (pci_cfgacc_get16(rcdip, bdf, PCI_CONF_DEVID) ==
	    NVIDIA_CK804_DEVICE_ID) &&
	    (pci_cfgacc_get8(rcdip, bdf, PCI_CONF_REVID) >=
	    NVIDIA_CK804_AER_VALID_REVID)) {
		cya1 = pci_cfgacc_get16(rcdip, bdf, NVIDIA_CK804_VEND_CYA1_OFF);
		if (!(cya1 & ~NVIDIA_CK804_VEND_CYA1_ERPT_MASK))
			(void) pci_cfgacc_put16(rcdip, bdf,
			    NVIDIA_CK804_VEND_CYA1_OFF,
			    cya1 | NVIDIA_CK804_VEND_CYA1_ERPT_VAL);
	}
}

static void
create_ioapic_node(int bus, int dev, int fn, ushort_t vendorid,
    ushort_t deviceid)
{
	static dev_info_t *ioapicsnode = NULL;
	static int numioapics = 0;
	dev_info_t *ioapic_node;
	uint64_t physaddr;
	uint32_t lobase, hibase = 0;

	/* BAR 0 contains the IOAPIC's memory-mapped I/O address */
	lobase = pci_getl(bus, dev, fn, PCI_CONF_BASE0);

	/* We (and the rest of the world) only support memory-mapped IOAPICs */
	if ((lobase & PCI_BASE_SPACE_M) != PCI_BASE_SPACE_MEM)
		return;

	if ((lobase & PCI_BASE_TYPE_M) == PCI_BASE_TYPE_ALL)
		hibase = pci_getl(bus, dev, fn, PCI_CONF_BASE0 + 4);

	lobase &= PCI_BASE_M_ADDR_M;

	physaddr = (((uint64_t)hibase) << 32) | lobase;

	/*
	 * Create a nexus node for all IOAPICs under the root node.
	 */
	if (ioapicsnode == NULL) {
		if (ndi_devi_alloc(ddi_root_node(), IOAPICS_NODE_NAME,
		    (pnode_t)DEVI_SID_NODEID, &ioapicsnode) != NDI_SUCCESS) {
			return;
		}
		(void) ndi_devi_online(ioapicsnode, 0);
	}

	/*
	 * Create a child node for this IOAPIC
	 */
	ioapic_node = ddi_add_child(ioapicsnode, IOAPICS_CHILD_NAME,
	    DEVI_SID_NODEID, numioapics++);
	if (ioapic_node == NULL) {
		return;
	}

	/* Vendor and Device ID */
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, ioapic_node,
	    IOAPICS_PROP_VENID, vendorid);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, ioapic_node,
	    IOAPICS_PROP_DEVID, deviceid);

	/* device_type */
	(void) ndi_prop_update_string(DDI_DEV_T_NONE, ioapic_node,
	    "device_type", IOAPICS_DEV_TYPE);

	/* reg */
	(void) ndi_prop_update_int64(DDI_DEV_T_NONE, ioapic_node,
	    "reg", physaddr);
}

/*
 * add_nvidia_isa_bridge_props():
 *	To enable native hotplug; we need to map in two I/O BARs
 *	from ISA bridge's config space
 *
 * NOTE: For now, this function is only used for Nvidia's CrushK 8-04 chipsets.
 */
static void
add_nvidia_isa_bridge_props(dev_info_t *dip, uchar_t bus, uchar_t dev,
    uchar_t func)
{
	uint_t devloc, base;
	pci_regspec_t regs[2] = {{0}};
	pci_regspec_t assigned[2] = {{0}};

	devloc = PCI_REG_MAKE_BDFR(bus, dev, func, 0);
	regs[0].pci_phys_hi = devloc;

	/* System Control BAR i/o space */
	base = pci_getl(bus, dev, func, NVIDIA_CK804_ISA_SYSCTRL_BAR_OFF);
	regs[0].pci_size_low = assigned[0].pci_size_low = PCI_CONF_HDR_SIZE;
	assigned[0].pci_phys_hi = regs[0].pci_phys_hi = (PCI_RELOCAT_B |
	    PCI_ADDR_IO | devloc | NVIDIA_CK804_ISA_SYSCTRL_BAR_OFF);
	assigned[0].pci_phys_low = regs[0].pci_phys_low =
	    base & PCI_BASE_IO_ADDR_M;

	/* Analog BAR i/o space */
	base = pci_getl(bus, dev, func, NVIDIA_CK804_ISA_ANALOG_BAR_OFF);
	regs[1].pci_size_low = assigned[1].pci_size_low = PCI_CONF_HDR_SIZE;
	assigned[1].pci_phys_hi = regs[1].pci_phys_hi = (PCI_RELOCAT_B |
	    PCI_ADDR_IO | devloc | NVIDIA_CK804_ISA_ANALOG_BAR_OFF);
	assigned[1].pci_phys_low = regs[1].pci_phys_low =
	    base & PCI_BASE_IO_ADDR_M;

	(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, dip, "reg",
	    (int *)regs, 2 * sizeof (pci_regspec_t) / sizeof (int));
	(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, dip,
	    "assigned-addresses",
	    (int *)assigned, 2 * sizeof (pci_regspec_t) / sizeof (int));
}

/*
 * As a workaround for devices which is_pciide() would not match due to device
 * issues, check an undocumented device tree property 'pci-ide', the value of
 * which is a 1275 device identifier.
 *
 * Should a device matching this (in normal 'compatible' order) be found, and
 * the device not otherwise bound, it will have its node name changed to
 * 'pci-ide' so the pci-ide driver will attach.
 *
 * This can be set via `eeprom pci-ide=pciXXXX,YYYY` (see eeprom(8)) or
 * otherwise added to bootenv.rc.
 */
static boolean_t
check_pciide_prop(uchar_t revid, ushort_t venid, ushort_t devid,
    ushort_t subvenid, ushort_t subdevid)
{
	static int prop_exist = -1;
	static char *pciide_str;
	char compat[32];

	if (prop_exist == -1) {
		prop_exist = (ddi_prop_lookup_string(DDI_DEV_T_ANY,
		    ddi_root_node(), DDI_PROP_DONTPASS, "pci-ide",
		    &pciide_str) == DDI_SUCCESS);
	}

	if (!prop_exist)
		return (B_FALSE);

	/* compare property value against various forms of compatible */
	if (subvenid) {
		(void) snprintf(compat, sizeof (compat), "pci%x,%x.%x.%x.%x",
		    venid, devid, subvenid, subdevid, revid);
		if (strcmp(pciide_str, compat) == 0)
			return (B_TRUE);

		(void) snprintf(compat, sizeof (compat), "pci%x,%x.%x.%x",
		    venid, devid, subvenid, subdevid);
		if (strcmp(pciide_str, compat) == 0)
			return (B_TRUE);

		(void) snprintf(compat, sizeof (compat), "pci%x,%x",
		    subvenid, subdevid);
		if (strcmp(pciide_str, compat) == 0)
			return (B_TRUE);
	}
	(void) snprintf(compat, sizeof (compat), "pci%x,%x.%x",
	    venid, devid, revid);
	if (strcmp(pciide_str, compat) == 0)
		return (B_TRUE);

	(void) snprintf(compat, sizeof (compat), "pci%x,%x", venid, devid);
	if (strcmp(pciide_str, compat) == 0)
		return (B_TRUE);

	return (B_FALSE);
}

static boolean_t
is_pciide(const pci_prop_data_t *prop)
{
	struct ide_table {
		ushort_t venid;
		ushort_t devid;
	};

	/*
	 * Devices which need to be matched specially as pci-ide because of
	 * various device issues.  Commonly their specification as being
	 * PCI_MASS_OTHER or PCI_MASS_SATA despite our using them in ATA mode.
	 */
	static struct ide_table ide_other[] = {
		{0x1095, 0x3112}, /* Silicon Image 3112 SATALink/SATARaid */
		{0x1095, 0x3114}, /* Silicon Image 3114 SATALink/SATARaid */
		{0x1095, 0x3512}, /* Silicon Image 3512 SATALink/SATARaid */
		{0x1095, 0x680},  /* Silicon Image PCI0680 Ultra ATA-133 */
		{0x1283, 0x8211} /* Integrated Technology Express 8211F */
	};

	if (prop->ppd_class != PCI_CLASS_MASS)
		return (B_FALSE);

	if (prop->ppd_subclass == PCI_MASS_IDE) {
		return (B_TRUE);
	}

	if (check_pciide_prop(prop->ppd_rev, prop->ppd_vendid,
	    prop->ppd_devid, prop->ppd_subvid, prop->ppd_subsys)) {
		return (B_TRUE);
	}

	if (prop->ppd_subclass != PCI_MASS_OTHER &&
	    prop->ppd_subclass != PCI_MASS_SATA) {
		return (B_FALSE);
	}

	for (size_t i = 0; i < ARRAY_SIZE(ide_other); i++) {
		if (ide_other[i].venid == prop->ppd_vendid &&
		    ide_other[i].devid == prop->ppd_devid)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * config info for pci-ide devices
 */
static struct {
	uchar_t  native_mask;	/* 0 == 'compatibility' mode, 1 == native */
	uchar_t  bar_offset;	/* offset for alt status register */
	ushort_t addr;		/* compatibility mode base address */
	ushort_t length;	/* number of ports for this BAR */
} pciide_bar[] = {
	{ 0x01, 0, 0x1f0, 8 },	/* primary lower BAR */
	{ 0x01, 2, 0x3f6, 1 },	/* primary upper BAR */
	{ 0x04, 0, 0x170, 8 },	/* secondary lower BAR */
	{ 0x04, 2, 0x376, 1 }	/* secondary upper BAR */
};

static boolean_t
pciide_adjust_bar(uchar_t progcl, uint_t bar, uint_t *basep, uint_t *lenp)
{
	boolean_t hard_decode = B_FALSE;

	/*
	 * Adjust the base and len for the BARs of the PCI-IDE
	 * device's primary and secondary controllers. The first
	 * two BARs are for the primary controller and the next
	 * two BARs are for the secondary controller. The fifth
	 * and sixth bars are never adjusted.
	 */
	if (bar <= 3) {
		*lenp = pciide_bar[bar].length;

		if (progcl & pciide_bar[bar].native_mask) {
			*basep += pciide_bar[bar].bar_offset;
		} else {
			*basep = pciide_bar[bar].addr;
			hard_decode = B_TRUE;
		}
	}

	/*
	 * if either base or len is zero make certain both are zero
	 */
	if (*basep == 0 || *lenp == 0) {
		*basep = 0;
		*lenp = 0;
		hard_decode = B_FALSE;
	}

	return (hard_decode);
}

/*
 * A device is a legacy IDE controller if it says so, or if it is one of the
 * devices known not to say so correctly.  Such a controller in compatibility
 * mode decodes the fixed ATA task file ports rather than the ones its base
 * address registers name, which is what the three hooks below are for.
 */
static boolean_t
pci_prd_i86pc_claim(dev_info_t *dip, uint8_t bus __unused, uint8_t dev
    __unused, uint8_t func __unused, const pci_prop_data_t *prop)
{
	if (!is_pciide(prop))
		return (B_FALSE);

	/*
	 * If some driver of higher precedence in driver_aliases will claim
	 * the node, leave it alone: it is not ours to rename, and its
	 * registers are whatever its base address registers say they are.
	 */
	if (ddi_compatible_driver_major(dip, NULL) != (major_t)-1)
		return (B_FALSE);

	(void) ndi_devi_set_nodename(dip, "pci-ide", 0);

	return (B_TRUE);
}

static void
pci_prd_i86pc_claimed(dev_info_t *dip, uint8_t bus __unused,
    uint8_t dev __unused, uint8_t func __unused,
    const pci_prop_data_t *prop)
{
	dev_info_t *cdip;

	VERIFY(is_pciide(prop));
	VERIFY(strcmp(ddi_node_name(dip), "pci-ide") == 0);

	/*
	 * Create properties specified by P1275 Working Group
	 * Proposal #414 Version 1
	 */
	(void) ndi_prop_update_string(DDI_DEV_T_NONE, dip,
	    "device_type", "pci-ide");
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, dip,
	    "#address-cells", 1);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, dip,
	    "#size-cells", 0);

	/* allocate two child nodes */
	ndi_devi_alloc_sleep(dip, "ide", (pnode_t)DEVI_SID_NODEID, &cdip);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip, "reg", 0);
	(void) ndi_devi_bind_driver(cdip, 0);
	ndi_devi_alloc_sleep(dip, "ide", (pnode_t)DEVI_SID_NODEID, &cdip);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip, "reg", 1);
	(void) ndi_devi_bind_driver(cdip, 0);
}

static boolean_t
pci_prd_i86pc_io_bar(dev_info_t *dip, uint8_t bus, uint8_t dev, uint8_t func,
    uint_t bar, uint32_t *basep, uint_t *lenp, boolean_t *hard_decodep)
{
	uint8_t subclass = pci_getb(bus, dev, func, PCI_CONF_SUBCLASS);
	uint8_t progclass = pci_getb(bus, dev, func, PCI_CONF_PROGCLASS);
	uint_t adjbase = *basep;

	VERIFY(strcmp(ddi_node_name(dip), "pci-ide") == 0);

	/*
	 * Only the first four registers, which are the primary and secondary
	 * controllers' pairs, are ours to describe.  The rest we leave alone
	 * unless the device says they are I/O space, in which case we still
	 * pass them through the adjustment below so that an empty register is
	 * consistently reported as such.
	 */
	if (bar >= 4 && (*basep & PCI_BASE_SPACE_IO) == 0)
		return (B_FALSE);

	/*
	 * A device we recognized in spite of its class needs to be treated as
	 * though both of its channels were in native mode.
	 */
	if (subclass != PCI_MASS_IDE) {
		progclass = (PCI_IDE_IF_NATIVE_PRI | PCI_IDE_IF_NATIVE_SEC);
	}

	*hard_decodep = pciide_adjust_bar(progclass, bar, &adjbase, lenp);
	*basep = adjbase;

	return (B_TRUE);
}

/*
 * The regions a PC places at fixed addresses by convention: the legacy VGA
 * ranges.  A video adapter decodes these in addition to its base address
 * registers (see pci_prd_i86pc_aliases()), and a bridge told to forward VGA
 * passes them to its secondary bus, so enumeration has to recognize them
 * wherever they appear rather than treat them as space it may assign.
 *
 * The 8514 ranges are deliberately not here: they belong to the device that
 * has them, and bridges do not forward them.
 */
static const pci_boot_region_t pci_prd_i86pc_vga_ranges[] = {
	/* VGA hard decode 0x3b0-0x3bb */
	{ .pbr_io = B_TRUE,  .pbr_base = 0x3b0,   .pbr_len = 0xc },
	/* VGA hard decode 0x3c0-0x3df */
	{ .pbr_io = B_TRUE,  .pbr_base = 0x3c0,   .pbr_len = 0x20 },
	/* Video memory */
	{ .pbr_io = B_FALSE, .pbr_base = 0xa0000, .pbr_len = 0x20000 }
};

/*
 * A PC's video adapter decodes the legacy VGA addresses in addition to
 * whatever its base address registers describe, and an 8514-compatible one
 * decodes a further pair of legacy port ranges.  None of this is discoverable;
 * it is what a PC has always done, so we tell enumeration about it.
 */
static uint_t
pci_prd_i86pc_aliases(uint8_t bus, uint8_t dev, uint8_t func,
    pci_boot_region_t *alias, uint_t nmax)
{
	uint8_t baseclass = pci_getb(bus, dev, func, PCI_CONF_BASCLASS);
	uint8_t subclass = pci_getb(bus, dev, func, PCI_CONF_SUBCLASS);
	uint8_t progclass = pci_getb(bus, dev, func, PCI_CONF_PROGCLASS);
	uint_t n = 0;

	if ((baseclass != PCI_CLASS_DISPLAY || subclass != PCI_DISPLAY_VGA) &&
	    (baseclass != PCI_CLASS_NONE || subclass != PCI_NONE_VGA)) {
		return (0);
	}

	n = ARRAY_SIZE(pci_prd_i86pc_vga_ranges);
	VERIFY3U(n, <=, nmax);
	bcopy(pci_prd_i86pc_vga_ranges, alias, sizeof (*alias) * n);

	/* the hard-decode, aliased address spaces for 8514 */
	if (baseclass == PCI_CLASS_DISPLAY && subclass == PCI_DISPLAY_VGA &&
	    (progclass & PCI_DISPLAY_IF_8514) != 0) {
		/* hard decode 0x2e8 */
		VERIFY3U(n, <, nmax);
		alias[n].pbr_io = B_TRUE;
		alias[n].pbr_base = 0x2e8;
		alias[n].pbr_len = 0x1;
		n++;

		/* hard decode 0x2ea-0x2ef */
		VERIFY3U(n, <, nmax);
		alias[n].pbr_io = B_TRUE;
		alias[n].pbr_base = 0x2ea;
		alias[n].pbr_len = 0x6;
		n++;
	}

	return (n);
}

/*
 * Applied as each function is enumerated, before anything reads its
 * configuration space through the PCIe framework.
 */
static void
pci_prd_i86pc_devinit(dev_info_t *dip, uint8_t bus, uint8_t dev, uint8_t func,
    const pci_prop_data_t *prop)
{
	pcie_req_id_t bdf = PCI_GETBDF(bus, dev, func);

	/*
	 * Record BAD AMD bridges which don't support MMIO config access.
	 */
	if (IS_BAD_AMD_NTBRIDGE(prop->ppd_vendid, prop->ppd_devid) ||
	    IS_AMD_8132_CHIP(prop->ppd_vendid, prop->ppd_devid)) {
		uchar_t secbus = 0;
		uchar_t subbus = 0;

		if (pci_prop_class_is_pcibridge(prop)) {
			secbus = pci_getb(bus, dev, func, PCI_BCNF_SECBUS);
			subbus = pci_getb(bus, dev, func, PCI_BCNF_SUBBUS);
		}
		pci_cfgacc_add_workaround(bdf, secbus, subbus);
	}

	ck804_fix_aer_ptr(dip, bdf);
}

/*
 * Applied once a function's device node has been fully populated.
 */
static void
pci_prd_i86pc_devdone(dev_info_t *dip, uint8_t bus, uint8_t dev, uint8_t func,
    const pci_prop_data_t *prop)
{
	if (pci_prop_class_is_ioapic(prop)) {
		create_ioapic_node(bus, dev, func, prop->ppd_vendid,
		    prop->ppd_devid);
	}

	/* check for NVIDIA CK8-04/MCP55 based LPC bridge */
	if (NVIDIA_IS_LPC_BRIDGE(prop->ppd_vendid, prop->ppd_devid) &&
	    dev == 1 && func == 0) {
		add_nvidia_isa_bridge_props(dip, bus, dev, func);
		/* each LPC bridge has an integrated IOAPIC */
		apic_nvidia_io_max++;
	}

	/*
	 * Remember the graphics devices we come across.  The AMD IOMMU driver
	 * walks this list to find the devices whose translations it must set
	 * up before the framebuffer console can be used.
	 */
	if (pci_prop_class_is_vga(prop)) {
		gfx_entry_t *gfxp;

		gfxp = kmem_zalloc(sizeof (*gfxp), KM_SLEEP);
		gfxp->g_dip = dip;
		gfxp->g_prev = NULL;
		gfxp->g_next = gfx_devinfo_list;
		gfx_devinfo_list = gfxp;
		if (gfxp->g_next)
			gfxp->g_next->g_prev = gfxp;
	}
}


static boolean_t
pci_prd_i86pc_legacy_range(uint64_t base, uint64_t len, boolean_t io)
{
	for (uint_t i = 0; i < ARRAY_SIZE(pci_prd_i86pc_vga_ranges); i++) {
		const pci_boot_region_t *r = &pci_prd_i86pc_vga_ranges[i];

		if (r->pbr_io == io && r->pbr_base == base &&
		    r->pbr_len == len) {
			return (B_TRUE);
		}
	}

	return (B_FALSE);
}

/*
 * A bridge with VGA forwarding enabled passes the legacy VGA ranges to its
 * secondary bus whatever its windows say; every other bridge forwards
 * nothing of the sort.
 */
static uint_t
pci_prd_i86pc_bridge_regions(uint8_t bus, uint8_t dev, uint8_t func,
    pci_boot_region_t *ranges, uint_t nmax)
{
	uint_t n = ARRAY_SIZE(pci_prd_i86pc_vga_ranges);

	if ((pci_getw(bus, dev, func, PCI_BCNF_BCNTRL) &
	    PCI_BCNF_BCNTRL_VGA_ENABLE) == 0) {
		return (0);
	}

	VERIFY3U(n, <=, nmax);
	bcopy(pci_prd_i86pc_vga_ranges, ranges, sizeof (*ranges) * n);

	return (n);
}

static struct modlmisc pci_prd_modlmisc_i86pc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "i86pc PCI Resource Discovery"
};

static struct modlinkage pci_prd_modlinkage_i86pc = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &pci_prd_modlmisc_i86pc, NULL }
};

int
_init(void)
{
	return (mod_install(&pci_prd_modlinkage_i86pc));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&pci_prd_modlinkage_i86pc, modinfop));
}

int
_fini(void)
{
	return (mod_remove(&pci_prd_modlinkage_i86pc));
}
