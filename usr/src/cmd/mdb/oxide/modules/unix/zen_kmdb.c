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
 * This implements several dcmds for getting at state for use in kmdb. Several
 * of these kind of assume that someone else isn't doing something with them at
 * the same time that we are (mostly because there are only so many slots that
 * can be used for different purposes.
 */

#include <mdb/mdb_modapi.h>
#include <kmdb/kmdb_modext.h>
#include <sys/pci.h>
#include <sys/pcie.h>
#include <sys/pcie_impl.h>
#include <sys/sysmacros.h>
#include <milan/milan_physaddrs.h>
#include <sys/amdzen/ccx.h>
#include <sys/amdzen/umc.h>
#include <io/amdzen/amdzen.h>
#include "zen_umc.h"

static uint64_t pcicfg_physaddr;
static boolean_t pcicfg_valid;

/*
 * These variables, when set, contain a discovered fabric ID.
 * XXX: fold into df_ops?
 */
static boolean_t df_masks_valid;
static uint32_t df_node_shift;
static uint32_t df_node_mask;
static uint32_t df_comp_mask;
static void
df_print_dest(uint32_t dest);

typedef struct df_comp {
	uint_t dc_inst;
	uint_t dc_comp;
	const char *dc_name;
	uint_t dc_ndram;
} df_comp_t;

typedef struct df_ops {
	uint32_t dfo_supported_gens;

	uint32_t dfo_comp_names_count;
	const df_comp_t *dfo_comp_names;

	uint32_t dfo_chan_ileaves_count;
	const char **dfo_chan_ileaves;

	boolean_t (*dfo_read32_indirect_raw)(uint64_t sock, uintptr_t inst,
	    uintptr_t func, uint16_t reg, uint32_t *valp);
	boolean_t (*dfo_write32_indirect_raw)(uint64_t sock, uintptr_t inst,
	    uintptr_t func, uint16_t reg, uint32_t valp);

	boolean_t (*dfo_get_smn_busno)(uint64_t sock, uint8_t *busno);

	boolean_t (*dfo_fetch_masks)(void);

	uintptr_t dfo_dram_io_inst;
	uintptr_t dfo_mmio_pci_inst;

	void (*dfo_route_buses)(uint64_t sock, uintptr_t inst);
	void (*dfo_route_dram)(uint64_t sock, uintptr_t inst, uint_t ndram);
	void (*dfo_route_ioports)(uint64_t sock, uintptr_t inst);
	void (*dfo_route_mmio)(uint64_t sock, uintptr_t inst);
} df_ops_t;

/*
 * Specific definitions and functions to use per-supported chiprev.
 */
static const df_ops_t *df_ops = NULL;

static boolean_t
df_read32(uint64_t sock, const df_reg_def_t df, uint32_t *valp);
static boolean_t
df_write32(uint64_t sock, const df_reg_def_t df, uint32_t val);

/*
 * Milan
 */

static df_comp_t df_comp_names_milan[] = {
	{ 0, 0, "UMC0", 2 },
	{ 1, 1, "UMC1", 2 },
	{ 2, 2, "UMC2", 2 },
	{ 3, 3, "UMC3", 2 },
	{ 4, 4, "UMC4", 2 },
	{ 5, 5, "UMC5", 2 },
	{ 6, 6, "UMC6", 2 },
	{ 7, 7, "UMC7", 2 },
	{ 8, 8, "CCIX0", 2 },
	{ 9, 9, "CCIX1", 2 },
	{ 10, 10, "CCIX2", 2 },
	{ 11, 11, "CCIX3", 2 },
	{ 16, 16, "CCM0", 16 },
	{ 17, 17, "CCM1", 16 },
	{ 18, 18, "CCM2", 16 },
	{ 19, 19, "CCM3", 16 },
	{ 20, 20, "CCM4", 16 },
	{ 21, 21, "CCM5", 16 },
	{ 22, 22, "CCM6", 16 },
	{ 23, 23, "CCM7", 16 },
	{ 24, 24, "IOMS0", 16 },
	{ 25, 25, "IOMS1", 16 },
	{ 26, 26, "IOMS2", 16 },
	{ 27, 27, "IOMS3", 16 },
	{ 30, 30, "PIE0", 8 },
	{ 31, -1, "CAKE0" },
	{ 32, -1, "CAKE1" },
	{ 33, -1, "CAKE2" },
	{ 34, -1, "CAKE3" },
	{ 35, -1, "CAKE4" },
	{ 36, -1, "CAKE5" },
	{ 37, -1, "TCDX0" },
	{ 38, -1, "TCDX1" },
	{ 39, -1, "TCDX2" },
	{ 40, -1, "TCDX3" },
	{ 41, -1, "TCDX4" },
	{ 42, -1, "TCDX5" },
	{ 43, -1, "TCDX6" },
	{ 44, -1, "TCDX7" },
	{ 45, -1, "TCDX8" },
	{ 46, -1, "TCDX9" },
	{ 47, -1, "TCDX10" },
	{ 48, -1, "TCDX11" }
};

static const char *df_chan_ileaves_milan[16] = {
	"1", "2", "Reserved", "4",
	"Reserved", "8", "6", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"COD-4 2", "COD-2 4", "COD-1 8", "Reserved"
};

static boolean_t
df_read32_indirect_raw_milan(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t *valp)
{
	uint32_t val = 0;

	val = DF_FICAA_V2_SET_TARG_INST(val, 1);
	val = DF_FICAA_V2_SET_FUNC(val, func);
	val = DF_FICAA_V2_SET_INST(val, inst);
	val = DF_FICAA_V2_SET_64B(val, 0);
	val = DF_FICAA_V2_SET_REG(val, reg >> 2);

	if (!df_write32(sock, DF_FICAA_V2, val)) {
		return (B_FALSE);
	}

	if (!df_read32(sock, DF_FICAD_LO_V2, &val)) {
		return (B_FALSE);
	}

	*valp = val;
	return (B_TRUE);
}

static boolean_t
df_write32_indirect_raw_milan(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t val)
{
	uint32_t rval = 0;

	rval = DF_FICAA_V2_SET_TARG_INST(rval, 1);
	rval = DF_FICAA_V2_SET_FUNC(rval, func);
	rval = DF_FICAA_V2_SET_INST(rval, inst);
	rval = DF_FICAA_V2_SET_64B(rval, 0);
	rval = DF_FICAA_V2_SET_REG(rval, reg >> 2);

	if (!df_write32(sock, DF_FICAA_V2, rval)) {
		return (B_FALSE);
	}

	if (!df_write32(sock, DF_FICAD_LO_V2, val)) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
df_get_smn_busno_milan(uint64_t sock, uint8_t *busno)
{
	uint32_t df_busctl;

	if (!df_read32(sock, DF_CFG_ADDR_CTL_V2, &df_busctl)) {
		mdb_warn("failed to read DF config address\n");
		return (B_FALSE);
	}

	if (df_busctl == PCI_EINVAL32) {
		mdb_warn("got back PCI_EINVAL32 when reading from the df\n");
		return (B_FALSE);
	}

	*busno = DF_CFG_ADDR_CTL_GET_BUS_NUM(df_busctl);

	return (B_TRUE);
}

static boolean_t
df_fetch_masks_milan(void)
{
	uint32_t fid0, fid1;

	if (!df_read32(0, DF_FIDMASK0_V3, &fid0) ||
	    !df_read32(0, DF_FIDMASK1_V3, &fid1)) {
		mdb_warn("failed to read masks register\n");
		return (B_FALSE);
	}

	df_node_mask = DF_FIDMASK0_V3_GET_NODE_MASK(fid0);
	df_comp_mask = DF_FIDMASK0_V3_GET_COMP_MASK(fid0);
	df_node_shift = DF_FIDMASK1_V3_GET_NODE_SHIFT(fid1);

	return (B_TRUE);
}

static void
df_route_buses_milan(uint64_t sock, uintptr_t inst)
{
	uint32_t val;

	for (uint_t i = 0; i < DF_MAX_CFGMAP; i++) {
		const df_reg_def_t def = DF_CFGMAP_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, def.drd_func,
		    def.drd_reg, &val)) {
			mdb_warn("failed to read cfgmap %u\n", i);
			continue;
		}

		if (val == PCI_EINVAL32) {
			mdb_warn("got back invalid read for cfgmap %u\n", i);
			continue;
		}

		mdb_printf("%-7#x %-7#x %c%c       ",
		    DF_CFGMAP_V2_GET_BUS_BASE(val),
		    DF_CFGMAP_V2_GET_BUS_LIMIT(val),
		    DF_CFGMAP_V2_GET_RE(val) ? 'R' : '-',
		    DF_CFGMAP_V2_GET_WE(val) ? 'W' : '-');
		df_print_dest(DF_CFGMAP_V3_GET_DEST_ID(val));
		mdb_printf("\n");
	}
}

static void
df_route_dram_milan(uint64_t sock, uintptr_t inst, uint_t ndram)
{
	for (uint_t i = 0; i < ndram; i++) {
		uint32_t breg, lreg;
		uint64_t base, limit;
		const char *chan;
		char ileave[16];

		const df_reg_def_t bdef = DF_DRAM_BASE_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read DRAM port base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_DRAM_LIMIT_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read DRAM port limit %u\n", i);
			continue;
		}

		base = DF_DRAM_BASE_V2_GET_BASE(breg);
		base <<= DF_DRAM_BASE_V2_BASE_SHIFT;
		limit = DF_DRAM_LIMIT_V2_GET_LIMIT(lreg);
		limit <<= DF_DRAM_LIMIT_V2_LIMIT_SHIFT;
		limit += DF_DRAM_LIMIT_V2_LIMIT_EXCL - 1;

		chan = df_chan_ileaves_milan[
		    DF_DRAM_BASE_V3_GET_ILV_CHAN(breg)];
		(void) mdb_snprintf(ileave, sizeof (ileave), "%u/%s/%u/%u",
		    DF_DRAM_BASE_V3_GET_ILV_ADDR(breg) + DF_DRAM_ILV_ADDR_BASE,
		    chan, DF_DRAM_BASE_V3_GET_ILV_DIE(breg) + 1,
		    DF_DRAM_BASE_V3_GET_ILV_SOCK(breg) + 1);

		mdb_printf("%-?#lx %-?#lx %c%c%c     %-15s ", base, limit,
		    DF_DRAM_BASE_V2_GET_VALID(breg) ? 'V' : '-',
		    DF_DRAM_BASE_V2_GET_HOLE_EN(breg) ? 'H' : '-',
		    DF_DRAM_LIMIT_V3_GET_BUS_BREAK(lreg) ?
		    'B' : '-', ileave);
		df_print_dest(DF_DRAM_LIMIT_V3_GET_DEST_ID(lreg));
		mdb_printf("\n");
	}
}

static void
df_route_ioports_milan(uint64_t sock, uintptr_t inst)
{
	for (uint_t i = 0; i < DF_MAX_IO_RULES; i++) {
		uint32_t breg, lreg, base, limit;

		const df_reg_def_t bdef = DF_IO_BASE_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read I/O port base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_IO_LIMIT_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read I/O port limit %u\n", i);
			continue;
		}

		base = DF_IO_BASE_V2_GET_BASE(breg);
		base <<= DF_IO_BASE_SHIFT;
		limit = DF_IO_LIMIT_V2_GET_LIMIT(lreg);
		limit <<= DF_IO_LIMIT_SHIFT;
		limit += DF_IO_LIMIT_EXCL - 1;

		mdb_printf("%-8#x %-8#x %c%c%c      ", base, limit,
		    DF_IO_BASE_V2_GET_RE(breg) ? 'R' : '-',
		    DF_IO_BASE_V2_GET_WE(breg) ? 'W' : '-',
		    DF_IO_BASE_V2_GET_IE(breg) ? 'I' : '-');
		df_print_dest(DF_IO_LIMIT_V3_GET_DEST_ID(lreg));
		mdb_printf("\n");
	}
}

static void
df_route_mmio_milan(uint64_t sock, uintptr_t inst)
{
	for (uint_t i = 0; i < DF_MAX_MMIO_RULES; i++) {
		uint32_t breg, lreg, creg;
		uint64_t base, limit;

		const df_reg_def_t bdef = DF_MMIO_BASE_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read MMIO base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_MMIO_LIMIT_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read MMIO limit %u\n", i);
			continue;
		}

		const df_reg_def_t cdef = DF_MMIO_CTL_V2(i);
		if (!df_read32_indirect_raw_milan(sock, inst, cdef.drd_func,
		    cdef.drd_reg, &creg)) {
			mdb_warn("failed to read MMIO control %u\n", i);
			continue;
		}

		base = (uint64_t)breg << DF_MMIO_SHIFT;
		limit = (uint64_t)lreg << DF_MMIO_SHIFT;
		limit += DF_MMIO_LIMIT_EXCL - 1;

		mdb_printf("%-?#lx %-?#lx %c%c%c%c     ", base, limit,
		    DF_MMIO_CTL_GET_RE(creg) ? 'R' : '-',
		    DF_MMIO_CTL_GET_WE(creg) ? 'W' : '-',
		    DF_MMIO_CTL_V3_GET_NP(creg) ? 'N' : '-',
		    DF_MMIO_CTL_GET_CPU_DIS(creg) ? 'C' : '-');
		df_print_dest(DF_MMIO_CTL_V3_GET_DEST_ID(creg));
		mdb_printf("\n");
	}
}

static const df_ops_t df_ops_milan = {
	.dfo_supported_gens = DF_REV_3,
	.dfo_comp_names_count = ARRAY_SIZE(df_comp_names_milan),
	.dfo_comp_names = df_comp_names_milan,
	.dfo_chan_ileaves_count = ARRAY_SIZE(df_chan_ileaves_milan),
	.dfo_chan_ileaves = df_chan_ileaves_milan,
	.dfo_read32_indirect_raw = df_read32_indirect_raw_milan,
	.dfo_write32_indirect_raw = df_write32_indirect_raw_milan,
	.dfo_get_smn_busno = df_get_smn_busno_milan,
	.dfo_fetch_masks = df_fetch_masks_milan,
	/*
	 * For DRAM, default to CCM0 (we don't use a UMC because it has very few
	 * rules). For I/O ports, use CCM0 as well as the IOMS entries don't
	 * really have rules here. For MMIO and PCI buses, use IOMS0.
	 */
	.dfo_dram_io_inst = 16,
	.dfo_mmio_pci_inst = 24,
	.dfo_route_buses = df_route_buses_milan,
	.dfo_route_dram = df_route_dram_milan,
	.dfo_route_ioports = df_route_ioports_milan,
	.dfo_route_mmio = df_route_mmio_milan,
};

/*
 * Genoa
 */

static df_comp_t df_comp_names_genoa[] = {
	{ 0, 0, "UMC0", 4 },
	{ 1, 1, "UMC1", 4 },
	{ 2, 2, "UMC2", 4 },
	{ 3, 3, "UMC3", 4 },
	{ 4, 4, "UMC4", 4 },
	{ 5, 5, "UMC5", 4 },
	{ 6, 6, "UMC6", 4 },
	{ 7, 7, "UMC7", 4 },
	{ 8, 8, "UMC8", 4 },
	{ 9, 9, "UMC9", 4 },
	{ 10, 10, "UMC10", 4 },
	{ 11, 11, "UMC11", 4 },
	{ 12, 12, "CMP0", 4 },
	{ 13, 13, "CMP1", 4 },
	{ 14, 14, "CMP2", 4 },
	{ 15, 15, "CMP3", 4 },
	{ 16, 96, "CCM0", 20 },
	{ 17, 97, "CCM1", 20 },
	{ 18, 98, "CCM2", 20 },
	{ 19, 99, "CCM3", 20 },
	{ 20, 100, "CCM4", 20 },
	{ 21, 101, "CCM5", 20 },
	{ 22, 102, "CCM6", 20 },
	{ 23, 103, "CCM7", 20 },
	{ 24, 108, "ACM0", 20 },
	{ 25, 109, "ACM1", 20 },
	{ 26, 110, "ACM2", 20 },
	{ 27, 111, "ACM3", 20 },
	{ 28, 112, "NCM0_IOMMU0", 20 },
	{ 29, 113, "NCM1_IOMMU1", 20 },
	{ 30, 114, "NCM2_IOMMU2", 20 },
	{ 31, 115, "NCM3_IOMMU3", 20 },
	{ 32, 120, "IOM0_IOHUBM0", 20 },
	{ 33, 121, "IOM1_IOHUBM1", 20 },
	{ 34, 122, "IOM2_IOHUBM2", 20 },
	{ 35, 123, "IOM3_IOHUBM3", 20 },
	{ 36, 32, "IOHUBS0", 1 },
	{ 37, 33, "IOHUBS1", 1 },
	{ 38, 34, "IOHUBS2", 1 },
	{ 39, 35, "IOHUBS3", 1 },
	{ 40, 124, "ICNG0" },
	{ 41, 125, "ICNG1" },
	{ 42, 126, "ICNG2" },
	{ 43, 127, "ICNG3" },
	{ 44, 119, "PIE0", 20 },
	{ 45, -1, "CAKE0" },
	{ 46, -1, "CAKE1" },
	{ 47, -1, "CAKE2" },
	{ 48, -1, "CAKE3" },
	{ 49, -1, "CAKE4" },
	{ 50, -1, "CAKE5" },
	{ 51, -1, "CAKE6" },
	{ 52, -1, "CAKE7" },
	{ 53, -1, "CNLI0" },
	{ 54, -1, "CNLI1" },
	{ 55, -1, "CNLI2" },
	{ 56, -1, "CNLI3" },
	{ 57, -1, "PFX0" },
	{ 58, -1, "PFX1" },
	{ 59, -1, "PFX2" },
	{ 60, -1, "PFX3" },
	{ 61, -1, "PFX4" },
	{ 62, -1, "PFX5" },
	{ 63, -1, "PFX6" },
	{ 64, -1, "PFX7" },
	{ 65, -1, "SPF0", 8 },
	{ 66, -1, "SPF1", 8 },
	{ 67, -1, "SPF2", 8 },
	{ 68, -1, "SPF3", 8 },
	{ 69, -1, "SPF4", 8 },
	{ 70, -1, "SPF5", 8 },
	{ 71, -1, "SPF6", 8 },
	{ 72, -1, "SPF7", 8 },
	{ 73, -1, "SPF8", 8 },
	{ 74, -1, "SPF9", 8 },
	{ 75, -1, "SPF10", 8 },
	{ 76, -1, "SPF11", 8 },
	{ 77, -1, "SPF12", 8 },
	{ 78, -1, "SPF13", 8 },
	{ 79, -1, "SPF14", 8 },
	{ 80, -1, "SPF15", 8 },
	{ 81, -1, "TCDX0" },
	{ 82, -1, "TCDX1" },
	{ 83, -1, "TCDX2" },
	{ 84, -1, "TCDX3" },
	{ 85, -1, "TCDX4" },
	{ 86, -1, "TCDX5" },
	{ 87, -1, "TCDX6" },
	{ 88, -1, "TCDX7" },
	{ 89, -1, "TCDX8" },
	{ 90, -1, "TCDX9" },
	{ 91, -1, "TCDX10" },
	{ 92, -1, "TCDX11" },
	{ 93, -1, "TCDX12" },
	{ 94, -1, "TCDX13" },
	{ 95, -1, "TCDX14" },
	{ 96, -1, "TCDX15" }
};

static const char *df_chan_ileaves_genoa[32] = {
	"1", "2", "Reserved", "4",
	"Reserved", "8", "Reserved", "16",
	"32", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"NPS-4 2", "NPS-2 4", "NPS-1 8", "NPS-4 3",
	"NPS-2 6", "NPS-1 12", "NPS-2 5", "NPS-1 10",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
};

static boolean_t
df_read32_indirect_raw_genoa(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t *valp)
{
	uint32_t val = 0;

	val = DF_FICAA_V2_SET_TARG_INST(val, 1);
	val = DF_FICAA_V2_SET_FUNC(val, func);
	val = DF_FICAA_V2_SET_INST(val, inst);
	val = DF_FICAA_V2_SET_64B(val, 0);
	val = DF_FICAA_V4_SET_REG(val, reg >> 2);

	if (!df_write32(sock, DF_FICAA_V4, val)) {
		return (B_FALSE);
	}

	if (!df_read32(sock, DF_FICAD_LO_V4, &val)) {
		return (B_FALSE);
	}

	*valp = val;
	return (B_TRUE);
}

static boolean_t
df_write32_indirect_raw_genoa(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t val)
{
	uint32_t rval = 0;

	rval = DF_FICAA_V2_SET_TARG_INST(rval, 1);
	rval = DF_FICAA_V2_SET_INST(rval, inst);
	rval = DF_FICAA_V2_SET_FUNC(rval, func);
	rval = DF_FICAA_V2_SET_64B(rval, 0);
	rval = DF_FICAA_V4_SET_REG(rval, reg >> 2);

	if (!df_write32(sock, DF_FICAA_V4, rval)) {
		return (B_FALSE);
	}

	if (!df_write32(sock, DF_FICAD_LO_V4, val)) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
df_get_smn_busno_genoa(uint64_t sock, uint8_t *busno)
{
	uint32_t df_busctl;

	if (!df_read32(sock, DF_CFG_ADDR_CTL_V4, &df_busctl)) {
		mdb_warn("failed to read DF config address\n");
		return (B_FALSE);
	}

	if (df_busctl == PCI_EINVAL32) {
		mdb_warn("got back PCI_EINVAL32 when reading from the df\n");
		return (B_FALSE);
	}

	*busno = DF_CFG_ADDR_CTL_GET_BUS_NUM(df_busctl);

	return (B_TRUE);
}

static boolean_t
df_fetch_masks_genoa(void)
{
	uint32_t fid0, fid1;

	if (!df_read32(0, DF_FIDMASK0_V4, &fid0) ||
	    !df_read32(0, DF_FIDMASK1_V4, &fid1)) {
		mdb_warn("failed to read masks register\n");
		return (B_FALSE);
	}

	df_node_mask = DF_FIDMASK0_V3P5_GET_NODE_MASK(fid0);
	df_comp_mask = DF_FIDMASK0_V3P5_GET_COMP_MASK(fid0);
	df_node_shift = DF_FIDMASK1_V3P5_GET_NODE_SHIFT(fid1);

	return (B_TRUE);
}

static void
df_route_buses_genoa(uint64_t sock, uintptr_t inst)
{
	uint32_t breg, lreg;

	for (uint_t i = 0; i < DF_MAX_CFGMAP; i++) {
		const df_reg_def_t bdef = DF_CFGMAP_BASE_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read cfgmap base %u\n", i);
			continue;
		}
		if (breg == PCI_EINVAL32) {
			mdb_warn("got back invalid read for cfgmap base %u\n",
			    i);
			continue;
		}

		const df_reg_def_t ldef = DF_CFGMAP_LIMIT_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read cfgmap limit %u\n", i);
			continue;
		}
		if (lreg == PCI_EINVAL32) {
			mdb_warn("got back invalid read for cfgmap limit %u\n",
			    i);
			continue;
		}

		mdb_printf("%-7#x %-7#x %c%c       ",
		    DF_CFGMAP_BASE_V4_GET_BASE(breg),
		    DF_CFGMAP_LIMIT_V4_GET_LIMIT(lreg),
		    DF_CFGMAP_BASE_V4_GET_RE(breg) ? 'R' : '-',
		    DF_CFGMAP_BASE_V4_GET_WE(breg) ? 'W' : '-');
		df_print_dest(DF_CFGMAP_LIMIT_V4_GET_DEST_ID(lreg));
		mdb_printf("\n");
	}
}

static void
df_route_dram_genoa(uint64_t sock, uintptr_t inst, uint_t ndram)
{
	for (uint_t i = 0; i < ndram; i++) {
		uint32_t breg, lreg, ireg, creg;
		uint64_t base, limit;
		const char *chan;
		char ileave[16];

		const df_reg_def_t bdef = DF_DRAM_BASE_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read DRAM port base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_DRAM_LIMIT_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read DRAM port limit %u\n", i);
			continue;
		}

		const df_reg_def_t idef = DF_DRAM_ILV_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, idef.drd_func,
		    idef.drd_reg, &ireg)) {
			mdb_warn("failed to read DRAM port ilv %u\n", i);
			continue;
		}

		const df_reg_def_t cdef = DF_DRAM_CTL_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, cdef.drd_func,
		    cdef.drd_reg, &creg)) {
			mdb_warn("failed to read DRAM port ctl %u\n", i);
			continue;
		}

		base = DF_DRAM_BASE_V4_GET_ADDR(breg);
		base <<= DF_DRAM_BASE_V4_BASE_SHIFT;
		limit = DF_DRAM_LIMIT_V4_GET_ADDR(lreg);
		limit <<= DF_DRAM_LIMIT_V4_LIMIT_SHIFT;
		limit += DF_DRAM_LIMIT_V4_LIMIT_EXCL - 1;

		chan = df_chan_ileaves_genoa[DF_DRAM_ILV_V4_GET_CHAN(ireg)];
		(void) mdb_snprintf(ileave, sizeof (ileave), "%u/%s/%u/%u",
		    DF_DRAM_ILV_V4_GET_ADDR(ireg) + DF_DRAM_ILV_ADDR_BASE,
		    chan, DF_DRAM_ILV_V4_GET_DIE(ireg) + 1,
		    DF_DRAM_ILV_V4_GET_SOCK(ireg) + 1);

		mdb_printf("%-?#lx %-?#lx %c%c%c     %-15s ", base, limit,
		    DF_DRAM_CTL_V4_GET_VALID(creg) ? 'V' : '-',
		    DF_DRAM_CTL_V4_GET_HOLE_EN(creg) ? 'H' : '-',
		    '-', // XXX: no BreakBusLock in DF4?
		    ileave);
		df_print_dest(DF_DRAM_CTL_V4_GET_DEST_ID(creg));
		mdb_printf("\n");
	}
}

static void
df_route_ioports_genoa(uint64_t sock, uintptr_t inst)
{
	for (uint_t i = 0; i < DF_MAX_IO_RULES; i++) {
		uint32_t breg, lreg, base, limit;

		const df_reg_def_t bdef = DF_IO_BASE_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read I/O port base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_IO_LIMIT_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read I/O port limit %u\n", i);
			continue;
		}

		base = DF_IO_BASE_V4_GET_BASE(breg);
		base <<= DF_IO_BASE_SHIFT;
		limit = DF_IO_LIMIT_V4_GET_LIMIT(lreg);
		limit <<= DF_IO_LIMIT_SHIFT;
		limit += DF_IO_LIMIT_EXCL - 1;

		mdb_printf("%-8#x %-8#x %c%c%c      ", base, limit,
		    DF_IO_BASE_V4_GET_RE(breg) ? 'R' : '-',
		    DF_IO_BASE_V4_GET_WE(breg) ? 'W' : '-',
		    DF_IO_BASE_V4_GET_IE(breg) ? 'I' : '-');
		df_print_dest(DF_IO_LIMIT_V4_GET_DEST_ID(lreg));
		mdb_printf("\n");
	}
}

static void
df_route_mmio_genoa(uint64_t sock, uintptr_t inst)
{
	for (uint_t i = 0; i < DF_MAX_MMIO_RULES; i++) {
		uint32_t breg, lreg, creg, ereg;
		uint64_t base, limit;

		const df_reg_def_t bdef = DF_MMIO_BASE_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, bdef.drd_func,
		    bdef.drd_reg, &breg)) {
			mdb_warn("failed to read MMIO base %u\n", i);
			continue;
		}

		const df_reg_def_t ldef = DF_MMIO_LIMIT_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, ldef.drd_func,
		    ldef.drd_reg, &lreg)) {
			mdb_warn("failed to read MMIO limit %u\n", i);
			continue;
		}

		const df_reg_def_t cdef = DF_MMIO_CTL_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, cdef.drd_func,
		    cdef.drd_reg, &creg)) {
			mdb_warn("failed to read MMIO control %u\n", i);
			continue;
		}

		const df_reg_def_t edef = DF_MMIO_EXT_V4(i);
		if (!df_read32_indirect_raw_genoa(sock, inst, edef.drd_func,
		    edef.drd_reg, &ereg)) {
			mdb_warn("failed to read MMIO ext %u\n", i);
			continue;
		}

		base = (uint64_t)breg << DF_MMIO_SHIFT |
		    ((uint64_t)DF_MMIO_EXT_V4_GET_BASE(ereg)
		        << DF_MMIO_EXT_SHIFT);
		limit = (uint64_t)lreg << DF_MMIO_SHIFT |
		    ((uint64_t)DF_MMIO_EXT_V4_GET_LIMIT(ereg)
		        << DF_MMIO_EXT_SHIFT);
		limit += DF_MMIO_LIMIT_EXCL - 1;

		mdb_printf("%-?#lx %-?#lx %c%c%c%c     ", base, limit,
		    DF_MMIO_CTL_GET_RE(creg) ? 'R' : '-',
		    DF_MMIO_CTL_GET_WE(creg) ? 'W' : '-',
		    DF_MMIO_CTL_V4_GET_NP(creg) ? 'N' : '-',
		    DF_MMIO_CTL_GET_CPU_DIS(creg) ? 'C' : '-');
		df_print_dest(DF_MMIO_CTL_V4_GET_DEST_ID(creg));
		mdb_printf("\n");
	}
}

static const df_ops_t df_ops_genoa = {
	.dfo_supported_gens = DF_REV_4,
	.dfo_comp_names_count = ARRAY_SIZE(df_comp_names_genoa),
	.dfo_comp_names = df_comp_names_genoa,
	.dfo_chan_ileaves_count = ARRAY_SIZE(df_chan_ileaves_genoa),
	.dfo_chan_ileaves = df_chan_ileaves_genoa,
	.dfo_read32_indirect_raw = df_read32_indirect_raw_genoa,
	.dfo_write32_indirect_raw = df_write32_indirect_raw_genoa,
	.dfo_get_smn_busno = df_get_smn_busno_genoa,
	.dfo_fetch_masks = df_fetch_masks_genoa,
	/*
	 * For DRAM, default to CCM0 (we don't use a UMC because it has very few
	 * rules). For I/O ports, use CCM0 as well as the IOMS entries don't
	 * really have rules here. For MMIO and PCI buses, use IOM0_IOHUBM0.
	 */
	.dfo_dram_io_inst = 16,
	.dfo_mmio_pci_inst = 32,
	.dfo_route_buses = df_route_buses_genoa,
	.dfo_route_dram = df_route_dram_genoa,
	.dfo_route_ioports = df_route_ioports_genoa,
	.dfo_route_mmio = df_route_mmio_genoa,
};

static boolean_t
df_ops_init(void)
{
	df_ops = &df_ops_milan;
	df_ops = &df_ops_genoa;
	return (B_TRUE);
	/*
	x86_chiprev_t chiprev;

	if (df_ops != NULL)
		return (B_TRUE);

	// TODO: should this be comparing uarch instead?
	chiprev = cpuid_getchiprev(CPU);
	if (chiprev_matches(chiprev, X86_CHIPREV_AMD_MILAN_ANY))
		df_ops = &df_ops_milan;
	else if (chiprev_matches(chiprev, X86_CHIPREV_AMD_GENOA_ANY))
		df_ops = &df_ops_genoa;
	else
	{
		mdb_warn("unsupported chiprev\n");
		return (B_FALSE);
	}

	return (B_TRUE);
	*/
}

static const char *
df_comp_name(uint32_t compid)
{
	if (!df_ops_init())
		return (NULL);

	const df_comp_t *df_comp_names = df_ops->dfo_comp_names;
	for (uint_t i = 0; i < df_ops->dfo_comp_names_count; i++) {
		if (compid == df_comp_names[i].dc_comp) {
			return (df_comp_names[i].dc_name);
		}
	}

	return (NULL);
}

static uint_t
df_comp_ndram(uint32_t instid)
{
	if (!df_ops_init())
		return (0);

	const df_comp_t *df_comp_names = df_ops->dfo_comp_names;
	for (uint_t i = 0; i < df_ops->dfo_comp_names_count; i++) {
		if (instid == df_comp_names[i].dc_inst) {
			return (df_comp_names[i].dc_ndram);
		}
	}

	return (0);
}

static boolean_t
df_get_smn_busno(uint64_t sock, uint8_t *busno)
{
	if (!df_ops_init())
		return (B_FALSE);

	return (df_ops->dfo_get_smn_busno(sock, busno));
}

/*
 * Determine if MMIO configuration space is valid at this point. Once it is, we
 * store that fact and don't check again.
 */
static boolean_t
pcicfg_space_init(void)
{
	uint64_t msr;

	if (pcicfg_valid) {
		return (B_TRUE);
	}

	if (mdb_x86_rdmsr(MSR_AMD_MMIO_CFG_BASE_ADDR, &msr) != DCMD_OK) {
		mdb_warn("failed to read MSR_AMD_MMIOCFG_BASEADDR");
		return (B_FALSE);
	}

	if (AMD_MMIO_CFG_BASE_ADDR_GET_EN(msr) != 0) {
		pcicfg_physaddr = AMD_MMIO_CFG_BASE_ADDR_GET_ADDR(msr) <<
		    AMD_MMIO_CFG_BASE_ADDR_ADDR_SHIFT;
		pcicfg_valid = B_TRUE;
		return (B_TRUE);
	}

	mdb_warn("PCI config space is not currently enabled in the CPU\n");
	return (B_FALSE);
}

static boolean_t
pcicfg_validate(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg,
    uint8_t len)
{
	if (dev >= PCI_MAX_DEVICES) {
		mdb_warn("invalid pci device: %x\n", dev);
		return (B_FALSE);
	}

	/*
	 * We don't know whether the target uses ARI, but we need to accommodate
	 * the possibility that it does.  If it does not, we allow the
	 * possibility of an invalid function number with device 0.  Note that
	 * we also don't check the function number at all in that case because
	 * ARI allows function numbers up to 255 which is the entire range of
	 * the type we're using for func.  As this is supported only in kmdb, we
	 * really have no choice but to trust the user anyway.
	 */
	if (dev != 0 && func >= PCI_MAX_FUNCTIONS) {
		mdb_warn("invalid pci function: %x\n", func);
		return (B_FALSE);
	}

	if (reg >= PCIE_CONF_HDR_SIZE) {
		mdb_warn("invalid pci register: %x\n", reg);
		return (B_FALSE);
	}

	if (len != 1 && len != 2 && len != 4) {
		mdb_warn("invalid register length: %x\n", len);
		return (B_FALSE);
	}

	if (!IS_P2ALIGNED(reg, len)) {
		mdb_warn("register must be naturally aligned\n", reg);
		return (B_FALSE);
	}

	if (!pcicfg_space_init()) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

static uint64_t
pcicfg_mkaddr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg)
{
	return (pcicfg_physaddr + PCIE_CADDR_ECAM(bus, dev, func, reg));
}

static boolean_t
pcicfg_read(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint8_t len,
    uint32_t *val)
{
	ssize_t ret;
	uint64_t addr;

	if (!pcicfg_validate(bus, dev, func, reg, len)) {
		return (B_FALSE);
	}

	addr = pcicfg_mkaddr(bus, dev, func, reg);
	ret = mdb_pread(val, (size_t)len, addr);
	if (ret != len) {
		mdb_warn("failed to read %x/%x/%x reg 0x%x len %u",
		    bus, dev, func, reg, len);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
pcicfg_write(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg, uint8_t len,
    uint32_t val)
{
	ssize_t ret;
	uint64_t addr;

	if (!pcicfg_validate(bus, dev, func, reg, len)) {
		return (B_FALSE);
	}

	if ((val & ~(0xffffffffU >> ((4 - len) << 3))) != 0) {
		mdb_warn("value 0x%x does not fit in %u bytes\n", val, len);
		return (B_FALSE);
	}

	addr = pcicfg_mkaddr(bus, dev, func, reg);
	ret = mdb_pwrite(&val, (size_t)len, addr);
	if (ret != len) {
		mdb_warn("failed to write %x/%x/%x reg 0x%x len %u",
		    bus, dev, func, reg, len);
		return (B_FALSE);
	}

	return (B_TRUE);
}

typedef enum pcicfg_rw {
	PCICFG_RD,
	PCICFG_WR
} pcicfg_rw_t;

static int
pcicfg_rw(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv,
    pcicfg_rw_t rw)
{
	u_longlong_t parse_val;
	uint32_t val = 0;
	uintptr_t len = 4;
	uint_t next_arg;
	uintptr_t bus, dev, func, off;
	boolean_t res;

	if (!(flags & DCMD_ADDRSPEC))
		return (DCMD_USAGE);

	next_arg = mdb_getopts(argc, argv,
	    'L', MDB_OPT_UINTPTR, &len, NULL);

	if (argc - next_arg != (rw == PCICFG_RD ? 3 : 4)) {
		return (DCMD_USAGE);
	}

	bus = (uintptr_t)mdb_argtoull(&argv[next_arg++]);
	dev = (uintptr_t)mdb_argtoull(&argv[next_arg++]);
	func = (uintptr_t)mdb_argtoull(&argv[next_arg++]);
	if (rw == PCICFG_WR) {
		parse_val = mdb_argtoull(&argv[next_arg++]);
		if (parse_val > UINT32_MAX) {
			mdb_warn("write value must be a 32-bit quantity\n");
			return (DCMD_ERR);
		}
		val = (uint32_t)parse_val;
	}
	off = addr;

	if (bus > UINT8_MAX || dev > UINT8_MAX || func > UINT8_MAX ||
	    off > UINT16_MAX) {
		mdb_warn("b/d/f/r does not fit in 1/1/1/2 bytes\n");
		return (DCMD_ERR);
	}

	switch (rw) {
	case PCICFG_RD:
		res = pcicfg_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
		    (uint16_t)off, (uint8_t)len, &val);
		break;
	case PCICFG_WR:
		res = pcicfg_write((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
		    (uint16_t)off, (uint8_t)len, val);
		break;
	default:
		mdb_warn("internal error: unreachable PCI R/W type %d\n", rw);
		return (DCMD_ERR);
	}

	if (!res)
		return (DCMD_ERR);

	if (rw == PCICFG_RD) {
		mdb_printf("%llx\n", (u_longlong_t)val);
	}

	return (DCMD_OK);
}

int
rdpcicfg_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	return (pcicfg_rw(addr, flags, argc, argv, PCICFG_RD));
}

int
wrpcicfg_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	return (pcicfg_rw(addr, flags, argc, argv, PCICFG_WR));
}

static const char *dfhelp =
"%s a register %s the data fabric. The register is indicated by the address\n"
"of the dcmd. This can either be directed at a specific instance or be\n"
"broadcast to all instances. One of -b or -i inst is required. If no socket\n"
"(really the I/O die) is specified, then the first one will be selected. The\n"
"following options are supported:\n"
"\n"
"  -b		broadcast the I/O rather than direct it at a single function\n"
"  -f func	direct the I/O to the specified DF function\n"
"  -i inst	direct the I/O to the specified instance, otherwise use -b\n"
"  -s socket	direct the I/O to the specified I/O die, generally a socket\n";

void
rddf_dcmd_help(void)
{
	mdb_printf(dfhelp, "Read", "from");
}

void
wrdf_dcmd_help(void)
{
	mdb_printf(dfhelp, "Write", "to");
}

static int
df_dcmd_check(uintptr_t addr, uint_t flags, boolean_t inst_set, uintptr_t inst,
    boolean_t func_set, uintptr_t func, boolean_t sock_set, uintptr_t *sock,
    uint_t broadcast)
{
	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	} else if ((addr & ~0xffc) != 0) {
		mdb_warn("invalid register: 0x%x, must be 4-byte aligned\n",
		    addr);
		return (DCMD_ERR);
	}

	if (sock_set) {
		/*
		 * We don't really know how many I/O dies there are in advance;
		 * however, the theoretical max is 8 (2P Naples with 4 dies);
		 * however, on the Oxide architecture there'll only ever be 2.
		 */
		if (*sock > 1) {
			mdb_warn("invalid socket ID: %lu\n", *sock);
			return (DCMD_ERR);
		}
	} else {
		*sock = 0;
	}

	if (!func_set) {
		mdb_warn("-f is required\n");
		return (DCMD_ERR);
	} else if (func >= 8) {
		mdb_warn("only functions 0-7 are allowed: %lu\n", func);
		return (DCMD_ERR);
	}


	if ((!inst_set && !broadcast) ||
	    (inst_set && broadcast)) {
		mdb_warn("One of -i or -b must be set\n");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static boolean_t
df_read32(uint64_t sock, const df_reg_def_t df, uint32_t *valp)
{
	return (pcicfg_read(0, 0x18 + sock, df.drd_func, df.drd_reg,
	    sizeof (*valp), valp));
}

static boolean_t
df_write32(uint64_t sock, const df_reg_def_t df, uint32_t val)
{
	return (pcicfg_write(0, 0x18 + sock, df.drd_func, df.drd_reg,
	    sizeof (val), val));
}

static boolean_t
df_write32_indirect_raw(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t val)
{
	if (!df_ops_init())
		return (B_FALSE);

	return (df_ops->dfo_write32_indirect_raw(sock, inst, func, reg, val));
}

static boolean_t
df_read32_indirect_raw(uint64_t sock, uintptr_t inst, uintptr_t func,
    uint16_t reg, uint32_t *valp)
{
	if (!df_ops_init())
		return (B_FALSE);

	return (df_ops->dfo_read32_indirect_raw(sock, inst, func, reg, valp));
}

static boolean_t
df_read32_indirect(uint64_t sock, uintptr_t inst, const df_reg_def_t def,
    uint32_t *valp)
{
	if (!df_ops_init())
		return (B_FALSE);

	if ((def.drd_gens & df_ops->dfo_supported_gens) == 0) {
		mdb_warn("asked to read DF reg with unsupported Gen: "
		    "func/reg: %u/0x%x, gens: 0x%x, supported_gens: 0x%\n",
		    def.drd_func, def.drd_reg, def.drd_gens,
		    df_ops->dfo_supported_gens);
		return (B_FALSE);
	}

	return (df_ops->dfo_read32_indirect_raw(sock, inst, def.drd_func,
	    def.drd_reg, valp));
}

int
rddf_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t broadcast = FALSE;
	boolean_t inst_set = FALSE, func_set = FALSE, sock_set = FALSE;
	uintptr_t inst, func, sock;
	uint32_t val;
	int ret;

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &broadcast,
	    'f', MDB_OPT_UINTPTR_SET, &func_set, &func,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst,
	    's', MDB_OPT_UINTPTR_SET, &sock_set, &sock,
	    NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((ret = df_dcmd_check(addr, flags, inst_set, inst, func_set, func,
	    sock_set, &sock, broadcast)) != DCMD_OK) {
		return (ret);
	}

	/*
	 * For a broadcast read, read directly. Otherwise we need to use the
	 * FICAA register.
	 */
	if (broadcast) {
		if (!pcicfg_read(0, 0x18 + sock, func, addr, sizeof (val),
		    &val)) {
			return (DCMD_ERR);
		}
	} else {
		if (!df_read32_indirect_raw(sock, inst, func, addr, &val)) {
			return (DCMD_ERR);
		}
	}

	mdb_printf("%x\n", val);
	return (DCMD_OK);
}

int
wrdf_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t broadcast = FALSE;
	boolean_t inst_set = FALSE, func_set = FALSE, sock_set = FALSE;
	uintptr_t inst, func, sock;
	u_longlong_t parse_val;
	uint32_t val;
	int ret;

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &broadcast,
	    'f', MDB_OPT_UINTPTR_SET, &func_set, &func,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst,
	    's', MDB_OPT_UINTPTR_SET, &sock_set, &sock,
	    NULL) != argc - 1) {
		mdb_warn("missing required value to write\n");
		return (DCMD_USAGE);
	}

	parse_val = mdb_argtoull(&argv[argc - 1]);
	if (parse_val > UINT32_MAX) {
		mdb_warn("write value must be a 32-bit quantity\n");
		return (DCMD_ERR);
	}
	val = (uint32_t)parse_val;


	if ((ret = df_dcmd_check(addr, flags, inst_set, inst, func_set, func,
	    sock_set, &sock, broadcast)) != DCMD_OK) {
		return (ret);
	}

	if (broadcast) {
		if (!pcicfg_write(0, 0x18 + sock, func, addr, sizeof (val),
		    val)) {
			return (DCMD_ERR);
		}
	} else {
		if (!df_write32_indirect_raw(sock, inst, func, addr, val)) {
			return (DCMD_ERR);
		}
	}

	return (DCMD_OK);
}

static const char *smnhelp =
"%s a register %s the system management network (SMN). The address of the\n"
"dcmd is used to indicate the register to target. If no socket (really the\n"
"I/O die) is specified, then the first one will be selected. The NBIO\n"
"instance to use is determined based on what the DF indicates. The following\n"
"options are supported:\n"
"\n"
"  -L len	use access size {1,2,4} bytes, default 4\n"
"  -s socket	direct the I/O to the specified I/O die, generally a socket\n";

void
rdsmn_dcmd_help(void)
{
	mdb_printf(smnhelp, "Read", "from");
}

void
wrsmn_dcmd_help(void)
{
	mdb_printf(smnhelp, "Write", "to");
}

typedef enum smn_rw {
	SMN_RD,
	SMN_WR
} smn_rw_t;

static int
smn_rw_regdef(const smn_reg_t reg, uint64_t sock, smn_rw_t rw,
    uint32_t *smn_val)
{
	uint8_t smn_busno;
	boolean_t res;
	size_t len = SMN_REG_SIZE(reg);
	uint32_t addr = SMN_REG_ADDR(reg);

	if (!SMN_REG_SIZE_IS_VALID(reg)) {
		mdb_warn("invalid read length %lu (allowed: {1,2,4})\n", len);
		return (DCMD_ERR);
	}

	if (!SMN_REG_IS_NATURALLY_ALIGNED(reg)) {
		mdb_warn("address %x is not aligned on a %lu-byte boundary\n",
		    addr, len);
		return (DCMD_ERR);
	}

	if (rw == SMN_WR && !SMN_REG_VALUE_FITS(reg, *smn_val)) {
		mdb_warn("write value %lx does not fit in size %lu\n", *smn_val,
		    len);
		return (DCMD_ERR);
	}

	const uint32_t base_addr = SMN_REG_ADDR_BASE(reg);
	const uint32_t addr_off = SMN_REG_ADDR_OFF(reg);

	if (!df_get_smn_busno(sock, &smn_busno)) {
		mdb_warn("failed to get SMN bus number\n");
		return (DCMD_ERR);
	}

	if (!pcicfg_write(smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_ADDR, sizeof (base_addr),
	    base_addr)) {
		mdb_warn("failed to write to IOHC SMN address register\n");
		return (DCMD_ERR);
	}

	switch (rw) {
	case SMN_RD:
		res = pcicfg_read(smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA + addr_off,
		    SMN_REG_SIZE(reg), smn_val);
		break;
	case SMN_WR:
		res = pcicfg_write(smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA + addr_off,
		    SMN_REG_SIZE(reg), *smn_val);
		break;
	default:
		mdb_warn("internal error: unreachable SMN R/W type %d\n", rw);
		return (DCMD_ERR);
	}

	if (!res) {
		mdb_warn("failed to read from IOHC SMN data register\n");
		return (DCMD_ERR);
	}

	return (DCMD_OK);

}

static int
smn_rw(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv,
    smn_rw_t rw)
{
	size_t len = 4;
	u_longlong_t parse_val;
	uint32_t smn_val = 0;
	uint64_t sock = 0;
	int ret;

	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	}

	if (mdb_getopts(argc, argv, 'L', MDB_OPT_UINTPTR, (uintptr_t *)&len,
	    's', MDB_OPT_UINT64, &sock, NULL) !=
	    ((rw == SMN_RD) ? argc : (argc - 1))) {
		return (DCMD_USAGE);
	}

	if (rw == SMN_WR) {
		parse_val = mdb_argtoull(&argv[argc - 1]);
		if (parse_val > UINT32_MAX) {
			mdb_warn("write value must be a 32-bit quantity\n");
			return (DCMD_ERR);
		}
		smn_val = (uint32_t)parse_val;
	}

	if (sock > 1) {
		mdb_warn("invalid socket ID: %lu", sock);
		return (DCMD_ERR);
	}

	if (addr > UINT32_MAX) {
		mdb_warn("address %lx is out of range [0, 0xffffffff]\n", addr);
		return (DCMD_ERR);
	}

	const smn_reg_t reg = SMN_MAKE_REG_SIZED(addr, len);

	ret = smn_rw_regdef(reg, sock, rw, &smn_val);
	if (ret != DCMD_OK) {
		return (ret);
	}

	if (rw == SMN_RD) {
		mdb_printf("%x\n", smn_val);
	}

	return (DCMD_OK);
}

static int
rdmsn_regdef(const smn_reg_t reg, uint8_t sock, uint32_t *val)
{
	return (smn_rw_regdef(reg, sock, SMN_RD, val));
}

int
rdsmn_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	return (smn_rw(addr, flags, argc, argv, SMN_RD));
}

int
wrsmn_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	return (smn_rw(addr, flags, argc, argv, SMN_WR));
}

static boolean_t
df_fetch_masks(void)
{
	if (!df_ops_init() || !df_ops->dfo_fetch_masks())
		return (B_FALSE);

	df_masks_valid = B_TRUE;
	return (B_TRUE);
}

/*
 * Given a data fabric fabric ID (critically not an instance ID), print
 * information about that.
 */
static void
df_print_dest(uint32_t dest)
{
	uint32_t node, comp;
	const char *name;

	if (!df_masks_valid) {
		if (!df_fetch_masks()) {
			mdb_printf("%x", dest);
			return;
		}
	}

	node = (dest & df_node_mask) >> df_node_shift;
	comp = dest & df_comp_mask;
	name = df_comp_name(comp);

	mdb_printf("%#x (%#x/%#x)", dest, node, comp);
	if (name != NULL) {
		mdb_printf(" -- %s", name);
	}
}

static const char *df_route_help =
"Print out routing rules in the data fabric. This currently supports reading\n"
"the PCI bus, I/O port, MMIO, and DRAM routing rules. These values can vary,\n"
"especially with DRAM, from instance to instance. All route entries of a\n"
"given type are printed. Where possible, we will select a default instance to\n"
"use for this. The following options are used to specify the type of routing\n"
"entries to print:\n"
"  -b           print PCI bus routing entries\n"
"  -d           print DRAM routing entries\n"
"  -I           print I/O port entries\n"
"  -m           print MMIO routing entries\n"
"\n"
"The following options are used to control which instance to print from\n"
"  -i inst	print entries from the specified instance\n"
"  -s socket	print entries from the specified I/O die, generally a socket\n"
"\n"
"The following letters are used in the rather terse FLAGS output:\n"
"\n"
"    R		Read Enabled (PCI Bus, I/O Ports, MMIO)\n"
"    W		Write Enabled (PCI Bus, I/O Ports, MMIO)\n"
"    I		ISA Shenanigans (I/O ports)\n"
"    N		Non-posted mode (MMIO)\n"
"    C		CPU redirected to compat addresses (MMIO)\n"
"    B		Break Bus lock (DRAM)\n"
"    H		MMIO Hole Enabled (DRAM)\n"
"    V		Rule Valid (DRAM)\n";

void
df_route_dcmd_help(void)
{
	mdb_printf(df_route_help);
}

static int
df_route_buses(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (!df_ops_init())
		return (DCMD_ERR);

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-7s %-7s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINATION");
	}

	df_ops->dfo_route_buses(sock, inst);

	return (DCMD_OK);
}

static int
df_route_dram(uint_t flags, uint64_t sock, uintptr_t inst)
{
	uint_t ndram;

	if (!df_ops_init())
		return (DCMD_ERR);

	if ((ndram = df_comp_ndram(inst)) == 0) {
		mdb_warn("component 0x%x has no DRAM rules\n", inst);
		return (DCMD_ERR);
	}

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-?s %-?s %-7s %-15s %s\n", "BASE", "LIMIT",
		    "FLAGS", "INTERLEAVE", "DESTINATION");
	}

	df_ops->dfo_route_dram(sock, inst, ndram);

	return (DCMD_OK);
}

static int
df_route_ioports(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (!df_ops_init())
		return (DCMD_ERR);

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-8s %-8s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINATION");
	}

	df_ops->dfo_route_ioports(sock, inst);

	return (DCMD_OK);
}

static int
df_route_mmio(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (!df_ops_init())
		return (DCMD_ERR);

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-?s %-?s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINATION");
	}

	df_ops->dfo_route_mmio(sock, inst);

	return (DCMD_OK);
}

int
df_route_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint64_t sock = 0;
	uintptr_t inst;
	boolean_t inst_set = B_FALSE;
	uint_t opt_b = FALSE, opt_d = FALSE, opt_I = FALSE, opt_m = FALSE;
	uint_t count = 0;

	if (!df_ops_init())
		return (DCMD_ERR);

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &opt_b,
	    'd', MDB_OPT_SETBITS, TRUE, &opt_d,
	    'I', MDB_OPT_SETBITS, TRUE, &opt_I,
	    'm', MDB_OPT_SETBITS, TRUE, &opt_m,
	    's', MDB_OPT_UINT64, &sock,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst, NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((flags & DCMD_ADDRSPEC) != 0) {
		mdb_warn("df_route does not support addresses\n");
		return (DCMD_USAGE);
	}

	if (opt_b) {
		count++;
	}
	if (opt_d)
		count++;
	if (opt_I)
		count++;
	if (opt_m)
		count++;

	if (count == 0) {
		mdb_warn("one of -b, -d, -I, and -m must be specified\n");
		return (DCMD_ERR);
	} else if (count > 1) {
		mdb_warn("only one of -b -d, -I, and -m may be specified\n");
		return (DCMD_ERR);
	}

	if (sock > 1) {
		mdb_warn("invalid socket ID: %lu\n", sock);
		return (DCMD_ERR);
	}

	/*
	 * For DRAM, default to CCM0 (we don't use a UMC because it has very few
	 * rules). For I/O ports, use CCM0 as well as the IOMS entries don't
	 * really have rules here. For MMIO and PCI buses, use IOMS0.
	 */
	if (!inst_set) {
		if (opt_d || opt_I) {
			inst = df_ops->dfo_dram_io_inst;
		} else {
			inst = df_ops->dfo_mmio_pci_inst;
		}
	}

	if (opt_d) {
		return (df_route_dram(flags, sock, inst));
	} else if (opt_b) {
		return (df_route_buses(flags, sock, inst));
	} else if (opt_I) {
		return (df_route_ioports(flags, sock, inst));
	} else {
		return (df_route_mmio(flags, sock, inst));
	}

	return (DCMD_OK);
}

static const char *dimmhelp =
"Print a summary of DRAM training for each channel on the SoC. This uses the\n"
"UMC::CH::UmcConfig Ready bit to determine whether or not the channel\n"
"trained. Separately, there is a column indicating whether there is a DIMM\n"
"installed in each location in the channel. A 1 DPC system will always show\n"
"DIMM 1 missing. The following columns will be output:\n"
"\n"
"CHAN:\t\tIndicates the socket and board channel letter\n"
"UMC:\t\tIndicates the UMC instance\n"
"TRAIN:\tIndicates whether or not training completed successfully\n"
"DIMM 0:\tIndicates whether DIMM 0 in the channel is present\n"
"DIMM 1:\tIndicates whether DIMM 0 in the channel is present\n";


void
dimm_report_dcmd_help(void)
{
	mdb_printf(dimmhelp);
}

/*
 * Check both the primary and secondary base address values to see if an enable
 * flags is present. DIMM 0 uses chip selects 0/1 and DIMM 1 uses chip selects
 * 2/3.
 */
static int
dimm_report_dimm_present(uint8_t sock, uint8_t umcno, uint8_t dimm,
    boolean_t *pres)
{
	int ret;
	uint32_t base0, base1, sec0, sec1;
	uint8_t cs0 = dimm * 2;
	uint8_t cs1 = dimm * 2 + 1;
	smn_reg_t base0_reg = UMC_BASE(umcno, cs0);
	smn_reg_t base1_reg = UMC_BASE(umcno, cs1);
	smn_reg_t sec0_reg = UMC_BASE_SEC(umcno, cs0);
	smn_reg_t sec1_reg = UMC_BASE_SEC(umcno, cs1);

	if ((ret = rdmsn_regdef(base0_reg, sock, &base0)) != DCMD_OK ||
	    (ret = rdmsn_regdef(base1_reg, sock, &base1)) != DCMD_OK ||
	    (ret = rdmsn_regdef(sec0_reg, sock, &sec0)) != DCMD_OK ||
	    (ret = rdmsn_regdef(sec1_reg, sock, &sec1)) != DCMD_OK) {
		return (ret);
	}

	*pres = UMC_BASE_GET_EN(base0) != 0 || UMC_BASE_GET_EN(base1) != 0 ||
	    UMC_BASE_GET_EN(sec0) != 0 || UMC_BASE_GET_EN(sec1) != 0;
	return (DCMD_OK);
}

/*
 * Output in board order, not UMC order (hence umc_order[] below) a summary of
 * training information for each DRAM channel.
 */
static int
dimm_report_dcmd_sock(uint8_t sock)
{
	const uint8_t umc_order[8] = { 0, 1, 3, 2, 6, 7, 5, 4 };

	for (size_t i = 0; i < ARRAY_SIZE(umc_order); i++) {
		const uint8_t umcno = umc_order[i];
		const char *brdchan = milan_chan_map[umcno];
		int ret;
		boolean_t train, dimm0, dimm1;

		smn_reg_t umccfg_reg = UMC_UMCCFG(umcno);
		uint32_t umccfg;

		ret = rdmsn_regdef(umccfg_reg, sock, &umccfg);
		if (ret != DCMD_OK) {
			return (ret);
		}
		train = UMC_UMCCFG_GET_READY(umccfg);

		ret = dimm_report_dimm_present(sock, umcno, 0, &dimm0);
		if (ret != DCMD_OK) {
			mdb_warn("failed to read UMC %u DIMM 0 presence\n",
			    umcno);
			return (DCMD_ERR);
		}

		ret = dimm_report_dimm_present(sock, umcno, 1, &dimm1);
		if (ret != DCMD_OK) {
			mdb_warn("failed to read UMC %u DIMM 1 presence\n",
			    umcno);
			return (DCMD_ERR);
		}

		mdb_printf("%u/%s\t%u\t%s\t%s\t%s\n", sock, brdchan, umcno,
		    train ? "yes" : "no", dimm0 ? "present" : "missing",
		    dimm1 ? "present" : "missing");
	}

	return (DCMD_OK);
}

/*
 * Report DIMM presence and DRAM channel readiness, which is a proxy for
 * training having completed.
 */
int
dimm_report_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	int ret;
	uint32_t val;

	if ((flags & DCMD_ADDRSPEC) != 0) {
		mdb_warn("::dimm_report does not support addresses\n");
		return (DCMD_USAGE);
	}

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("CHAN\tUMC\tTRAIN\tDIMM 0\tDIMM 1\n");
	}

	ret = dimm_report_dcmd_sock(0);
	if (ret != DCMD_OK) {
		return (ret);
	}

	/*
	 * Attempt to read a DF entry to see if the other socket is present as a
	 * proxy.
	 */
	if (!df_read32(1, DF_FBIINFO0, &val)) {
		mdb_warn("failed to read DF config address\n");
		return (DCMD_ERR);
	}

	if (val != PCI_EINVAL32) {
		ret = dimm_report_dcmd_sock(1);

	}

	return (ret);
}
