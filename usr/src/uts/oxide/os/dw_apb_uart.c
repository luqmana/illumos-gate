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
 * Copyright 2023 Oxide Computer Co.
 */

/*
 * An early boot driver for the Synopsis DesignWare Advanced Peripheral Bus
 * UARTs found in the FCH ("Fusion Controller Hub") on AMD SoCs.
 *
 * Only the first two UARTs are supported and they are always configured to use
 * automatic flow control. Enabling additional UARTs would consume the flow
 * control pins used for the first two.
 *
 * There is no locking here. In general consumers are expected to use this
 * interface while the machine is still single-threaded or to manage locking
 * themselves. If two consumers access the same UART through this driver the
 * results are undefined.
 *
 * Initialising a UART via dw_apb_uart_init() will create physical-to-virtual
 * address mappings for the UART register area. If the UART is initialised
 * early in boot then the virtual address space will be allocated from the
 * earlyboot arena and the mappings will be destroyed in startup.c when it
 * calls clear_boot_mappings(). Consumers who intend to use the UART beyond
 * that point will need to call dw_apb_uart_init() again, after the device
 * arena is set up, in order to obtain new mappings.
 */

#include <sys/cmn_err.h>
#include <sys/prom_debug.h>
#include <sys/bootconf.h>
#include <sys/dw_apb_uart.h>
#include <sys/stdbool.h>
#include <sys/types.h>
#include <sys/uart.h>
#include <sys/amdzen/fch.h>
#include <sys/amdzen/fch/iomux.h>
#include <sys/io/fch/uart.h>
#include <sys/io/genoa/iomux.h>
#include <sys/io/milan/iomux.h>
#include <sys/x86_archext.h>
#include <vm/kboot_mmu.h>

/*
 * Comes from uts/oxide/os/fakebop.c.  We're too early to use the cpuid_*
 * interfaces to grab the processor family to determine the FCH type.
 */
extern x86_chiprev_t early_chiprev;

int dw_apb_invalid_disable_intr = 0;

static int
dw_apb_lcr(uint8_t *lcrp, const async_databits_t db, const async_parity_t par,
    const async_stopbits_t sb)
{
	uint8_t lcr = 0;

	switch (sb) {
	case AS_1BIT:
		break;
	case AS_15BITS:
		if (db != AD_5BITS)
			return (-1);
		lcr = FCH_UART_LCR_SET_STOP(lcr, 1);
		break;
	case AS_2BITS:
		if (db == AD_5BITS)
			return (-1);
		lcr = FCH_UART_LCR_SET_STOP(lcr, 1);
		break;
	default:
		return (-1);
	}

	switch (db) {
	case AD_5BITS:
		lcr = FCH_UART_LCR_SET_DLS(lcr, FCH_UART_LCR_DLS_5BIT);
		break;
	case AD_6BITS:
		lcr = FCH_UART_LCR_SET_DLS(lcr, FCH_UART_LCR_DLS_6BIT);
		break;
	case AD_7BITS:
		lcr = FCH_UART_LCR_SET_DLS(lcr, FCH_UART_LCR_DLS_7BIT);
		break;
	case AD_8BITS:
		lcr = FCH_UART_LCR_SET_DLS(lcr, FCH_UART_LCR_DLS_8BIT);
		break;
	default:
		return (-1);
	}

	switch (par) {
	case AP_NONE:
		break;
	case AP_SPACE:
		lcr = FCH_UART_LCR_SET_SP(lcr, 1);
		/* FALLTHROUGH */
	case AP_EVEN:
		lcr = FCH_UART_LCR_SET_EPS(lcr, 1);
		lcr = FCH_UART_LCR_SET_PEN(lcr, 1);
		break;
	case AP_MARK:
		lcr = FCH_UART_LCR_SET_SP(lcr, 1);
		/* FALLTHROUGH */
	case AP_ODD:
		lcr = FCH_UART_LCR_SET_PEN(lcr, 1);
		break;
	default:
		return (-1);
	}

	*lcrp = lcr;
	return (0);
}

/*
 * By the time we get here it is possible that some of the UART pins have been
 * remapped by the ABL based on the APCB contents. We explicitly set each pin to
 * the function we need.
 */
static void
dw_apb_uart_iomux_pinmux_set(void)
{
	static bool mapped = false;

	if (mapped)
		return;

	switch (chiprev_family(early_chiprev)) {
	case X86_PF_AMD_ROME:
	case X86_PF_AMD_MILAN:
		milan_uart_iomux_pinmux_reset();
		break;
	case X86_PF_AMD_GENOA:
	case X86_PF_AMD_BERGAMO:
		genoa_uart_iomux_pinmux_reset();
		break;
	default:
		bop_panic("dw_apb_uart_iomux_pinmux_set: unsupported proc family\n");
	}

	mapped = true;
}

int
dw_apb_uart_init(dw_apb_uart_t * const uart, const dw_apb_port_t port,
    const uint32_t baud, const async_databits_t db,
    const async_parity_t par, const async_stopbits_t sb)
{
	uint8_t unit;

	switch (port) {
	case DAP_0:
		unit = 0;
		break;
	case DAP_1:
		unit = 1;
		break;
	/*
	 * UARTs 2 & 3 are not currently supported. Their use would consume the
	 * flow control pins for 0 & 1, and Songshan does not have UART 3.
	 */
	case DAP_2:
	case DAP_3:
	default:
		return (-1);
	}

	dw_apb_uart_iomux_pinmux_set();

	if ((uart->dau_flags & DAUF_MAPPED))
		mmio_reg_block_unmap(&uart->dau_reg_block);

	switch (chiprev_fch_kind(early_chiprev)) {
	case FK_HUASHAN:
		uart->dau_reg_block = huashan_uart_mmio_block(unit);
		break;
	case FK_SONGSHAN:
		uart->dau_reg_block = songshan_uart_mmio_block(unit);
		break;
	default:
		bop_panic("dw_apb_uart_init: unsupported FCH kind\n");
	}

	uart->dau_reg_thr = FCH_UART_THR_MMIO(uart->dau_reg_block);
	uart->dau_reg_rbr = FCH_UART_RBR_MMIO(uart->dau_reg_block);
	uart->dau_reg_lsr = FCH_UART_LSR_MMIO(uart->dau_reg_block);
	uart->dau_reg_usr = FCH_UART_USR_MMIO(uart->dau_reg_block);
	uart->dau_reg_srr = FCH_UART_SRR_MMIO(uart->dau_reg_block);
	uart->dau_reg_mcr = FCH_UART_MCR_MMIO(uart->dau_reg_block);
	uart->dau_reg_ier = FCH_UART_IER_MMIO(uart->dau_reg_block);
	uart->dau_reg_lcr = FCH_UART_LCR_MMIO(uart->dau_reg_block);

	uart->dau_port = port;
	uart->dau_flags |= DAUF_MAPPED;

	if (!(uart->dau_flags & DAUF_INITDONE) ||
	    baud != uart->dau_baudrate || db != uart->dau_databits ||
	    par != uart->dau_parity || sb != uart->dau_stopbits) {

		const mmio_reg_t r_dlh = FCH_UART_DLH_MMIO(uart->dau_reg_block);
		const mmio_reg_t r_dll = FCH_UART_DLL_MMIO(uart->dau_reg_block);
		const mmio_reg_t r_fcr = FCH_UART_FCR_MMIO(uart->dau_reg_block);

		/*
		 * XXX We should really get our clock from whatever controls
		 * it.  We may also want to do something sensible if the baud
		 * rate is inexact or unsatisfiable.
		 */
		const uint32_t divisor = 3000000 / baud;
		const uint8_t dlh = (divisor & 0xff00) >> 8;
		const uint8_t dll = (divisor & 0x00ff);
		const uint8_t lcr_dlab = FCH_UART_LCR_SET_DLAB(0, 1);
		uint8_t lcr;

		if (dw_apb_lcr(&lcr, db, par, sb) != 0)
			return (-1);

		uint8_t fcr = 0;
		fcr = FCH_UART_FCR_SET_RT(fcr, FCH_UART_FCR_RT_QUARTER);
		fcr = FCH_UART_FCR_SET_TET(fcr, FCH_UART_FCR_TET_QUARTER);
		fcr = FCH_UART_FCR_SET_DMAM(fcr, 1);
		fcr = FCH_UART_FCR_SET_RFIFOR(fcr, 1);
		fcr = FCH_UART_FCR_SET_XFIFOR(fcr, 1);
		fcr = FCH_UART_FCR_SET_FIFOE(fcr, 1);

		uint8_t mcr = 0;
		mcr = FCH_UART_MCR_SET_DTR(mcr, 1);
		mcr = FCH_UART_MCR_SET_RTS(mcr, 1);
		mcr = FCH_UART_MCR_SET_OUT2(mcr, 1);
		mcr = FCH_UART_MCR_SET_AFCE(mcr, 1);
		/* Stash so it can be restored later via dw_apb_reset_mcr() */
		uart->dau_mcr = mcr;

		uint32_t srr = 0;
		srr = FCH_UART_SRR_SET_XFR(srr, 1);
		srr = FCH_UART_SRR_SET_RFR(srr, 1);
		srr = FCH_UART_SRR_SET_UR(srr, 1);

		mmio_reg_write(uart->dau_reg_srr, srr);
		/* Allow dlh/dll write */
		mmio_reg_write(uart->dau_reg_lcr, lcr_dlab);
		mmio_reg_write(r_dlh, dlh);
		mmio_reg_write(r_dll, dll);
		mmio_reg_write(uart->dau_reg_lcr, lcr);
		mmio_reg_write(r_fcr, fcr);
		mmio_reg_write(uart->dau_reg_mcr, mcr);

		uart->dau_flags |= DAUF_INITDONE;
		uart->dau_baudrate = baud;
		uart->dau_databits = db;
		uart->dau_parity = par;
		uart->dau_stopbits = sb;
	}

	return (0);
}

inline bool
dw_apb_uart_readable(const dw_apb_uart_t * const uart)
{
	const uint32_t lsr = mmio_reg_read(uart->dau_reg_lsr);
	/* Data Ready */
	return (FCH_UART_LSR_GET_DR(lsr) != 0);
}

size_t
dw_apb_uart_rx_nb(const dw_apb_uart_t * const uart, uint8_t *dbuf, size_t len)
{
	size_t i = 0;

	if (dbuf == NULL)
		return (0);

	while (i < len && dw_apb_uart_readable(uart))
		dbuf[i++] = mmio_reg_read(uart->dau_reg_rbr);

	return (i);
}

uint8_t
dw_apb_uart_rx_one(const dw_apb_uart_t * const uart)
{
	uint8_t ch;

	while (dw_apb_uart_rx_nb(uart, &ch, 1) < 1)
		;

	return (ch);
}

bool
dw_apb_uart_writable(const dw_apb_uart_t * const uart)
{
	const uint32_t usr = mmio_reg_read(uart->dau_reg_usr);
	/* Transmit FIFO Not Full */
	return (FCH_UART_USR_GET_TFNF(usr) != 0);
}

size_t
dw_apb_uart_tx_nb(const dw_apb_uart_t * const uart, const uint8_t *dbuf,
    size_t len)
{
	size_t i = 0;

	while (len > 0 && dw_apb_uart_writable(uart)) {
		mmio_reg_write(uart->dau_reg_thr, dbuf[i]);
		i++;
		len--;
	}

	return (i);
}

void
dw_apb_uart_tx(const dw_apb_uart_t * const uart, const uint8_t *dbuf,
    size_t len)
{
	while (len > 0) {
		size_t sent = dw_apb_uart_tx_nb(uart, dbuf, len);
		dbuf += sent;
		len -= sent;
	}
}

void
dw_apb_uart_flush(const dw_apb_uart_t * const uart)
{
	uint32_t v = 0;

	v = FCH_UART_SRR_SET_XFR(v, 1);
	v = FCH_UART_SRR_SET_RFR(v, 1);
	mmio_reg_write(uart->dau_reg_srr, v);
}

void
dw_apb_reset_mcr(const dw_apb_uart_t * const uart)
{
	mmio_reg_write(uart->dau_reg_mcr, uart->dau_mcr);
}

void
dw_apb_disable_intr(const dw_apb_uart_t * const uart)
{
	uint32_t lcr = mmio_reg_read(uart->dau_reg_lcr);

	/*
	 * If LCR[DLAB] is set, the UART cannot be operating.  This is
	 * unexpected, but we also know it can't be generating interrupts in
	 * this state.  We can't access the IER and we don't want to enable the
	 * UART here by clearing DLAB, so we will do nothing.  The consumer is
	 * probably confused and nothing good is going to happen, but we aren't
	 * in a place to do much about it other than record this event should
	 * anyone be able to get a crash dump or working debugger (unlikely).
	 */
	if (FCH_UART_LCR_GET_DLAB(lcr) != 0) {
		++dw_apb_invalid_disable_intr;
		return;
	}

	mmio_reg_write(uart->dau_reg_ier, 0);
}
