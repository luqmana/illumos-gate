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
 * Copyright 2022 Oxide Computer Company
 */

#ifndef _SYS_IO_MILAN_IOMUX_H
#define	_SYS_IO_MILAN_IOMUX_H

/*
 * This file contains constants that are specific to the Milan implementation of
 * the I/O Mux.  That is, while <sys/amdzen/fch/iomux.h> describes the general
 * interface to the unit, the following definitions relate to what specific
 * alternate functions are and what the pins mean.
 *
 * This header should not generally have everything that exists in the I/O Mux.
 * That is what the general zen_data_sp3.c tables are for. Instead, this is here
 * to support early boot and general things we need to do before we have the
 * full I/O multiplexing driver existing and in a useable state.
 */

#include <sys/amdzen/mmioreg.h>
#include <sys/amdzen/fch.h>
#include <sys/amdzen/fch/iomux.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is a convenience macro for setting the function for a particular pin
 * using MMIO.  It uses the function values defined below and will fail to
 * compile if a definition for the requested function and pin does not exist.
 */
#define	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(b, r, f)	\
	mmio_reg_write(FCH_IOMUX_IOMUX_MMIO(b, r),	\
	MILAN_FCH_IOMUX_ ## r ## _ ## f)

/*
 * Pinmux function values.
 */

/*
 * Documentation is inconsistent with respect to the names of the GPIO functions
 * associated with this pin: EGPIO_26 == EGPIO26_0 and EGPIO27 == EGPIO26_3.
 */
#define	MILAN_FCH_IOMUX_26_PCIE_RST0_L	0
#define	MILAN_FCH_IOMUX_26_EGPIO26	1
#define	MILAN_FCH_IOMUX_26_EGPIO26_0	1
#define	MILAN_FCH_IOMUX_27_PCIE_RST3_L	0
#define	MILAN_FCH_IOMUX_27_EGPIO27	1
#define	MILAN_FCH_IOMUX_27_EGPIO26_3	1

#define	MILAN_FCH_IOMUX_129_KBRST_L		0
#define	MILAN_FCH_IOMUX_129_GPIO129		2

#define	MILAN_FCH_IOMUX_135_UART0_CTS_L	0
#define	MILAN_FCH_IOMUX_136_UART0_RXD		0
#define	MILAN_FCH_IOMUX_137_UART0_RTS_L	0
#define	MILAN_FCH_IOMUX_138_UART0_TXD		0
#define	MILAN_FCH_IOMUX_139_GPIO139		1

#define	MILAN_FCH_IOMUX_140_UART1_CTS_L	0
#define	MILAN_FCH_IOMUX_141_UART1_RXD		0
#define	MILAN_FCH_IOMUX_142_UART1_RTS_L	0
#define	MILAN_FCH_IOMUX_143_UART1_TXD		0
#define	MILAN_FCH_IOMUX_144_GPIO144		1

#define	MILAN_FCH_RMTMUX_10_PCIE_RST1_L		0
#define	MILAN_FCH_RMTMUX_10_EGPIO26_1		1
#define	MILAN_FCH_RMTMUX_11_PCIE_RST2_L		0
#define	MILAN_FCH_RMTMUX_11_EGPIO26_2		1

/*
 * The default at-reset mappings for IOMUX pins relating to UARTs on Milan
 * according to the PPRs are shown below.
 *
 *	0x87 - GPIO135		[UART0_CTS_L]
 *	0x88 - UART0_RXD	[UART0_RXD]
 *	0x89 - GPIO_137		[UART0_RTS_L]
 *	0x8a - GPIO_138		[UART0_TXD]
 *
 *	0x8c - GPIO_140		[UART1_CTS_L]
 *	0x8d - UART1_RXD	[UART1_RXD]
 *	0x8e - GPIO_142		[UART1_RTS_L]
 *	0x8f - GPIO_143		[UART1_TXD]
 */
static inline void
milan_uart_iomux_pinmux_reset(void)
{
	mmio_reg_block_t block = fch_iomux_mmio_block();

	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 135, UART0_CTS_L);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 136, UART0_RXD);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 137, UART0_RTS_L);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 138, UART0_TXD);

	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 140, UART1_CTS_L);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 141, UART1_RXD);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 142, UART1_RTS_L);
	MILAN_FCH_IOMUX_PINMUX_SET_MMIO(block, 143, UART1_TXD);

	mmio_reg_block_unmap(&block);
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_IOMUX_H */
