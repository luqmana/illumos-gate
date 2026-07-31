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
 * A nexus driver representing each data fabric (DF) instance in the system.
 * Every I/O die in an AMD Zen-family SoC implements a DF instance -- the
 * crossbar that connects the compute complexes, memory controllers, and the
 * I/O subsystem to one another and, via inter-socket links, to the other
 * sockets' fabrics.  Each DF instance has a node ID by which the other
 * instances address it and on all currently supported processors there is
 * exactly one I/O die -- therefore one DF instance, per socket.
 *
 * There is one instance of this driver for every DF instance (equivalently,
 * every I/O die), named df@<node_id>.  Our device nodes are created by the
 * root nexus' bus_config from the kernel's fabric topology (see
 * uts/oxide/io/rootnex.c).  Our own bus_config in turn then creates the nodes
 * for the IOMS units on our die, which host the PCIe root complexes -- see
 * ioms(4D) for that story and for the terminology.
 *
 * At present this driver is only structure: it identifies its DF instance,
 * parents the ioms nodes, and passes DMA, interrupt, and FM operations
 * through between its children and the root nexus.  Attaching it has no
 * effect on the fabric itself, which is configured by unix long before the
 * DDI comes up.
 */

#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/ddi_implfuncs.h>
#include <sys/ddi_intr_impl.h>
#include <sys/ddifm_impl.h>
#include <sys/debug.h>
#include <sys/modctl.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/systm.h>
#include <sys/stdbool.h>
#include <sys/types.h>
#include <sys/io/zen/df.h>
#include <sys/io/zen/ioms.h>
#include <sys/io/zen/fabric.h>
#include <sys/io/zen/fabric_limits.h>


/*
 * Per-instance state: the device node and the fabric I/O die it represents.
 */
typedef struct df {
	dev_info_t	*df_dip;
	zen_iodie_t	*df_iodie;
} df_t;

static void *df_state;

typedef struct df_match {
	uint32_t	dm_nodeid;
	zen_iodie_t	*dm_iodie;
} df_match_t;

static int
df_match_cb(zen_iodie_t *iodie, void *arg)
{
	df_match_t *dm = arg;

	if (zen_iodie_node_id(iodie) == dm->dm_nodeid) {
		dm->dm_iodie = iodie;
		return (1);
	}

	return (0);
}

static bool
df_set_iprop(dev_info_t *dip, const char *name, int val)
{
	if (ndi_prop_update_int(DDI_DEV_T_NONE, dip, (char *)name, val) !=
	    NDI_SUCCESS) {
		cmn_err(CE_WARN, "df: failed to create '%s' property", name);
		return (false);
	}

	return (true);
}

static int
df_create_ioms(df_t *df, zen_ioms_t *ioms)
{
	dev_info_t *pdip = df->df_dip;
	dev_info_t *dip;
	const uint16_t nodeid = zen_iodie_node_id(zen_ioms_iodie(ioms));
	const uint8_t iomsno = zen_ioms_num(ioms);
	const char *iohctype =
	    zen_ioms_iohc_type(ioms) == ZEN_IOHCT_LARGE ? "large" : "small";
	char ua[8];

	ASSERT(DEVI_BUSY_OWNED(pdip));
	ASSERT3P(zen_ioms_iodie(ioms), ==, df->df_iodie);

	for (dip = ddi_get_child(pdip); dip != NULL;
	    dip = ddi_get_next_sibling(dip)) {
		if (strcmp(ddi_node_name(dip), IOMS_NODENAME) != 0)
			continue;
		/*
		 * A node already exists for this IOMS -- nothing more to do.
		 */
		ASSERT3U(iomsno, !=, (uint16_t)-1);
		if (ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
		    IOMS_PROP_IOMS, -1) == iomsno)
			return (NDI_SUCCESS);
	}

	ndi_devi_alloc_sleep(pdip, IOMS_NODENAME, (pnode_t)DEVI_SID_NODEID,
	    &dip);

	(void) snprintf(ua, sizeof (ua), "%x", iomsno);
	if (ndi_prop_update_string(DDI_DEV_T_NONE, dip, "unit-address",
	    ua) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "df: failed to create 'unit-address' "
		    "property");
		goto fail;
	}

	if (!df_set_iprop(dip, IOMS_PROP_NODE_ID, nodeid) ||
	    !df_set_iprop(dip, IOMS_PROP_IOMS, iomsno) ||
	    !df_set_iprop(dip, IOMS_PROP_NBIO,
	    zen_nbio_num(zen_ioms_nbio(ioms))) ||
	    !df_set_iprop(dip, IOMS_PROP_IOHUB, zen_ioms_iohub_num(ioms)) ||
	    !df_set_iprop(dip, IOMS_PROP_IOHC, zen_ioms_iohc_num(ioms)) ||
	    !df_set_iprop(dip, IOMS_PROP_PCI_BUS, zen_ioms_pci_busno(ioms)) ||
	    !df_set_iprop(dip, IOMS_PROP_FABRIC_ID,
	    zen_ioms_fabric_id(ioms))) {
		goto fail;
	}

	if (ndi_prop_update_string(DDI_DEV_T_NONE, dip, IOMS_PROP_IOHC_TYPE,
	    (char *)iohctype) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "df: failed to create '%s' property",
		    IOMS_PROP_IOHC_TYPE);
		goto fail;
	}

	if (ndi_devi_bind_driver(dip, 0) != NDI_SUCCESS) {
		cmn_err(CE_WARN, "df: failed to bind node %s@%s",
		    IOMS_NODENAME, ua);
		goto fail;
	}

	return (NDI_SUCCESS);

fail:
	(void) ndi_devi_free(dip);
	return (NDI_FAILURE);
}

/*
 * Walk callback for BUS_CONFIG_ALL / BUS_CONFIG_DRIVER: create the node for
 * every IOMS on our die.  A creation failure is reported (in df_create_ioms)
 * but does not terminate the walk, as the remaining instances are
 * independently useful.
 */
static int
df_config_ioms_cb(zen_ioms_t *ioms, void *arg)
{
	df_t *df = arg;

	if (zen_iodie_node_id(zen_ioms_iodie(ioms)) ==
	    zen_iodie_node_id(df->df_iodie)) {
		(void) df_create_ioms(df, ioms);
	}

	return (0);
}

typedef struct df_ioms_find {
	zen_iodie_t	*dif_iodie;
	uint8_t		dif_num;
	zen_ioms_t	*dif_ioms;
} df_ioms_find_t;

static int
df_find_ioms_cb(zen_ioms_t *ioms, void *arg)
{
	df_ioms_find_t *find = arg;

	if (zen_ioms_iodie(ioms) == find->dif_iodie &&
	    zen_ioms_num(ioms) == find->dif_num) {
		find->dif_ioms = ioms;
		return (1);
	}

	return (0);
}

static int
df_config_one(df_t *df, const char *devname)
{
	char *devname_dup, *cdrv, *caddr;
	size_t devname_sz;
	u_longlong_t num;
	df_ioms_find_t find;

	devname_dup = i_ddi_strdup(devname, KM_SLEEP);
	devname_sz = strlen(devname_dup) + 1;
	i_ddi_parse_name(devname_dup, &cdrv, &caddr, NULL);

	if (cdrv == NULL || caddr == NULL || strcmp(cdrv, IOMS_NODENAME) != 0 ||
	    ddi_strtoull(caddr, NULL, 16, &num) != 0 ||
	    num > UINT8_MAX) {
		kmem_free(devname_dup, devname_sz);
		return (NDI_EINVAL);
	}

	find.dif_iodie = df->df_iodie;
	find.dif_num = (uint8_t)num;
	find.dif_ioms = NULL;

	kmem_free(devname_dup, devname_sz);

	(void) zen_walk_ioms(df_find_ioms_cb, &find);
	if (find.dif_ioms == NULL)
		return (NDI_EINVAL);

	return (df_create_ioms(df, find.dif_ioms));
}

static int
df_bus_config(dev_info_t *pdip, uint_t flags, ddi_bus_config_op_t op,
    void *arg, dev_info_t **childp)
{
	df_t *df = ddi_get_soft_state(df_state, ddi_get_instance(pdip));
	int ret;

	if (df == NULL)
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
		ret = df_config_one(df, (const char *)arg);
	} else {
		(void) zen_walk_ioms(df_config_ioms_cb, df);
		ret = NDI_SUCCESS;
	}
	ndi_devi_exit(pdip);

	if (ret != NDI_SUCCESS)
		return (ret);

	flags |= NDI_ONLINE_ATTACH;
	return (ndi_busop_bus_config(pdip, flags, op, arg, childp, 0));
}

static int
df_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	df_t *df;
	df_match_t dm = { 0 };
	int inst, nodeid;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	nodeid = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    DF_PROP_NODE_ID, -1);
	if (nodeid < 0) {
		dev_err(dip, CE_WARN, "missing '%s' property",
		    DF_PROP_NODE_ID);
		return (DDI_FAILURE);
	}

	dm.dm_nodeid = (uint32_t)nodeid;
	(void) zen_walk_iodie(df_match_cb, &dm);
	if (dm.dm_iodie == NULL) {
		dev_err(dip, CE_WARN, "no fabric I/O die matches node ID %d",
		    nodeid);
		return (DDI_FAILURE);
	}

	inst = ddi_get_instance(dip);
	VERIFY0(ddi_soft_state_zalloc(df_state, inst));
	df = ddi_get_soft_state(df_state, inst);
	df->df_dip = dip;
	df->df_iodie = dm.dm_iodie;

	/*
	 * Eagerly create (but do not attach) our ioms children.
	 */
	ndi_devi_enter(dip);
	(void) zen_walk_ioms(df_config_ioms_cb, df);
	ndi_devi_exit(dip);

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
df_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	df_t *df;
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
	df = ddi_get_soft_state(df_state, inst);
	if (df == NULL || df->df_dip != dip)
		return (DDI_FAILURE);

	ddi_soft_state_free(df_state, inst);

	return (DDI_SUCCESS);
}

static int
df_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop,
    void *arg, void *result)
{
	switch (ctlop) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == NULL)
			return (DDI_FAILURE);
		cmn_err(CE_CONT, "?DF device: %s@%s, %s%d\n",
		    ddi_node_name(rdip), ddi_get_name_addr(rdip),
		    ddi_driver_name(rdip), ddi_get_instance(rdip));
		break;
	case DDI_CTLOPS_INITCHILD:
		return (impl_ddi_sunbus_initchild(arg));
	case DDI_CTLOPS_UNINITCHILD:
		impl_ddi_sunbus_removechild(arg);
		break;
	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}

	return (DDI_SUCCESS);
}

static int
df_fm_init(dev_info_t *dip, dev_info_t *tdip, int cap, ddi_iblock_cookie_t *ibc)
{
	return (i_ndi_busop_fm_init(dip, cap, ibc));
}

static struct bus_ops df_bus_ops = {
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
	.bus_ctl = df_bus_ctl,
	.bus_config = df_bus_config,
	.bus_fm_init = df_fm_init,
	.bus_intr_op = i_ddi_intr_ops
};

static struct dev_ops df_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_getinfo = nodev,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = df_attach,
	.devo_detach = df_detach,
	.devo_reset = nodev,
	.devo_quiesce = ddi_quiesce_not_needed,
	.devo_bus_ops = &df_bus_ops
};

static struct modldrv df_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "AMD Zen Data Fabric Nexus Driver",
	.drv_dev_ops = &df_dev_ops
};

static struct modlinkage df_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &df_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	ret = ddi_soft_state_init(&df_state, sizeof (df_t),
	    ZEN_FABRIC_MAX_IO_DIES);
	if (ret != 0) {
		return (ret);
	}

	if ((ret = mod_install(&df_modlinkage)) != 0) {
		ddi_soft_state_fini(&df_state);
		return (ret);
	}

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&df_modlinkage, modinfop));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&df_modlinkage)) != 0)
		return (ret);

	ddi_soft_state_fini(&df_state);
	return (0);
}
