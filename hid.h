/* $FreeBSD$ */
/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 * Copyright (c) 1998 The NetBSD Foundation, Inc. All rights reserved.
 * Copyright (c) 1998 Lennart Augustsson. All rights reserved.
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

#ifndef _HID_H_
#define	_HID_H_

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbhid.h>

/* Declare parent SYSCTL USB node. */
#ifdef SYSCTL_DECL
SYSCTL_DECL(_hw_hid);
#endif

#ifndef HUG_D_PAD_UP
#define	HUG_D_PAD_UP	0x90
#define	HUG_D_PAD_DOWN	0x91
#define	HUG_D_PAD_RIGHT	0x92
#define	HUG_D_PAD_LEFT	0x93
#endif

#define	HID_INPUT_REPORT	0x1
#define	HID_OUTPUT_REPORT	0x2
#define	HID_FEATURE_REPORT	0x3

#define	HID_IN_POLLING_MODE_FUNC() hid_in_polling_mode()
#define	HID_IN_POLLING_MODE_VALUE() (SCHEDULER_STOPPED() || kdb_active)

/*
 * Walk through all HID items hi belonging Top Level Collection #tidx
 */
#define HID_TLC_FOREACH_ITEM(hd, hi, tidx)				\
	for (uint8_t _iter_##tidx = 0; hid_get_item((hd), (hi));)	\
		if ((_iter_##tidx +=					\
		    (((hi)->kind == hid_endcollection &&		\
		      (hi)->collevel == 0) ? 1 : 0)) == (tidx))

typedef usb_size_t hid_size_t;

struct hid_absinfo {
	int32_t min;
	int32_t max;
	int32_t res;
};

typedef void hid_intr_t(void *context, void *data, hid_size_t len);

struct hid_device_info {
	device_t	parent;
	char		name[80];
	char		serial[80];
	uint16_t	idBus;
	uint16_t	idVendor;
	uint16_t	idProduct;
	uint16_t	idVersion;
	uint16_t	rdescsize;	/* Report descriptor size */
	/* Quirks */
	bool		isXBox360GP;	/* XBox360 Game Pad */
	bool		noWriteEp;	/* Do writes through CTRL endpoint */
	bool		pBootKbd;	/* Support Keyboard Boot Protocol */
	bool		pBootMouse;	/* Support Mouse Boot Protocol */
};

/* OpenBSD/NetBSD compat shim */
#define	HID_GET_USAGE(u) ((u) & 0xffff)
#define	HID_GET_USAGE_PAGE(u) (((u) >> 16) & 0xffff)

static __inline uint32_t
hid_get_udata(const uint8_t *buf, hid_size_t len, struct hid_location *loc)
{
	return (hid_get_data_unsigned(buf, len, loc));
}

/*
 * hid_report_size_1 is a port of userland hid_report_size() from usbhid(3)
 * to kernel. XXX: to be renamed back to hid_report_size()
 */
int	hid_report_size_1(const void *buf, hid_size_t len, enum hid_kind k,
	    uint8_t id);
int	hid_tlc_locate(const void *desc, hid_size_t size, int32_t u,
	    enum hid_kind k, uint8_t tlc_index, uint8_t index,
	    struct hid_location *loc, uint32_t *flags, uint8_t *id,
	    struct hid_absinfo *ai);
int	hid_in_polling_mode(void);

#endif					/* _HID_H_ */
