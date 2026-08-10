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

#ifndef	_SYS_PCI_BOOT_H
#define	_SYS_PCI_BOOT_H

/*
 * Interfaces exported by the misc/pci_boot module, which enumerates PCI at
 * boot and programs the resources of what it finds.  These are Private to the
 * platform code that drives that enumeration; the module itself has no
 * opinion about when or on whose behalf it runs.
 */

#include <sys/types.h>
#include <sys/dditypes.h>
#include <sys/pci_props.h>
#include <sys/plat/pci_prd.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * The operations enumeration calls out to as it walks each device.  The
 * enumeration itself is generic, but the devices it walks past are not:
 * individual chipsets need errata worked around, some devices keep their
 * registers somewhere other than where they say, and some platforms want
 * device nodes of their own created for what is found.  All of that is
 * knowledge the platform has and the library does not, so it is registered
 * here rather than carried in the library, which lets a platform that needs
 * none of it do nothing at all.
 *
 * All of these are optional: a NULL member, or registering nothing, simply
 * means there is nothing to do at that point.  Each is called once per device
 * function, with its properties already read, and must not assume it is
 * called for functions in any particular order.
 *
 * Errata that must be worked around before enumeration so much as looks at
 * other devices are not here: they run before any of this, in their own pass,
 * and are registered as fix/unfix pairs through pci_prd_upcalls_t.
 *
 *  pbo_devinit_f Called as each function is enumerated, once its node exists
 *		  and has been named, but before the PCIe framework has
 *		  initialized against it.  Fixes that must be in place before
 *		  anything reads that device's configuration space, notably ones
 *		  concerning how that space may be accessed at all, belong here.
 *
 *  pbo_devdone_f Called once a function's node has been fully populated.
 *		  Suitable for quirks that need the finished node, or that
 *		  create nodes of their own.
 *
 * The last three concern devices the platform *claims*: ones whose registers
 * their base address registers do not describe in the usual way, such as a
 * storage controller in a legacy compatibility mode, where some registers are
 * at addresses fixed by convention rather than at the ones the device
 * reports.  Enumeration cannot know about such conventions, so it hands the
 * device to the platform instead:
 *
 *  pbo_claim_f	  Called once a function's node exists and its compatible
 *		  properties are set, but before its registers are described.
 *		  Returns whether the platform is claiming the device, in
 *		  which case it may also have renamed the node so that the
 *		  driver it wants will bind.  A claimed device is never
 *		  reprogrammed.
 *
 *  pbo_claimed_f Called once a claimed device has been bound to its driver,
 *		  for whatever else the platform wants to do with it, such as
 *		  creating the children the driver expects to find.
 *
 *  pbo_io_bar_f  Called for each base address register of a claimed device,
 *		  with the base the register holds and the length it reports.
 *		  Returns whether the platform is describing this register, in
 *		  which case it has set the base and length to use and said
 *		  whether that address is hard-decoded (fixed, and so not
 *		  relocatable).
 *
 *		  Only I/O space can be described this way: a register the
 *		  platform claims here is taken to be I/O ports, which is what
 *		  the conventions this exists for (legacy compatibility-mode
 *		  port ranges) are made of.  Returning false leaves the
 *		  register to be interpreted normally, and is the only
 *		  outcome under which it may turn out to be memory.
 *
 * Finally, and separately from claiming, a device may decode address space
 * that no base address register describes because convention says it does, e.g.
 * the legacy aliases of a VGA adapter.  Enumeration has no way to know of such
 * conventions, so it asks:
 *
 *  pbo_aliases_f Called as each function's registers are described, to fill
 *		  in up to the given number of regions the device decodes by
 *		  convention.  Returns how many it filled in; enumeration
 *		  describes them on the node and accounts for them against
 *		  the bus's resources as it does for any other register.
 *
 *  pbo_legacy_range_f
 *		  Returns whether the given region is one that exists at a
 *		  fixed address on this platform by convention.  Enumeration
 *		  has to recognize such regions wherever they turn up, since
 *		  they are not space it may hand out: it must not stretch a
 *		  bridge's window to cover one, nor conclude that a bridge
 *		  holding nothing else is worth reprogramming.
 *
 *  pbo_bridge_regions_f
 *		  Called for each bridge, to fill in up to the given number of
 *		  regions that bridge passes to its secondary bus regardless
 *		  of its windows (that instead might be expressed in a platform
 *		  specific way enumeration doesn't know about). Returns how many
 *		  it filled in, zero if the bridge forwards nothing.
 *		  Enumeration gives what is reported to the secondary bus and
 *		  accounts for it as used on the parent.
 */

typedef struct pci_boot_region {
	boolean_t	pbr_io;		/* I/O space, otherwise 32-bit memory */
	uint32_t	pbr_base;
	uint32_t	pbr_len;
} pci_boot_region_t;

#define	PCI_BOOT_MAX_REGIONS	6

typedef struct pci_boot_ops {
	void (*pbo_devinit_f)(dev_info_t *, uint8_t, uint8_t, uint8_t,
	    const pci_prop_data_t *);
	void (*pbo_devdone_f)(dev_info_t *, uint8_t, uint8_t, uint8_t,
	    const pci_prop_data_t *);
	boolean_t (*pbo_claim_f)(dev_info_t *, uint8_t, uint8_t, uint8_t,
	    const pci_prop_data_t *);
	void (*pbo_claimed_f)(dev_info_t *, uint8_t, uint8_t, uint8_t,
	    const pci_prop_data_t *);
	boolean_t (*pbo_io_bar_f)(dev_info_t *, uint8_t, uint8_t, uint8_t,
	    uint_t, uint32_t *, uint_t *, boolean_t *);
	uint_t (*pbo_aliases_f)(uint8_t, uint8_t, uint8_t, pci_boot_region_t *,
	    uint_t);
	boolean_t (*pbo_legacy_range_f)(uint64_t, uint64_t, boolean_t);
	uint_t (*pbo_bridge_regions_f)(uint8_t, uint8_t, uint8_t,
	    pci_boot_region_t *, uint_t);
} pci_boot_ops_t;


/*
 * The highest bus number that enumeration should consider, and debug
 * controls.  pci_boot_maxbus must be set before the first enumeration pass.
 */
extern int pci_boot_maxbus;
extern int pci_boot_debug;

/*
 * Returns the device node of the root bus with the given bus number, or NULL
 * if no such node has been created.  Supplied to the platform resource
 * discovery module as an upcall.
 */
extern dev_info_t *pci_boot_bus_to_dip(uint32_t);

/*
 * Readies the fix machinery during module initialization before the platform
 * is offered the chance to register anything.
 */
extern void pci_boot_fix_init(void);

/*
 * Records a fix to be applied to devices before enumeration begins, and the
 * means of undoing it afterwards; see pci_prd_upcalls_t, through which the
 * platform reaches this.  Enumeration owns the record of which devices a fix
 * was applied to, so that all a platform has to supply is the pair itself.
 */
extern void pci_boot_register_fix(pci_prd_fix_f, pci_prd_unfix_f);

/*
 * The whole-system, two-pass enumeration flow used on i86pc.  The first pass
 * creates the device tree; the second reprograms devices the BIOS did not set
 * up.  Both passes must be bracketed by add_pci_fixes()/undo_pci_fixes(),
 * which install temporary workarounds needed while enumerating.
 */
extern void pci_setup_tree(void);
extern void pci_reprogram(void);
extern void add_pci_fixes(void);
extern void undo_pci_fixes(void);

/*
 * Records a region of I/O port or memory space as consumed by an ISA device,
 * so that enumeration does not subsequently assign it to a PCI device.  The
 * first argument is a bus type in the sense of a 1275 regspec: 1 for I/O
 * ports, 0 for memory.  Called by the isa(4D) nexus as it enumerates.
 */
extern void pci_register_isa_resources(int, uint32_t, uint32_t);

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_PCI_BOOT_H */
