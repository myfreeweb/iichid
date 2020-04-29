/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2020 Greg V <greg@unrelenting.technology>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * XBox 360 gamepad driver thanks to the custom descriptor in usbhid.
 * 
 * Tested on: SVEN GC-5070 in both XInput (XBox 360) and DirectInput modes
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "usbdevs.h"
#include <dev/usb/input/usb_rdesc.h>

#include "hgame.h"
#include "hid.h"
#include "hidbus.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	xb360gp_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int xb360gp_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, xb360gp, CTLFLAG_RW, 0,
		"XBox360 gamepad");
SYSCTL_INT(_hw_hid_xb360gp, OID_AUTO, debug, CTLFLAG_RWTUN,
		&xb360gp_debug, 0, "Debug level");
#endif

static const uint8_t	xb360gp_rdesc[] = {UHID_XB360GP_REPORT_DESCR()};

#define XB360GP_MAP_BUT(number, code)	\
	{ HMAP_KEY(HUP_BUTTON, number, code) }
#define XB360GP_MAP_ABS(usage, code)	\
	{ HMAP_ABS(HUP_GENERIC_DESKTOP, HUG_##usage, code) }
#define XB360GP_MAP_CRG(usage_from, usage_to, callback)	\
	{ HMAP_ANY_CB_RANGE(HUP_GENERIC_DESKTOP,	\
	    HUG_##usage_from, HUG_##usage_to, callback) }
#define XB360GP_COMPLCB(cb)		\
	{ HMAP_COMPL_CB(&cb) }

/* Customized to match usbhid's XBox 360 descriptor */
static const struct hmap_item xb360gp_map[] = {
	XB360GP_MAP_BUT(1,		BTN_SOUTH),
	XB360GP_MAP_BUT(2,		BTN_EAST),
	XB360GP_MAP_BUT(3,		BTN_WEST),
	XB360GP_MAP_BUT(4,		BTN_NORTH),
	XB360GP_MAP_BUT(5,		BTN_TL),
	XB360GP_MAP_BUT(6,		BTN_TR),
	XB360GP_MAP_BUT(7,		BTN_SELECT),
	XB360GP_MAP_BUT(8,		BTN_START),
	XB360GP_MAP_BUT(9,		BTN_THUMBL),
	XB360GP_MAP_BUT(10,		BTN_THUMBR),
	XB360GP_MAP_BUT(11,		BTN_MODE),
	XB360GP_MAP_CRG(D_PAD_UP, D_PAD_LEFT, hgame_dpad_cb),
	XB360GP_MAP_ABS(X,		ABS_X),
	XB360GP_MAP_ABS(Y,		ABS_Y),
	XB360GP_MAP_ABS(Z,		ABS_Z),
	XB360GP_MAP_ABS(RX,		ABS_RX),
	XB360GP_MAP_ABS(RY,		ABS_RY),
	XB360GP_MAP_ABS(RZ,		ABS_RZ),
	XB360GP_COMPLCB(		hgame_compl_cb),
};

static const STRUCT_USB_HOST_ID xb360gp_devs[] = {
	/* the Xbox 360 gamepad doesn't use the HID class */
	{USB_IFACE_CLASS(UICLASS_VENDOR),
	 USB_IFACE_SUBCLASS(UISUBCLASS_XBOX360_CONTROLLER),
	 USB_IFACE_PROTOCOL(UIPROTO_XBOX360_GAMEPAD),},
};

static void
xb360gp_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);

	/* the Xbox 360 gamepad has no report descriptor */
	if (hw->isXBox360GP)
		hid_set_report_descr(parent, xb360gp_rdesc,
		    sizeof(xb360gp_rdesc));
}

static int
xb360gp_probe(device_t dev)
{
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int error;

	if (!hw->isXBox360GP)
		return (ENXIO);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	error = hmap_add_map(dev, xb360gp_map, nitems(xb360gp_map), NULL);
	if (error != 0)
		return (error);

	device_set_desc(dev, "XBox 360 Gamepad");

	return (BUS_PROBE_DEFAULT);
}

static int
xb360gp_attach(device_t dev)
{
	int error;

	/*
	 * Turn off the four LEDs on the gamepad which
	 * are blinking by default:
	 */
	static const uint8_t reportbuf[3] = {1, 3, 0};
	error = hid_set_report(dev, reportbuf, sizeof(reportbuf),
	    HID_OUTPUT_REPORT, 0);
	if (error)
		DPRINTF("set output report failed, error=%d "
		    "(ignored)\n", error);

	return (hmap_attach(dev));
}

static devclass_t xb360gp_devclass;

static device_method_t xb360gp_methods[] = {
	DEVMETHOD(device_identify,	xb360gp_identify),
	DEVMETHOD(device_probe,		xb360gp_probe),
	DEVMETHOD(device_attach,	xb360gp_attach),
	DEVMETHOD_END
};

DEFINE_CLASS_2(xb360gp, xb360gp_driver, xb360gp_methods,
    sizeof(struct hgame_softc), hgame_driver, hmap_driver);
DRIVER_MODULE(xb360gp, hidbus, xb360gp_driver, xb360gp_devclass, NULL, 0);
MODULE_DEPEND(xb360gp, hid, 1, 1, 1);
MODULE_DEPEND(xb360gp, hmap, 1, 1, 1);
MODULE_DEPEND(xb360gp, hgame, 1, 1, 1);
MODULE_DEPEND(xb360gp, evdev, 1, 1, 1);
MODULE_VERSION(xb360gp, 1);
USB_PNP_HOST_INFO(xb360gp_devs);
