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
 * A nexus driver for the IOMS units found in AMD Zen-family SoCs, which host
 * the system's PCIe root complexes.  At present each instance is only a
 * placeholder identifying its unit in the I/O fabric: it creates no children
 * and owns no resources.  See "Current Scope and Direction" below.
 *
 * --------------------------------------
 * Physical Organization and Nomenclature
 * --------------------------------------
 *
 * The I/O die at the center of each Zen SoC connects its compute complexes to
 * the outside world via a number of I/O paths hanging off the data fabric
 * (DF).  The units of interest here are the DF endpoints that route MMIO and
 * legacy I/O outward and host PCIe: on Milan, a single combined DF component
 * that the PPR calls the "IO Master/Slave (IOMS)"; on Genoa and Turin, a pair
 * of components, the IOM (master) and IOS (slave), that always occur and
 * operate together.  Since the split pair maps 1:1 onto the older combined
 * component, our fabric code represents both designs as a single unified IOMS
 * unit (zen_ioms_t) and this driver follows that abstraction; see the
 * discussion in uts/oxide/turin/turin_fabric.c.
 *
 * On the northbridge side, each IOMS connects to an IOHUB, within which sits
 * an IOHC ("IOHUB Core"), which the PPRs describe as the I/O crossbar: it
 * routes between the DF on one side and the PCIe cores, IOMMU, IOAPIC, nBIFs,
 * and (on one instance per socket) the FCH on the other.  The IOHC implements
 * the host side of the PCIe root complex: it appears in PCI configuration
 * space as device 0 function 0 on the root bus (what the PPRs call NBCONFIG)
 * and is the attachment point for npe(4D).  The IOHUBs are in turn grouped
 * into NBIO units.  IOMS, IOHUB, IOHC, and PCI root bus all correspond 1:1 on
 * every supported microarchitecture; only the counts and groupings differ:
 *
 *		NBIO/die	IOHUB/NBIO	IOMS (== IOHC == root bus)/die
 * Milan	4		1		4
 * Genoa	2		2		4
 * Turin	2		4		8
 *
 * Turin additionally has two flavors of IOHC, which its PPR calls IOHC0
 * (larger, with an L2IOMMU) and IOHC1 (smaller); four of each per I/O die.
 * There is one instance of this driver for every IOMS in the system,
 * including those hosting the smaller IOHCs and regardless of whether any
 * PCIe ports or devices exist below the corresponding root complex.
 *
 * ---------------------
 * Naming and Addressing
 * ---------------------
 *
 * The devinfo node (and driver) name follows the kernel's unified IOMS
 * abstraction rather than any single AMD document term: it matches
 * zen_ioms_t, zen_walk_ioms(), and mdb's ::ioms, which is what one wants when
 * correlating a device node with fabric state.  We deliberately do not use
 * the amdzen_ prefix: that denotes membership in the amdzen(4D) pseudo-nexus
 * ecosystem used on platforms where the OS does not own the fabric, of which
 * this driver is specifically not a part.
 *
 * Each node is a child of the df(4D) nexus representing its I/O die's data
 * fabric instance, and its unit address is the die-relative IOMS index in
 * hex: df@0's child ioms@3 is socket 0's IOMS 3, and df@1's child ioms@3 is
 * its socket 1 twin.  The unit address is communicated via the
 * "unit-address" string property, which our parent's child naming honors
 * directly; unlike fch(4D) we carry no fabricated legacy "reg" property.
 * The AMD-visible identity of the underlying hardware is exposed as
 * properties rather than being encoded in the name, not least because IOHC
 * instance numbering is not monotonic in bus order on Turin:
 *
 *	ioms		die-relative IOMS index (matches mdb's ::ioms)
 *	nbio		die-relative NBIO index
 *	iohub		NBIO-relative IOHUB index
 *	iohc		IOHC instance number, per the PPR
 *	iohc-type	"large" or "small" (the PPR's IOHC0/IOHC1)
 *	node-id		DF node ID of the containing I/O die
 *	pci-bus		bus number of the hosted PCIe root complex's root bus
 *	fabric-id	DF fabric ID of this IOMS (of the IOS on Genoa/Turin)
 *
 * Together nbio, iohub, and iohc identify the PPR register instance for the
 * unit (e.g. instIOHC0_iohub0_nbio0 on Turin).
 */

#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/ddi_intr_impl.h>
#include <sys/ddifm_impl.h>
#include <sys/debug.h>
#include <sys/modctl.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/types.h>
#include <sys/io/zen/fabric.h>
#include <sys/io/zen/fabric_limits.h>
#include <sys/io/zen/ioms.h>

/*
 * Per-instance state: the device node and the fabric IOMS it represents.
 */
typedef struct ioms {
	dev_info_t	*io_dip;
	zen_ioms_t	*io_ioms;
} ioms_t;

static void *ioms_state;

/*
 * Used to find the zen_ioms_t corresponding to a device node's identity
 * properties when attaching.
 */
typedef struct ioms_match {
	uint32_t	im_nodeid;
	uint32_t	im_num;
	zen_ioms_t	*im_ioms;
} ioms_match_t;

static int
ioms_match_cb(zen_ioms_t *ioms, void *arg)
{
	ioms_match_t *im = arg;

	if (zen_iodie_node_id(zen_ioms_iodie(ioms)) == im->im_nodeid &&
	    zen_ioms_num(ioms) == im->im_num) {
		im->im_ioms = ioms;
		return (1);
	}

	return (0);
}

static int
ioms_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	ioms_t *iop;
	ioms_match_t im = { 0 };
	int inst, nodeid, num;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	/*
	 * Recover the fabric IOMS this node was created from.  The identity
	 * properties were set by our parent's bus_config when it created the
	 * node and are stable across the life of the node.
	 */
	nodeid = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    IOMS_PROP_NODE_ID, -1);
	num = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    IOMS_PROP_IOMS, -1);
	if (nodeid < 0 || num < 0) {
		dev_err(dip, CE_WARN, "missing '%s' or '%s' property",
		    IOMS_PROP_NODE_ID, IOMS_PROP_IOMS);
		return (DDI_FAILURE);
	}

	im.im_nodeid = (uint32_t)nodeid;
	im.im_num = (uint32_t)num;
	(void) zen_walk_ioms(ioms_match_cb, &im);
	if (im.im_ioms == NULL) {
		dev_err(dip, CE_WARN, "no fabric IOMS matches node ID %d "
		    "IOMS %d", nodeid, num);
		return (DDI_FAILURE);
	}

	inst = ddi_get_instance(dip);
	VERIFY0(ddi_soft_state_zalloc(ioms_state, inst));
	iop = ddi_get_soft_state(ioms_state, inst);
	iop->io_dip = dip;
	iop->io_ioms = im.im_ioms;

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
ioms_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	ioms_t *iop;
	int inst;

	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	inst = ddi_get_instance(dip);
	iop = ddi_get_soft_state(ioms_state, inst);
	if (iop == NULL || iop->io_dip != dip)
		return (DDI_FAILURE);

	ddi_soft_state_free(ioms_state, inst);

	return (DDI_SUCCESS);
}

static int
ioms_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop,
    void *arg, void *result)
{
	switch (ctlop) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == NULL)
			return (DDI_FAILURE);
		cmn_err(CE_CONT, "?IOMS device: %s@%s, %s%d\n",
		    ddi_node_name(rdip), ddi_get_name_addr(rdip),
		    ddi_driver_name(rdip), ddi_get_instance(rdip));
		break;
	case DDI_CTLOPS_INITCHILD:
		/*
		 * We do not enumerate any children yet.
		 */
		return (DDI_FAILURE);
	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}

	return (DDI_SUCCESS);
}

static int
ioms_fm_init(dev_info_t *dip, dev_info_t *tdip, int cap,
    ddi_iblock_cookie_t *ibc)
{
	return (i_ndi_busop_fm_init(dip, cap, ibc));
}

static struct bus_ops ioms_bus_ops = {
	.busops_rev = BUSO_REV,
	.bus_map = nullbusmap,
	.bus_dma_allochdl = ddi_dma_allochdl,
	.bus_dma_freehdl = ddi_dma_freehdl,
	.bus_dma_bindhdl = ddi_dma_bindhdl,
	.bus_dma_unbindhdl = ddi_dma_unbindhdl,
	.bus_dma_flush = ddi_dma_flush,
	.bus_dma_win = ddi_dma_win,
	.bus_dma_ctl = ddi_dma_mctl,
	.bus_prop_op = ddi_bus_prop_op,
	.bus_ctl = ioms_bus_ctl,
	.bus_fm_init = ioms_fm_init,
	.bus_intr_op = i_ddi_intr_ops
};

static struct dev_ops ioms_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_getinfo = nodev,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = ioms_attach,
	.devo_detach = ioms_detach,
	.devo_reset = nodev,
	.devo_quiesce = ddi_quiesce_not_needed,
	.devo_bus_ops = &ioms_bus_ops
};

static struct modldrv ioms_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "AMD Zen IOMS Nexus Driver",
	.drv_dev_ops = &ioms_dev_ops
};

static struct modlinkage ioms_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &ioms_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	ret = ddi_soft_state_init(&ioms_state, sizeof (ioms_t),
	    ZEN_IODIE_MAX_IOMS);
	if (ret != 0) {
		return (ret);
	}

	if ((ret = mod_install(&ioms_modlinkage)) != 0) {
		ddi_soft_state_fini(&ioms_state);
		return (ret);
	}

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ioms_modlinkage, modinfop));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&ioms_modlinkage)) != 0)
		return (ret);

	ddi_soft_state_fini(&ioms_state);
	return (0);
}
