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
 * the system's PCIe root complexes and, on one instance per socket, the FCH.
 * Each instance identifies its unit in the I/O fabric, owns the generic
 * (non-PCI) address space the fabric routes to that unit, and enumerates the
 * children that decode it: its PCIe root complex, and the fch(4D) nexus on the
 * FCH-bearing instance.
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
 * its socket 1 twin.  The unit address is communicated via the "unit-address"
 * string property, which our parent's child naming honors directly.
 *
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
 *
 * -------------------------
 * Address Space and the FCH
 * -------------------------
 *
 * The DF routes each region of the physical address space to a single IOMS,
 * and the fabric code partitions each IOMS's share into a pool for its PCIe
 * root complex and a generic (non-PCI) pool for everything else.  This driver
 * owns the generic pool for its unit, and resources flow strictly parent to
 * child from here: the IOMS grants a child a window of that space via a
 * "ranges" property on the child's node, and the child sub-allocates its
 * grant to its own children without ever reaching back into the fabric.
 *
 * Only the FCH is granted such a window, our other child, the root complex,
 * draws on the fabric's PCI pool by an entirely different route (see below).
 * The FCH is present on the one IOMS per socket whose IOHC it is attached to
 * (ZEN_IOMS_F_HAS_FCH; IOMS 3 on Milan, 4 on Turin -- always fabric-derived,
 * never assumed).  What the FCH can decode depends on its fabric role, which
 * we communicate to fch(4D) via the "fabric-role" property:
 *
 *  - The primary FCH (on the primary I/O die) subtractively decodes the whole
 *    legacy/compatibility space; it is granted everything in the generic
 *    pool.  It also has a small bank of real registers of its own -- the
 *    legacy PC interrupt-routing crossbar at [0xc00, 0xc01] in I/O port space
 *    -- which we expose as a "reg" property so fch(4D) can map it with
 *    ddi_regs_map_setup(9F).  Because we create all of our children, the
 *    format of their properties is ours to choose: "reg" uses the same
 *    fch_rangespec_t format as "ranges" (see sys/io/fch/ranges.h) rather
 *    than the legacy 3-cell struct regspec, and our INITCHILD, bus_map, and
 *    REGSIZE/NREGS resolve everything from the properties directly -- no
 *    sunbus parent-private data is ever built for our children.  Unlike the
 *    legacy format, the rangespec can describe 64-bit regions.
 *
 *  - A secondary FCH (other sockets, if present) decodes only a single 8KiB
 *    relocatable MMIO window selected by its FCH::PM::ALTMMIO{BASE,EN} BAR.
 *    Programming that BAR is a routing decision -- structurally the same as
 *    a bridge programming its decode window -- so it happens here, in the
 *    parent: we carve a suitably aligned window out of the generic pool,
 *    program and enable the BAR over SMN, and grant the FCH exactly that
 *    window.  From fch(4D)'s perspective, primary and secondary attachments
 *    are then uniform: the two differ only in the ranges received.  A
 *    secondary FCH has no "reg": nothing else of it is reachable.
 *
 * The transfer of the generic pool out of the fabric's available lists is
 * destructive and can happen only once, so the fabric records the grant
 * persistently and hands the same grant back on any subsequent call
 * (zen_fabric_gen_grant()); on top of that, we simply refuse to detach an
 * instance that holds it.  The remainder of a secondary FCH's pool beyond
 * the carved window is currently unused: there are no other consumers of
 * generic space on such an IOMS, and it remains accounted as used in the
 * fabric should that ever change.
 *
 * The FCH is a singleton with no address on any bus so its node has an
 * explicitly empty "unit-address", the property from which our INITCHILD names
 * every child: the node is just e.g. "huashan", identified positionally by its
 * df@/ioms@ ancestry.
 *
 * ---------------------
 * The PCIe Root Complex
 * ---------------------
 *
 * Our other child is the root bus of the root complex the IOHC implements,
 * created for every instance since every IOMS has one.  We create and name the
 * node ("pci"), addressed by the fabric's bus number for this unit, and
 * misc/pci_boot does everything else: the properties that make it a PCI root
 * bus, the walk of the buses beneath it, and the assignment and programming of
 * the resources it finds, which come from the fabric's PCI pool via
 * pci_prd_find_resource() rather than from the generic pool we hold.  See
 * pci_boot_rc_config() in <sys/pci_boot.h>.
 */

#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/cpuvar.h>
#include <sys/ddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/ddi_implfuncs.h>
#include <sys/ddi_intr_impl.h>
#include <sys/ddifm_impl.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/memlist.h>
#include <sys/modctl.h>
#include <sys/pci_boot.h>
#include <sys/stdbool.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/x86_archext.h>
#include <sys/amdzen/fch.h>
#include <sys/io/fch/ixbar.h>
#include <sys/io/fch/pmio.h>
#include <sys/io/fch/props.h>
#include <sys/io/fch/ranges.h>
#include <sys/io/zen/fch.h>
#include <sys/io/zen/fabric.h>
#include <sys/io/zen/fabric_limits.h>
#include <sys/io/zen/ioms.h>
#include <sys/io/zen/smn.h>

/*
 * Per-instance state: the device node, the fabric IOMS it represents, and (on
 * the FCH-bearing instance) the windows of generic address space granted to the
 * fch child, in the format of its "ranges" property.
 */
typedef struct ioms {
	dev_info_t	*io_dip;
	zen_ioms_t	*io_ioms;
	bool		io_fch_primary;
	uint_t		io_fch_nranges;
	fch_rangespec_t	*io_fch_ranges;
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

/*
 * Add the contents of memlist ml to the set of ranges frp, assuming address
 * space as.  The return value is the number of ranges used, which may be
 * smaller than the number of memlist entries: adjacent spans are coalesced
 * into a single range and empty spans are discarded.  The memlist itself is
 * owned by the fabric and left intact.
 */
static uint_t
ioms_memlist_to_ranges(const struct memlist *ml, fch_rangespec_t *frp,
    fch_addrsp_t as)
{
	uint_t ridx = 0;

	while (ml != NULL) {
		uint64_t size, end;

		if (ml->ml_size == 0) {
			ml = ml->ml_next;
			continue;
		}

		/* Overflowing 64-bit space is always a bug. */
		VERIFY3U(ml->ml_address + (ml->ml_size - 1), >=,
		    ml->ml_address);

		size = ml->ml_size;
		end = ml->ml_address + (ml->ml_size - 1);

		frp[ridx].fr_physlo = (uint32_t)ml->ml_address;
		frp[ridx].fr_physhi = (uint32_t)(ml->ml_address >> 32);

		/* Check for contiguous spans and coalesce. */
		while ((ml = ml->ml_next) != NULL &&
		    ml->ml_address == end + 1) {
			VERIFY3U(size, <, size + ml->ml_size);
			VERIFY3U(end, <, end + ml->ml_size);

			size += ml->ml_size;
			end += ml->ml_size;
		}

		/* Close out and count this range. */
		frp[ridx].fr_sizelo = (uint32_t)size;
		frp[ridx].fr_sizehi = (uint32_t)(size >> 32);
		frp[ridx].fr_addrsp = as;
		ridx++;
	}

	return (ridx);
}

/*
 * If this IOMS has the FCH attached, take ownership of the generic address
 * space the fabric routes to us and derive the windows to be granted to the
 * fch child, as described in the theory statement.  Failure here means the
 * FCH goes unrepresented, which is reported but does not fail our own attach.
 *
 * Everything here is idempotent across a repeated or retried attach: the
 * fabric returns the recorded grant, the window carved for a secondary FCH is
 * a deterministic function of it, and reprogramming the BAR with the same
 * value is harmless.
 */
static void
ioms_fch_init(ioms_t *iop)
{
	zen_ioms_t *ioms = iop->io_ioms;
	zen_iodie_t *iodie = zen_ioms_iodie(ioms);
	const smn_reg_t enreg = fch_pmio_smn_reg(D_FCH_PMIO_ALTMMIOEN, 0);
	const smn_reg_t bar = fch_pmio_smn_reg(D_FCH_PMIO_ALTMMIOBASE, 0);
	const struct memlist *ioml, *mmml;
	fch_rangespec_t *frp, *ufrp = NULL;
	size_t mlcount;
	uint_t rangecount, usable = 0;
	bool primary;

	if ((zen_ioms_flags(ioms) & ZEN_IOMS_F_HAS_FCH) == 0)
		return;

	primary = (zen_iodie_flags(iodie) & ZEN_IODIE_F_PRIMARY) != 0;

	if (primary) {
		uint32_t val;

		/*
		 * The FCH::PM::ALTMMIO{BASE,EN} registers don't have any
		 * effect on primary FCHs that we can tell, and we never set
		 * them for one.  If this has somehow come to be set, this
		 * implies an FCH we don't support and it may be hazardous to
		 * proceed.
		 */
		val = zen_iodie_read(iodie, enreg);
		if (FCH_PMIO_ALTMMIOEN_GET_EN(val) != 0) {
			dev_err(iop->io_dip, CE_WARN, "primary FCH has "
			    "alternate MMIO base address set; not enumerating "
			    "it");
			return;
		}
	}

	zen_fabric_gen_grant(ioms, &ioml, &mmml);
	mlcount = memlist_count(ioml) + memlist_count(mmml);
	if (mlcount == 0) {
		dev_err(iop->io_dip, CE_WARN, "no generic address space is "
		    "routed to this IOMS; not enumerating the FCH");
		return;
	}

	frp = kmem_zalloc(sizeof (fch_rangespec_t) * mlcount, KM_SLEEP);
	rangecount = ioms_memlist_to_ranges(ioml, frp, FA_LEGACY);
	rangecount += ioms_memlist_to_ranges(mmml, frp + rangecount, FA_MMIO);

	if (primary) {
		/*
		 * The primary FCH subtractively decodes everything the DF
		 * routes this way, so it is granted the whole pool.
		 */
		ufrp = frp;
		usable = rangecount;
	} else {
		/*
		 * A secondary FCH decodes only the 8KiB, 16-bit-aligned
		 * window programmed into its ALTMMIO BAR.  Find room for the
		 * window, program the BAR, and narrow the grant to exactly
		 * that window.  We would love to put this thing in 64-bit
		 * space but we cannot: while the BAR has a 64-bit option,
		 * setting it puts the region at 0xffff_ffff_XXXX_0000, an
		 * address this CPU cannot generate.  Sometimes all you can do
		 * is laugh.
		 *
		 * XXX It is also possible to route legacy I/O space to a
		 * secondary FCH and in turn allocate it to children just as a
		 * PCI bridge does.  When we want to use such a child, we will
		 * need to improve this.  See also fch_parent_base() in
		 * fch(4D).
		 */
		for (uint_t ridx = 0; ridx < rangecount; ridx++) {
			uint32_t val;
			uint64_t addr, size, end;

			if (frp[ridx].fr_addrsp != FA_MMIO)
				continue;
			if (frp[ridx].fr_physhi != 0)
				continue;
			size = fch_rangespec_size(frp + ridx);

			addr = fch_rangespec_addr(frp + ridx);
			end = addr + (size - 1);
			addr = P2ROUNDUP_TYPED(addr,
			    (1UL << FCH_PMIO_ALTMMIOBASE_SHIFT), uint64_t);

			if (addr + (FCH_PMIO_ALTMMIOBASE_SIZE - 1) > end)
				continue;

			ufrp = frp + ridx;
			usable = 1;

			ufrp->fr_physlo = (uint32_t)addr;
			ufrp->fr_sizelo = FCH_PMIO_ALTMMIOBASE_SIZE;
			ufrp->fr_sizehi = 0;

			val = zen_iodie_read(iodie, enreg);
			if (FCH_PMIO_ALTMMIOEN_GET_EN(val) != 0) {
				val = FCH_PMIO_ALTMMIOEN_SET_EN(val, 0);
				zen_iodie_write(iodie, enreg, val);
			}

			val = zen_iodie_read(iodie, bar);
			val = FCH_PMIO_ALTMMIOBASE_SET(val,
			    (uint32_t)addr >> FCH_PMIO_ALTMMIOBASE_SHIFT);
			zen_iodie_write(iodie, bar, val);

			val = FCH_PMIO_ALTMMIOEN_SET_EN(0, 1);
			val = FCH_PMIO_ALTMMIOEN_SET_WIDTH(val,
			    FCH_PMIO_ALTMMIOEN_WIDTH_32);
			zen_iodie_write(iodie, enreg, val);

			break;
		}
	}

	if (ufrp == NULL || usable == 0) {
		dev_err(iop->io_dip, CE_WARN, "no usable address space for "
		    "the FCH; not enumerating it");
		kmem_free(frp, sizeof (fch_rangespec_t) * mlcount);
		return;
	}

	iop->io_fch_primary = primary;
	iop->io_fch_nranges = usable;
	iop->io_fch_ranges =
	    kmem_alloc(sizeof (fch_rangespec_t) * usable, KM_SLEEP);
	bcopy(ufrp, iop->io_fch_ranges, sizeof (fch_rangespec_t) * usable);

	kmem_free(frp, sizeof (fch_rangespec_t) * mlcount);
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

	ioms_fch_init(iop);

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

	/*
	 * An instance that owns the FCH's address space grant hosts the
	 * console and boot-critical peripherals below it.  While a re-attach
	 * would recover the grant from the fabric, there is nothing useful to
	 * be gained from allowing this nexus to detach and much to lose if
	 * some part of the re-attach path fails.
	 */
	if (iop->io_fch_nranges != 0)
		return (DDI_FAILURE);

	ddi_soft_state_free(ioms_state, inst);

	return (DDI_SUCCESS);
}

/*
 * Idempotently create the fch child node, if this IOMS has an FCH and we
 * successfully took ownership of the address space it decodes at attach time.
 */
static int
ioms_config_fch(ioms_t *iop)
{
	dev_info_t *pdip = iop->io_dip;
	dev_info_t *cdip;
	const char *nodename;

	ASSERT(DEVI_BUSY_OWNED(pdip));

	if (iop->io_fch_nranges == 0)
		return (NDI_SUCCESS);

	nodename = fch_kind_name(chiprev_fch_kind(cpuid_getchiprev(CPU)));
	if (nodename == NULL) {
		/* There may be an FCH but we don't know what it is. */
		return (NDI_SUCCESS);
	}

	for (cdip = ddi_get_child(pdip); cdip != NULL;
	    cdip = ddi_get_next_sibling(cdip)) {
		/*
		 * The node already exists -- nothing more to do.
		 */
		if (strcmp(ddi_node_name(cdip), nodename) == 0)
			return (NDI_SUCCESS);
	}

	ndi_devi_alloc_sleep(pdip, nodename, (pnode_t)DEVI_SID_NODEID, &cdip);

	/*
	 * See the theory statement: the FCH is an address-less singleton, so
	 * the "unit-address" property is explicitly empty.
	 */
	if (ndi_prop_update_string(DDI_DEV_T_NONE, cdip, "unit-address", "") !=
	    NDI_SUCCESS) {
		dev_err(pdip, CE_WARN, "failed to create FCH 'unit-address' "
		    "property");
		goto fail;
	}

	if (ndi_prop_update_int_array(DDI_DEV_T_NONE, cdip,
	    FCH_PROPNAME_RANGES, (int *)iop->io_fch_ranges,
	    iop->io_fch_nranges * INTS_PER_RANGESPEC) != NDI_SUCCESS) {
		dev_err(pdip, CE_WARN, "failed to create FCH '%s' property",
		    FCH_PROPNAME_RANGES);
		goto fail;
	}

	if (ndi_prop_update_string(DDI_DEV_T_NONE, cdip,
	    FCH_PROPNAME_FABRIC_ROLE, iop->io_fch_primary ?
	    FCH_FABRIC_ROLE_PRI : FCH_FABRIC_ROLE_SEC) != NDI_SUCCESS) {
		dev_err(pdip, CE_WARN, "failed to create FCH '%s' property",
		    FCH_PROPNAME_FABRIC_ROLE);
		goto fail;
	}

	/*
	 * The primary FCH's own registers: the legacy interrupt-routing
	 * crossbar, in I/O port space, in the same rangespec format as the
	 * ranges above (see the theory statement).  It is the sole entry, at
	 * the register number fch(4D) maps it by.  A secondary FCH has no
	 * registers of its own and gets no "reg" at all.
	 */
	if (iop->io_fch_primary) {
		fch_rangespec_t reg = {
			.fr_addrsp = FA_LEGACY,
			.fr_physlo = FCH_IXBAR_IDX,
			.fr_sizelo = FCH_IXBAR_DATA - FCH_IXBAR_IDX + 1
		};

		CTASSERT(FCH_IXBAR_RNUM == 0);

		if (ndi_prop_update_int_array(DDI_DEV_T_NONE, cdip,
		    FCH_PROPNAME_REG, (int *)&reg,
		    INTS_PER_RANGESPEC) != NDI_SUCCESS) {
			dev_err(pdip, CE_WARN, "failed to create FCH '%s' "
			    "property", FCH_PROPNAME_REG);
			goto fail;
		}
	}

	/*
	 * It's fine if this fails (e.g., the driver may need to be added with
	 * add_drv).  Leave the newly created node as is and allow it to be
	 * bound later down the line.
	 */
	(void) ndi_devi_bind_driver(cdip, 0);

	return (NDI_SUCCESS);

fail:
	(void) ndi_devi_free(cdip);
	return (NDI_FAILURE);
}

/*
 * Idempotently create the node for this IOMS's PCIe root complex and have the
 * boot enumeration library fill in everything below it.  Every IOMS hosts an
 * IOHC that acts as a root complex with a root bus of its own, so unlike the
 * FCH there is one of these under each of us.
 *
 * The node is addressed by that root bus number which the library will also
 * record as the first half of the node's "bus-range".
 */
static int
ioms_config_rc(ioms_t *iop)
{
	dev_info_t *pdip = iop->io_dip;
	dev_info_t *cdip;
	uint32_t busno;
	char ua[5];
	int ret;

	ASSERT(DEVI_BUSY_OWNED(pdip));

	for (cdip = ddi_get_child(pdip); cdip != NULL;
	    cdip = ddi_get_next_sibling(cdip)) {
		/*
		 * The node already exists -- nothing more to do.
		 */
		if (strcmp(ddi_node_name(cdip), PCI_BOOT_RC_NODENAME) == 0)
			return (NDI_SUCCESS);
	}

	busno = zen_ioms_pci_busno(iop->io_ioms);
	(void) snprintf(ua, sizeof (ua), "%x", busno);

	ndi_devi_alloc_sleep(pdip, PCI_BOOT_RC_NODENAME,
	    (pnode_t)DEVI_SID_NODEID, &cdip);

	if (ndi_prop_update_string(DDI_DEV_T_NONE, cdip, "unit-address", ua) !=
	    NDI_SUCCESS) {
		dev_err(pdip, CE_WARN, "failed to create root complex "
		    "'unit-address' property");
		goto fail;
	}

	/*
	 * Configure the RC and any buses/devices under it.  This will also
	 * bind the correct driver.
	 */
	if ((ret = pci_boot_rc_config(cdip, busno)) != 0) {
		dev_err(pdip, CE_WARN, "failed to enumerate the root complex "
		    "on bus 0x%x: %d", busno, ret);
		goto fail;
	}

	return (NDI_SUCCESS);

fail:
	(void) ndi_devi_free(cdip);
	return (NDI_FAILURE);
}

static int
ioms_config_one(ioms_t *iop, const char *devname)
{
	const uint16_t busno = zen_ioms_pci_busno(iop->io_ioms);
	char *devname_dup, *cname, *caddr;
	unsigned long cua;
	size_t devname_sz;
	const char *fchname;
	bool rc, fch;

	fchname = fch_kind_name(chiprev_fch_kind(cpuid_getchiprev(CPU)));

	devname_dup = i_ddi_strdup(devname, KM_SLEEP);
	devname_sz = strlen(devname_dup) + 1;
	i_ddi_parse_name(devname_dup, &cname, &caddr, NULL);

	/*
	 * We have at most two children, and which one is being asked for is
	 * decided by the name.
	 */
	rc = cname != NULL && strcmp(cname, PCI_BOOT_RC_NODENAME) == 0;
	fch = iop->io_fch_nranges != 0 && fchname != NULL &&
	    cname != NULL && strcmp(cname, fchname) == 0 &&
	    (caddr == NULL || *caddr == '\0');

	/*
	 * The FCH is an address-less singleton, and only the one IOMS that has
	 * one offers it.  But the root complex is addressed by its bus number,
	 * so we validate that.
	 */
	if (rc && (ddi_strtoul(caddr, NULL, 16, &cua) != 0 || cua != busno)) {
		kmem_free(devname_dup, devname_sz);
		return (NDI_EINVAL);
	}

	kmem_free(devname_dup, devname_sz);

	if (fch)
		return (ioms_config_fch(iop));
	if (rc)

		return (ioms_config_rc(iop));

	return (NDI_EINVAL);
}

static int
ioms_bus_config(dev_info_t *pdip, uint_t flags, ddi_bus_config_op_t op,
    void *arg, dev_info_t **childp)
{
	ioms_t *iop = ddi_get_soft_state(ioms_state, ddi_get_instance(pdip));
	int ret;

	if (iop == NULL)
		return (NDI_BADHANDLE);

	switch (op) {
	case BUS_CONFIG_ONE:
	case BUS_CONFIG_ALL:
	case BUS_CONFIG_DRIVER:
		break;
	default:
		return (NDI_FAILURE);
	}

	ndi_devi_enter(pdip);
	if (op == BUS_CONFIG_ONE) {
		ASSERT3P(arg, !=, NULL);
		ret = ioms_config_one(iop, (const char *)arg);
	} else {
		ret = ioms_config_fch(iop);
		if (ret == NDI_SUCCESS)
			ret = ioms_config_rc(iop);
	}
	ndi_devi_exit(pdip);

	if (ret != NDI_SUCCESS)
		return (ret);

	flags |= NDI_ONLINE_ATTACH;
	return (ndi_busop_bus_config(pdip, flags, op, arg, childp, 0));
}

static int
ioms_bus_unconfig(dev_info_t *pdip, uint_t flags, ddi_bus_config_op_t op,
    void *arg)
{
	switch (op) {
	case BUS_UNCONFIG_ONE:
	case BUS_UNCONFIG_ALL:
	case BUS_UNCONFIG_DRIVER:
		/*
		 * Our children are stable, fabric-derived nodes: they may be
		 * detached, but the nodes themselves (and the properties
		 * describing their resource grants) persist, and a later
		 * bus_config finds and re-onlines them.
		 */
		return (ndi_busop_bus_unconfig(pdip, flags, op, arg));
	default:
		return (NDI_FAILURE);
	}
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
	case DDI_CTLOPS_INITCHILD: {
		dev_info_t *cdip = arg;
		char *ua;

		/*
		 * We create all of our children and name them from the
		 * "unit-address" property we gave them.  A child without one
		 * (including any .conf child, which we do not support) is not
		 * ours and cannot be named.  There is no legacy "reg"-derived
		 * naming and no sunbus parent-private data here: our
		 * children's "reg" is in the shared rangespec format,
		 * consumed directly by our bus_map and REGSIZE/NREGS below.
		 */
		if (ndi_dev_is_persistent_node(cdip) == 0)
			return (DDI_NOT_WELL_FORMED);

		if (ddi_prop_lookup_string(DDI_DEV_T_ANY, cdip,
		    DDI_PROP_DONTPASS, "unit-address", &ua) !=
		    DDI_PROP_SUCCESS) {
			return (DDI_NOT_WELL_FORMED);
		}

		ddi_set_name_addr(cdip, ua);
		ddi_prop_free(ua);
		break;
	}
	case DDI_CTLOPS_UNINITCHILD:
		ddi_set_name_addr((dev_info_t *)arg, NULL);
		break;
	case DDI_CTLOPS_REGSIZE:
	case DDI_CTLOPS_NREGS: {
		fch_rangespec_t *frp;
		uint_t nreg;

		if (rdip == NULL)
			return (DDI_FAILURE);

		/*
		 * fch(4D) is currently our only child and the only other party
		 * that expects our "reg"-as-rangespec convention.
		 */
		if (strcmp(ddi_driver_name(rdip), "fch") != 0)
			return (DDI_FAILURE);

		nreg = fch_get_regs(rdip, &frp);

		if (ctlop == DDI_CTLOPS_NREGS) {
			/*
			 * A child with no registers (which is valid here: a
			 * secondary FCH has none) gets a failure rather than
			 * zero, matching fch(4D)'s own treatment of its
			 * children.
			 */
			if (nreg == 0)
				return (DDI_FAILURE);

			*(int *)result = (int)nreg;
		} else {
			uint_t idx = (uint_t)(*(int *)arg);

			if (idx >= nreg) {
				if (nreg != 0)
					ddi_prop_free(frp);
				return (DDI_FAILURE);
			}
			*(off_t *)result = (off_t)fch_rangespec_size(frp + idx);
		}

		ddi_prop_free(frp);
		return (DDI_SUCCESS);
	}
	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}

	return (DDI_SUCCESS);
}

static int
ioms_bus_map(dev_info_t *dip, dev_info_t *rdip, ddi_map_req_t *mp, off_t offset,
    off_t len, caddr_t *vaddrp)
{
	switch (mp->map_type) {
	case DDI_MT_RNUMBER: {
		int rnumber = mp->map_obj.rnumber;
		ddi_map_req_t mr = *mp;
		struct regspec64 rs;
		fch_rangespec_t *frp;
		uint_t nreg;

		/*
		 * fch(4D) is currently our only child and the only other party
		 * that expects our "reg"-as-rangespec convention.
		 */
		if (strcmp(ddi_driver_name(rdip), "fch") != 0)
			return (DDI_FAILURE);

		/*
		 * A child mapping its own registers with ddi_regs_map_setup(9F)
		 * (fch(4D) and its interrupt crossbar, ixbar) refers to them by
		 * index into the "reg" property we gave it, in the shared
		 * rangespec format.  Resolve the index against the property and
		 * pass the request toward the root nexus as an extended
		 * regspec, leaving offset and length for it to apply, just as
		 * it does for its own children's rnumber requests.
		 */
		nreg = fch_get_regs(rdip, &frp);
		if (rnumber < 0 || (uint_t)rnumber >= nreg) {
			if (nreg != 0)
				ddi_prop_free(frp);

			return (DDI_ME_RNUMBER_RANGE);
		}

		rs.regspec_bustype =
		    fch_addrsp_to_bustype(frp[rnumber].fr_addrsp);
		rs.regspec_addr = fch_rangespec_addr(frp + rnumber);
		rs.regspec_size = fch_rangespec_size(frp + rnumber);
		ddi_prop_free(frp);

		mr.map_type = DDI_MT_REGSPEC;
		mr.map_obj.rp = (struct regspec *)&rs;
		mr.map_flags |= DDI_MF_EXT_REGSPEC;

		return (ddi_map(dip, &mr, offset, len, vaddrp));
	}
	case DDI_MT_REGSPEC:
		/*
		 * An already-resolved regspec, e.g. fch(4D) mapping on behalf
		 * of one of its children, just flows up toward the root
		 * nexus, which performs all such mappings for the fabric nexi.
		 */
		return (ddi_map(dip, mp, offset, len, vaddrp));
	default:
		return (DDI_ME_INVAL);
	}
}

static int
ioms_fm_init(dev_info_t *dip, dev_info_t *tdip, int cap,
    ddi_iblock_cookie_t *ibc)
{
	return (i_ndi_busop_fm_init(dip, cap, ibc));
}

static struct bus_ops ioms_bus_ops = {
	.busops_rev = BUSO_REV,
	.bus_map = ioms_bus_map,
	.bus_dma_allochdl = ddi_dma_allochdl,
	.bus_dma_freehdl = ddi_dma_freehdl,
	.bus_dma_bindhdl = ddi_dma_bindhdl,
	.bus_dma_unbindhdl = ddi_dma_unbindhdl,
	.bus_dma_flush = ddi_dma_flush,
	.bus_dma_win = ddi_dma_win,
	.bus_dma_ctl = ddi_dma_mctl,
	.bus_prop_op = ddi_bus_prop_op,
	.bus_ctl = ioms_bus_ctl,
	.bus_config = ioms_bus_config,
	.bus_unconfig = ioms_bus_unconfig,
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
