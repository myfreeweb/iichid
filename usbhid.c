/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * HID spec: https://www.usb.org/sites/default/files/documents/hid1_11.pdf
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/conf.h>
#include <sys/fcntl.h>

#include <dev/evdev/input.h>

#include "usbdevs.h"
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_ioctl.h>

#define	USB_DEBUG_VAR usbhid_debug
#include <dev/usb/usb_debug.h>

#include <dev/usb/quirk/usb_quirk.h>

#include "hid.h"
#include "hidbus.h"
#include "hid_if.h"

/* Set default probe priority lesser than other USB device drivers have */
#ifndef USBHID_BUS_PROBE_PRIO
#define	USBHID_BUS_PROBE_PRIO	(BUS_PROBE_GENERIC - 1)
#endif

#ifdef USB_DEBUG
static int usbhid_debug = 0;

static SYSCTL_NODE(_hw_usb, OID_AUTO, usbhid, CTLFLAG_RW, 0, "USB usbhid");
SYSCTL_INT(_hw_usb_usbhid, OID_AUTO, debug, CTLFLAG_RWTUN,
    &usbhid_debug, 0, "Debug level");
#endif

#define	USBHID_RSIZE		2048		/* bytes, max report size */
#define	USBHID_FRAME_NUM 	50		/* bytes, frame number */

enum {
	USBHID_INTR_DT_WR,
	USBHID_INTR_DT_RD,
	USBHID_CTRL_DT_WR,
	USBHID_CTRL_DT_RD,
	USBHID_N_TRANSFER,
};

/* Syncronous USB transfer context */
struct usbhid_xfer_ctx {
	struct usb_device_request *req;
	uint8_t *buf;
	uint16_t len;
	int error;
	int waiters;
	bool influx;
};

struct usbhid_softc {
	device_t sc_child;

	hid_intr_t *sc_intr_handler;
	void *sc_intr_context;
	struct mtx *sc_intr_mtx;
	void *sc_ibuf;

	struct hid_device_info sc_hw;

	struct usb_config sc_config[USBHID_N_TRANSFER];
	struct usb_xfer *sc_xfer[USBHID_N_TRANSFER];
	struct usb_device *sc_udev;

	uint8_t	sc_iface_no;
	uint8_t	sc_iface_index;

	struct usbhid_xfer_ctx sc_tr;
};

/* prototypes */

static device_probe_t usbhid_probe;
static device_attach_t usbhid_attach;
static device_detach_t usbhid_detach;

static usb_callback_t usbhid_intr_wr_callback;
static usb_callback_t usbhid_intr_rd_callback;
static usb_callback_t usbhid_ctrl_wr_callback;
static usb_callback_t usbhid_ctrl_rd_callback;

static void
usbhid_intr_wr_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_xfer_ctx *xfer_ctx = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		if (xfer_ctx->len > usbd_xfer_max_len(xfer)) {
			xfer_ctx->error = ENOBUFS;
			goto tr_exit;
		}
tr_setup:
		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_in(pc, 0, xfer_ctx->buf, xfer_ctx->len);
		usbd_xfer_set_frame_len(xfer, 0, xfer_ctx->len);
		usbd_transfer_submit(xfer);
		return;

	case USB_ST_TRANSFERRED:
		xfer_ctx->error = 0;
		goto tr_exit;

	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		xfer_ctx->error = EIO;
tr_exit:
		if (!HID_IN_POLLING_MODE_FUNC())
			wakeup(xfer_ctx);
		return;
	}
}

static void
usbhid_intr_rd_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	int actlen;

	usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		DPRINTF("transferred!\n");

		pc = usbd_xfer_get_frame(xfer, 0);

		/* limit report length to the maximum */
		if (actlen > (int)sc->sc_hw.rdsize)
			actlen = sc->sc_hw.rdsize;
		usbd_copy_out(pc, 0, sc->sc_ibuf, actlen);
		sc->sc_intr_handler(sc->sc_intr_context, sc->sc_ibuf, actlen);

	case USB_ST_SETUP:
re_submit:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		return;

	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto re_submit;
		}
		return;
	}
}

static void
usbhid_ctrl_wr_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_xfer_ctx *xfer_ctx = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		if (xfer_ctx->len > usbd_xfer_max_len(xfer)) {
			xfer_ctx->error = ENOBUFS;
			goto tr_exit;
		}

		if (xfer_ctx->len > 0) {
			pc = usbd_xfer_get_frame(xfer, 1);
			usbd_copy_in(pc, 0, xfer_ctx->buf, xfer_ctx->len);
			usbd_xfer_set_frame_len(xfer, 1, xfer_ctx->len);
		}

		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_in(pc, 0, xfer_ctx->req, sizeof(*xfer_ctx->req));
		usbd_xfer_set_frame_len(xfer, 0, sizeof(*xfer_ctx->req));

		usbd_xfer_set_frames(xfer, xfer_ctx->len > 0 ? 2 : 1);
		usbd_transfer_submit(xfer);
		return;

	case USB_ST_TRANSFERRED:
		xfer_ctx->error = 0;
		goto tr_exit;

	default:			/* Error */
		DPRINTFN(1, "error=%s\n", usbd_errstr(error));
		xfer_ctx->error = EIO;
tr_exit:
		if (!HID_IN_POLLING_MODE_FUNC())
			wakeup(xfer_ctx);
		return;
	}
}

static void
usbhid_ctrl_rd_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_xfer_ctx *xfer_ctx = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;

	pc = usbd_xfer_get_frame(xfer, 0);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		if (xfer_ctx->len > usbd_xfer_max_len(xfer)) {
			xfer_ctx->error = ENOBUFS;
			goto tr_exit;
		}

		usbd_copy_in(pc, 0, xfer_ctx->req, sizeof(*xfer_ctx->req));
		usbd_xfer_set_frame_len(xfer, 0, sizeof(*xfer_ctx->req));
		usbd_xfer_set_frame_len(xfer, 1, xfer_ctx->len);
		usbd_xfer_set_frames(xfer, xfer_ctx->len != 0 ? 2 : 1);
		usbd_transfer_submit(xfer);
		return;

	case USB_ST_TRANSFERRED:
		usbd_copy_out(pc, sizeof(*xfer_ctx->req), xfer_ctx->buf,
		    xfer_ctx->len);
		xfer_ctx->error = 0;
		goto tr_exit;

	default:			/* Error */
		/* bomb out */
		DPRINTFN(1, "error=%s\n", usbd_errstr(error));
		xfer_ctx->error = EIO;
tr_exit:
		if (!HID_IN_POLLING_MODE_FUNC())
			wakeup(xfer_ctx);
		return;
	}
}

static const struct usb_config usbhid_config[USBHID_N_TRANSFER] = {

	[USBHID_INTR_DT_WR] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.flags = {.pipe_bof = 1,.no_pipe_ok = 1,.proxy_buffer = 1},
		.callback = &usbhid_intr_wr_callback,
	},
	[USBHID_INTR_DT_RD] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.flags = {.pipe_bof = 1,.short_xfer_ok = 1,.proxy_buffer = 1},
		.callback = &usbhid_intr_rd_callback,
	},
	[USBHID_CTRL_DT_WR] = {
		.type = UE_CONTROL,
		.endpoint = 0x00,	/* Control pipe */
		.direction = UE_DIR_ANY,
		.flags = {.proxy_buffer = 1},
		.callback = &usbhid_ctrl_wr_callback,
		.timeout = 1000,	/* 1 second */
	},
	[USBHID_CTRL_DT_RD] = {
		.type = UE_CONTROL,
		.endpoint = 0x00,	/* Control pipe */
		.direction = UE_DIR_ANY,
		.flags = {.proxy_buffer = 1},
		.callback = &usbhid_ctrl_rd_callback,
		.timeout = 1000,	/* 1 second */
	},
};

static void
usbhid_intr_setup(device_t dev, struct mtx *mtx, hid_intr_t intr,
    void *context, uint16_t isize, uint16_t osize, uint16_t fsize)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	uint16_t n;
	int error;

	sc->sc_intr_handler = intr;
	sc->sc_intr_context = context;
	sc->sc_intr_mtx = mtx;
	bcopy(usbhid_config, sc->sc_config, sizeof(usbhid_config));

	/* Set buffer sizes to match HID report sizes */
	sc->sc_config[USBHID_INTR_DT_WR].bufsize = osize;
	sc->sc_config[USBHID_INTR_DT_RD].bufsize = isize;
	sc->sc_config[USBHID_CTRL_DT_WR].bufsize =
	    MAX(isize, MAX(osize, fsize));
	sc->sc_config[USBHID_CTRL_DT_RD].bufsize =
	    MAX(isize, MAX(osize, fsize));

	/*
	 * Setup the USB transfers one by one, so they are memory independent
	 * which allows for handling panics triggered by the HID drivers
	 * itself, typically by hkbd via CTRL+ALT+ESC sequences. Or if the HID
	 * keyboard driver was processing a key at the moment of panic.
	 */
	for (n = 0; n != USBHID_N_TRANSFER; n++) {
		error = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index,
		    sc->sc_xfer + n, sc->sc_config + n, 1,
		    n == USBHID_INTR_DT_RD ? (void *)sc : (void *)&sc->sc_tr,
		    sc->sc_intr_mtx);
		if (error)
			break;
	}

	if (error)
		DPRINTF("error=%s\n", usbd_errstr(error));

	sc->sc_hw.rdsize = usbd_xfer_max_len(sc->sc_xfer[USBHID_INTR_DT_RD]);
	sc->sc_hw.grsize = usbd_xfer_max_len(sc->sc_xfer[USBHID_CTRL_DT_RD]);
	sc->sc_hw.srsize = usbd_xfer_max_len(sc->sc_xfer[USBHID_CTRL_DT_WR]);
	sc->sc_hw.noWriteEp = sc->sc_xfer[USBHID_INTR_DT_WR] == NULL;
	sc->sc_hw.wrsize = sc->sc_hw.noWriteEp ? sc->sc_hw.srsize :
	    usbd_xfer_max_len(sc->sc_xfer[USBHID_INTR_DT_WR]);

	sc->sc_ibuf = malloc(sc->sc_hw.rdsize, M_USBDEV, M_ZERO | M_WAITOK);
}

static void
usbhid_intr_unsetup(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	usbd_transfer_unsetup(sc->sc_xfer, USBHID_N_TRANSFER);
	free(sc->sc_ibuf, M_USBDEV);
}

static int
usbhid_intr_start(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->sc_intr_mtx, MA_OWNED);

	usbd_transfer_start(sc->sc_xfer[USBHID_INTR_DT_RD]);

	return (0);
}

static int
usbhid_intr_stop(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->sc_intr_mtx, MA_OWNED);

	usbd_transfer_stop(sc->sc_xfer[USBHID_INTR_DT_RD]);

	return (0);
}

static void
usbhid_intr_poll(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	usbd_transfer_poll(sc->sc_xfer + USBHID_INTR_DT_RD, 1);
}

/*
 * HID interface
 */
static int
usbhid_sync_xfer(struct usbhid_softc* sc, int xfer_idx,
    struct usb_device_request *req, void *buf, uint16_t len)
{
	int error, timeout;
	struct usbhid_xfer_ctx save;

	if (HID_IN_POLLING_MODE_FUNC()) {
		save = sc->sc_tr;
	} else {
		mtx_lock(sc->sc_intr_mtx);
		++sc->sc_tr.waiters;
		while (sc->sc_tr.influx)
			mtx_sleep(&sc->sc_tr.waiters, sc->sc_intr_mtx, 0,
			    "usbhid wt", 0);
		--sc->sc_tr.waiters;
		sc->sc_tr.influx = true;
	}

	sc->sc_tr.buf = buf;
	sc->sc_tr.len = len;
	sc->sc_tr.req = req;
	sc->sc_tr.error = ETIMEDOUT;
	timeout = USB_DEFAULT_TIMEOUT;
	usbd_transfer_start(sc->sc_xfer[xfer_idx]);

	if (HID_IN_POLLING_MODE_FUNC())
		while (timeout > 0 && sc->sc_tr.error == ETIMEDOUT) {
			usbd_transfer_poll(sc->sc_xfer + xfer_idx, 1);
			DELAY(1000);
			timeout--;
                }
	 else
		msleep_sbt(&sc->sc_tr, sc->sc_intr_mtx, 0, "usbhid io",
		    SBT_1MS * timeout, 0, C_HARDCLOCK);

	usbd_transfer_stop(sc->sc_xfer[xfer_idx]);
	error = sc->sc_tr.error;

	if (HID_IN_POLLING_MODE_FUNC()) {
		sc->sc_tr = save;
	} else {
		sc->sc_tr.influx = false;
		if (sc->sc_tr.waiters != 0)
			wakeup_one(&sc->sc_tr.waiters);
		mtx_unlock(sc->sc_intr_mtx);
	}

	if (error)
		DPRINTF("USB IO error:%d\n", error);

	return (error);
}

static int
usbhid_get_report_desc(device_t dev, void *buf, uint16_t len)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int error;

	error = usbd_req_get_report_descriptor(sc->sc_udev, NULL,
	    buf, len, sc->sc_iface_index);

	if (error)
		DPRINTF("no report descriptor: %s\n", usbd_errstr(error));

	return (error == 0 ? 0 : ENXIO);
}

static int
usbhid_get_report(device_t dev, void *buf, uint16_t maxlen, uint16_t *actlen,
    uint8_t type, uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	struct usb_device_request req;
	int error;

	req.bmRequestType = UT_READ_CLASS_INTERFACE;
	req.bRequest = UR_GET_REPORT;
	USETW2(req.wValue, type, id);
	req.wIndex[0] = sc->sc_iface_no;
	req.wIndex[1] = 0;
	USETW(req.wLength, maxlen);

	error = usbhid_sync_xfer(sc, USBHID_CTRL_DT_RD, &req, buf, maxlen);
	if (!error && actlen != NULL)
		*actlen = maxlen;

	return (error);
}

static int
usbhid_set_report(device_t dev, void *buf, uint16_t len, uint8_t type,
    uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	struct usb_device_request req;

	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UR_SET_REPORT;
	USETW2(req.wValue, type, id);
	req.wIndex[0] = sc->sc_iface_no;
	req.wIndex[1] = 0;
	USETW(req.wLength, len);

	return (usbhid_sync_xfer(sc, USBHID_CTRL_DT_WR, &req, buf, len));
}

static int
usbhid_read(device_t dev, void *buf, uint16_t maxlen, uint16_t *actlen)
{

	return (ENOTSUP);
}

static int
usbhid_write(device_t dev, void *buf, uint16_t len)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	return (usbhid_sync_xfer(sc, USBHID_INTR_DT_WR, NULL, buf, len));
}

static int
usbhid_set_idle(device_t dev, uint16_t duration, uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	struct usb_device_request req;

	/* Duration is measured in 4 milliseconds per unit. */
	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UR_SET_IDLE;
	USETW2(req.wValue, (duration + 3) / 4, id);
	req.wIndex[0] = sc->sc_iface_no;
	req.wIndex[1] = 0;
	USETW(req.wLength, 0);

	return (usbhid_sync_xfer(sc, USBHID_CTRL_DT_WR, &req, NULL, 0));
}

static int
usbhid_set_protocol(device_t dev, uint16_t protocol)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	struct usb_device_request req;

	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UR_SET_PROTOCOL;
	USETW(req.wValue, protocol);
	req.wIndex[0] = sc->sc_iface_no;
	req.wIndex[1] = 0;
	USETW(req.wLength, 0);

	return (usbhid_sync_xfer(sc, USBHID_CTRL_DT_WR, &req, NULL, 0));
}

static const STRUCT_USB_HOST_ID usbhid_devs[] = {
	/* generic HID class */
	{USB_IFACE_CLASS(UICLASS_HID),},
	/* the Xbox 360 gamepad doesn't use the HID class */
	{USB_IFACE_CLASS(UICLASS_VENDOR),
	 USB_IFACE_SUBCLASS(UISUBCLASS_XBOX360_CONTROLLER),
	 USB_IFACE_PROTOCOL(UIPROTO_XBOX360_GAMEPAD),},
};

static int
usbhid_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	int error;

	DPRINTFN(11, "\n");

	if (uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	error = usbd_lookup_id_by_uaa(usbhid_devs, sizeof(usbhid_devs), uaa);
	if (error)
		return (error);

	if (usb_test_quirk(uaa, UQ_HID_IGNORE))
		return (ENXIO);

	return (USBHID_BUS_PROBE_PRIO);
}

static int
usbhid_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct usbhid_softc *sc = device_get_softc(dev);
	struct usb_hid_descriptor *hid;
	char *sep;
	int error = 0;

	DPRINTFN(10, "sc=%p\n", sc);

	device_set_usb_desc(dev);

	sc->sc_udev = uaa->device;

	sc->sc_iface_no = uaa->info.bIfaceNum;
	sc->sc_iface_index = uaa->info.bIfaceIndex;

	sc->sc_hw.parent = dev;
	strlcpy(sc->sc_hw.name, device_get_desc(dev), sizeof(sc->sc_hw.name));
	/* Strip extra parameters from device name created by usb_devinfo */
	sep = strchr(sc->sc_hw.name, ',');
	if (sep != NULL)
		*sep = '\0';
	strlcpy(sc->sc_hw.serial, usb_get_serial(uaa->device),
	    sizeof(sc->sc_hw.serial));
	sc->sc_hw.idBus = BUS_USB;
	sc->sc_hw.idVendor = uaa->info.idVendor;
	sc->sc_hw.idProduct = uaa->info.idProduct;
	sc->sc_hw.idVersion = 0;

	if (uaa->info.bInterfaceClass == UICLASS_HID &&
	    uaa->info.bInterfaceSubClass == UISUBCLASS_BOOT &&
	    uaa->info.bInterfaceProtocol == UIPROTO_BOOT_KEYBOARD)
		sc->sc_hw.pBootKbd = true;

	if (uaa->info.bInterfaceClass == UICLASS_HID &&
	    uaa->info.bInterfaceSubClass == UISUBCLASS_BOOT &&
	    uaa->info.bInterfaceProtocol == UIPROTO_MOUSE)
		sc->sc_hw.pBootMouse = true;

	/* Set quirks for devices which do not belong to HID class */
	if ((uaa->info.bInterfaceClass == UICLASS_VENDOR) &&
	    (uaa->info.bInterfaceSubClass == UISUBCLASS_XBOX360_CONTROLLER) &&
	    (uaa->info.bInterfaceProtocol == UIPROTO_XBOX360_GAMEPAD))
		sc->sc_hw.isXBox360GP = true;
	else if (uaa->info.bInterfaceClass == UICLASS_HID &&
		 uaa->iface != NULL && uaa->iface->idesc != NULL) {
		hid = hid_get_descriptor_from_usb(usbd_get_config_descriptor(
		    sc->sc_udev), uaa->iface->idesc);
		if (hid != NULL)
			sc->sc_hw.rdescsize =
			    UGETW(hid->descrs[0].wDescriptorLength);
	}

	error = usbd_req_set_idle(uaa->device, NULL,
	    uaa->info.bIfaceIndex, 0, 0);
	if (error) {
		DPRINTF("set idle failed, error=%s (ignored)\n",
		    usbd_errstr(error));
	}

	sc->sc_child = device_add_child(dev, "hidbus", -1);
	if (sc->sc_child == NULL) {
		device_printf(dev, "Could not add hidbus device\n");
		error = ENXIO;
		goto detach;
	}

	device_set_ivars(sc->sc_child, &sc->sc_hw);
	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: %d\n", error);

	return (0);			/* success */

detach:
	usbhid_detach(dev);
	return (ENOMEM);
}

static int
usbhid_detach(device_t dev)
{
	struct usbhid_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev))
		bus_generic_detach(dev);
	if (sc->sc_child)
		device_delete_child(dev, sc->sc_child);

	return (0);
}

static devclass_t usbhid_devclass;

static device_method_t usbhid_methods[] = {
	DEVMETHOD(device_probe, usbhid_probe),
	DEVMETHOD(device_attach, usbhid_attach),
	DEVMETHOD(device_detach, usbhid_detach),

	DEVMETHOD(hid_intr_setup,	usbhid_intr_setup),
	DEVMETHOD(hid_intr_unsetup,	usbhid_intr_unsetup),
	DEVMETHOD(hid_intr_start,	usbhid_intr_start),
	DEVMETHOD(hid_intr_stop,	usbhid_intr_stop),
	DEVMETHOD(hid_intr_poll,	usbhid_intr_poll),

	/* HID interface */
	DEVMETHOD(hid_get_report_descr,	usbhid_get_report_desc),
	DEVMETHOD(hid_read,		usbhid_read),
	DEVMETHOD(hid_write,		usbhid_write),
	DEVMETHOD(hid_get_report,	usbhid_get_report),
	DEVMETHOD(hid_set_report,	usbhid_set_report),
	DEVMETHOD(hid_set_idle,		usbhid_set_idle),
	DEVMETHOD(hid_set_protocol,	usbhid_set_protocol),

	DEVMETHOD_END
};

static driver_t usbhid_driver = {
	.name = "usbhid",
	.methods = usbhid_methods,
	.size = sizeof(struct usbhid_softc),
};

DRIVER_MODULE(usbhid, uhub, usbhid_driver, usbhid_devclass, NULL, 0);
MODULE_DEPEND(usbhid, usb, 1, 1, 1);
MODULE_DEPEND(usbhid, hid, 1, 1, 1);
MODULE_DEPEND(usbhid, hidbus, 1, 1, 1);
MODULE_VERSION(usbhid, 1);
USB_PNP_HOST_INFO(usbhid_devs);
