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
 * Copyright (c) 2012 Gary Mills
 * Copyright 2026 Oxide Computer Co.
 *
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/debug.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/esunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/promif.h>
#include <sys/modctl.h>
#include <sys/io/zen/fch.h>

int
plat_use_polled_debug()
{
	return (0);
}

int
plat_support_serial_kbd_and_ms()
{
	return (0);
}

int
plat_stdin_is_keyboard(void)
{
	return (0);
}

int
plat_stdout_is_framebuffer(void)
{
	return (0);
}

char *
plat_fbpath(void)
{
	return (NULL);
}

char *
plat_mousepath(void)
{
	return (NULL);
}

char *
plat_kbdpath(void)
{
	return (NULL);
}

/*
 * The path returned by plat_conspath() is expected to be of static
 * (or otherwise unbounded) lifetime, so we store it in a static buffer.  The
 * contents are deterministically derived from the fabric topology and are
 * filled on the first call, made from the single-threaded console configuration
 * during boot, and only ever read thereafter.
 */
static char plat_console_path[TYPICALMAXPATHLEN];

static char *
plat_conspath(void)
{
	size_t sz;

	if (plat_console_path[0] != '\0')
		return (plat_console_path);

	sz = fch_dev_path(plat_console_path, sizeof (plat_console_path));
	if (sz == 0 || sz >= sizeof (plat_console_path))
		panic("plat_conspath: no usable primary FCH device path");

	sz = strlcat(plat_console_path, "/dwu@0:0", sizeof (plat_console_path));
	if (sz >= sizeof (plat_console_path))
		panic("plat_conspath: console path too long");

	return (plat_console_path);
}

char *
plat_stdinpath(void)
{
	return (plat_conspath());
}

char *
plat_stdoutpath(void)
{
	return (plat_conspath());
}

char *
plat_diagpath(void)
{
	return (plat_conspath());
}
