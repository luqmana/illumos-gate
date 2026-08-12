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
 * This implements the interfaces required to get PCI resource discovery out to
 * the rest of the system. This is effectively a thin veneer around the
 * platform-specific code and related pieces of unix.
 */

#include <sys/plat/pci_prd.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/pci.h>
#include <sys/sunndi.h>
#include <sys/memlist_impl.h>
#include <sys/pcie_impl.h>
#include <sys/io/zen/ioms.h>
#include <sys/io/zen/pcie_impl.h>

#define	BUS(bdf) (((bdf) & PCIE_REQ_ID_BUS_MASK) >> PCIE_REQ_ID_BUS_SHIFT)
#define	DEV(bdf) (((bdf) & PCIE_REQ_ID_DEV_MASK) >> PCIE_REQ_ID_DEV_SHIFT)
#define	FUNC(bdf) (((bdf) & PCIE_REQ_ID_FUNC_MASK) >> PCIE_REQ_ID_FUNC_SHIFT)

static pci_prd_upcalls_t *prd_upcalls;

/*
 * We always just tell the system to scan all PCI buses.
 */
uint32_t
pci_prd_max_bus(void)
{
	return (PCI_MAX_BUS_NUM - 1);
}

static struct memlist *
pci_prd_grant(dev_info_t *dip, pci_prd_rsrc_t rsrc)
{
	pci_regspec_t *ps = NULL;
	struct memlist *ml = NULL;
	uint_t nint, nspec;
	uint32_t want;

	switch (rsrc) {
	case PCI_PRD_R_IO:
		want = PCI_ADDR_IO;
		break;
	case PCI_PRD_R_MMIO:
		want = PCI_ADDR_MEM32;
		break;
	case PCI_PRD_R_PREFETCH:
		want = PCI_ADDR_MEM32 | PCI_PREFETCH_B;
		break;
	default:
		return (NULL);
	}

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    IOMS_PROP_PCI_GRANT, (int **)&ps, &nint) != DDI_SUCCESS) {
		return (NULL);
	}

	nspec = nint / (sizeof (pci_regspec_t) / sizeof (int));

	for (uint_t i = 0; i < nspec; i++) {
		uint64_t base, len;

		if ((ps[i].pci_phys_hi & PCI_PREFETCH_B) !=
		    (want & PCI_PREFETCH_B)) {
			continue;
		}

		switch (ps[i].pci_phys_hi & PCI_ADDR_MASK) {
		case PCI_ADDR_IO:
			if ((want & PCI_ADDR_MASK) != PCI_ADDR_IO)
				continue;
			break;
		case PCI_ADDR_MEM32:
		case PCI_ADDR_MEM64:
			if ((want & PCI_ADDR_MASK) == PCI_ADDR_IO)
				continue;
			break;
		default:
			continue;
		}

		base = (uint64_t)ps[i].pci_phys_mid << 32 |
		    (uint64_t)ps[i].pci_phys_low;
		len = (uint64_t)ps[i].pci_size_hi << 32 |
		    (uint64_t)ps[i].pci_size_low;

		if (len != 0)
			(void) memlist_rsrc_add(base, len, &ml);
	}

	ddi_prop_free(ps);

	return (ml);
}

static struct memlist *
pci_prd_bus_grant(dev_info_t *dip)
{
	struct memlist *ml = NULL;
	int *range = NULL;
	uint_t nint;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    IOMS_PROP_BUS_GRANT, &range, &nint) != DDI_SUCCESS) {
		return (NULL);
	}

	if (nint == 2 && range[1] >= range[0])
		(void) memlist_rsrc_add(range[0], range[1] - range[0] + 1, &ml);

	ddi_prop_free(range);

	return (ml);
}

/*
 * The ioms(4D) nexus which creates the root complex nodes publishes the
 * address space routed to each as properties we can retrieve.
 * See IOMS_PROP_PCI_GRANT / IOMS_PROP_BUS_GRANT in <sys/io/zen/ioms.h>.
 */
struct memlist *
pci_prd_find_resource(uint32_t bus, pci_prd_rsrc_t rsrc)
{
	dev_info_t *dip;

	if (prd_upcalls == NULL ||
	    (dip = prd_upcalls->pru_bus2dip_f(bus)) == NULL) {
		return (NULL);
	}

	switch (rsrc) {
	case PCI_PRD_R_BUS:
		return (pci_prd_bus_grant(dip));
	case PCI_PRD_R_IO:
	case PCI_PRD_R_MMIO:
	case PCI_PRD_R_PREFETCH:
		return (pci_prd_grant(dip, rsrc));
	default:
		return (NULL);
	}
}

/*
 * No broken BIOS here!
 */
boolean_t
pci_prd_multi_root_ok(void)
{
	return (B_TRUE);
}

/*
 * We expect all PCI(e) devices to be in their reset state. If any of them do
 * happen to have been programmed (or not been properly reset from the last
 * boot), we always want to ignore that and lay things out ourselves.
 */
boolean_t
pci_prd_ignore_firmware(void)
{
	return (B_TRUE);
}

/*
 * Retrieve the requested LTSSM snapshot for the link below the given PCIe
 * bridge.
 */
int
pci_prd_pcie_ltssm(dev_info_t *bridge, pcie_ltssm_snap_t snap,
    pcie_ltssm_snapshot_t *snapshot)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(bridge);
	pcie_req_id_t bdf;

	if (bus_p == NULL)
		return (ENXIO);

	bdf = bus_p->bus_bdf;

	return (zen_pcie_ltssm_by_bdf(BUS(bdf), DEV(bdf), FUNC(bdf),
	    snap, snapshot));
}

/*
 * Record an LTSSM capture for the link below the given PCIe bridge in response
 * to a link state change.
 */
void
pci_prd_pcie_link_event(dev_info_t *bridge, boolean_t up)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(bridge);
	pcie_req_id_t bdf;

	if (bus_p == NULL)
		return;

	bdf = bus_p->bus_bdf;

	(void) zen_pcie_ltssm_link_event_by_bdf(BUS(bdf), DEV(bdf), FUNC(bdf),
	    up);
}

/*
 * Retrieve link equalisation (EQ) data for the link below the given PCIe
 * bridge, and program its equalisation preset search mask.
 */
int
pci_prd_pcie_eq(dev_info_t *bridge, pcie_link_speed_t speed, uint32_t nlanes,
    pcie_eq_t *eq)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(bridge);
	pcie_req_id_t bdf;

	if (bus_p == NULL)
		return (ENXIO);

	bdf = bus_p->bus_bdf;
	return (zen_pcie_eq_by_bdf(BUS(bdf), DEV(bdf), FUNC(bdf),
	    speed, nlanes, eq));
}

int
pci_prd_pcie_set_preset_mask(dev_info_t *bridge, pcie_link_speed_t speed,
    uint32_t mask)
{
	pcie_bus_t *bus_p = PCIE_DIP2BUS(bridge);
	pcie_req_id_t bdf;

	if (bus_p == NULL)
		return (ENXIO);

	bdf = bus_p->bus_bdf;
	return (zen_pcie_set_preset_mask_by_bdf(BUS(bdf), DEV(bdf), FUNC(bdf),
	    speed, mask));
}

int
pci_prd_init(pci_prd_upcalls_t *upcalls)
{
	prd_upcalls = upcalls;

	return (0);
}

void
pci_prd_fini(void)
{

}

/*
 * Boot-time PCI enumeration walks devices that, on a PC, may need errata
 * worked around, may keep registers somewhere other than where their base
 * address registers say, or may want nodes of their own created.  None of
 * that applies here: this architecture's devices are described by the fabric
 * and behave as PCI Express says they should, so we have nothing to add as
 * enumeration goes past them.
 */
const struct pci_boot_ops *
pci_prd_boot_ops(void)
{
	return (NULL);
}

/*
 * The PCI enumeration code only calls this for the whole-system flow that
 * relies on searching for present buses (and asking the platform for any it
 * might've missed via this).  On oxide we have an ioms(4D) instance for each
 * root complex present on the system which then drives the enumeration process
 * via the per-RC flow thus leaving this a no-op.
 */
void
pci_prd_root_complex_iter(pci_prd_root_complex_f func, void *arg)
{
}

/*
 * We have no alternative slot naming here. So this is a no-op and thus empty
 * function.
 */
void
pci_prd_slot_name(uint32_t bus, dev_info_t *dip)
{

}

/*
 * This indicates to the system what naming compatibility we would like to have.
 * On the Oxide platform, ISA free is the way to be, hence we don't need that.
 * Similarly we don't need bridge compatibility because we know all the systems
 * that the Oxide architecture runs on and we are not subject to issues there.
 * We still set PCI_PRD_COMPAT_PCI_NODE_NAME not for a principled reason, but
 * mostly to minimize risk late in the cycle here. XXX I am a chicken.
 */
pci_prd_compat_flags_t
pci_prd_compat_flags(void)
{
	return (PCI_PRD_COMPAT_PCI_NODE_NAME);
}

static struct modlmisc pci_prd_modlmisc_oxide = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "Oxide PCI Resource Discovery"
};

static struct modlinkage pci_prd_modlinkage_oxide = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &pci_prd_modlmisc_oxide, NULL }
};

int
_init(void)
{
	return (mod_install(&pci_prd_modlinkage_oxide));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&pci_prd_modlinkage_oxide, modinfop));
}

int
_fini(void)
{
	return (mod_remove(&pci_prd_modlinkage_oxide));
}
