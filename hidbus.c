/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
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

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/systm.h>

#include "hid.h"
#include "hidbus.h"

#include "hid_if.h"

#define	HID_DEBUG_VAR	hid_debug
#include "hid_debug.h"

static hid_intr_t	hidbus_intr;

static device_probe_t	hidbus_probe;
static device_attach_t	hidbus_attach;
static device_detach_t	hidbus_detach;

struct hidbus_softc {
	device_t			dev;
	struct mtx			lock;

	STAILQ_HEAD(, hidbus_ivar)	tlcs;
};

static device_t
hidbus_add_child(device_t dev, u_int order, const char *name, int unit)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	struct hidbus_ivar *tlc;
	device_t child;

	child = device_add_child_ordered(dev, order, name, unit);
	if (child == NULL)
			return (child);

	tlc = malloc(sizeof(struct hidbus_ivar), M_DEVBUF, M_WAITOK | M_ZERO);
	tlc->child = child;
	STAILQ_INSERT_TAIL(&sc->tlcs, tlc, link);
	device_set_ivars(child, tlc);

	return (child);
}

static int
hidbus_enumerate_children(device_t dev)
{
	struct hid_data *hd;
	struct hid_item hi;
	device_t child;
	void *data;
	uint16_t len;
	uint8_t index = 0;

	if (HID_GET_REPORT_DESCR(device_get_parent(dev), &data, &len) != 0)
		return (ENXIO);

	/* Add a child for each top level collection */
	hd = hid_start_parse(data, len, 1 << hid_input);
	while (hid_get_item(hd, &hi)) {
		if (hi.kind != hid_collection || hi.collevel != 1)
			continue;
		child = BUS_ADD_CHILD(dev, 0, NULL, -1);
		if (child == NULL) {
			device_printf(dev, "Could not add HID device\n");
			continue;
		}
		hidbus_set_index(child, index);
		hidbus_set_usage(child, hi.usage);
		index++;
		DPRINTF("Add child TLC: 0x%04hx:0x%04hx\n",
		    (uint16_t)(hi.usage >> 16), (uint16_t)(hi.usage & 0xFFFF));
	}
	hid_end_parse(hd);

	if (index == 0)
		return (ENXIO);

	return (0);
}

static int
hidbus_probe(device_t dev)
{

	device_set_desc(dev, "HID bus");

	/* Allow other subclasses to override this driver. */
	return (BUS_PROBE_GENERIC);
}

static int
hidbus_attach(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	int error;

	sc->dev = dev;
	STAILQ_INIT(&sc->tlcs);

	mtx_init(&sc->lock, "hidbus lock", NULL, MTX_DEF);
	HID_INTR_SETUP(device_get_parent(dev), &sc->lock, hidbus_intr, sc);

	error = hidbus_enumerate_children(dev);
	if (error != 0) {
		hidbus_detach(dev);
		return (ENXIO);
	}

	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: error %d\n", error);

	return (0);
}

static int
hidbus_detach(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	struct hidbus_ivar *tlc;

	bus_generic_detach(dev);
	device_delete_children(dev);

	HID_INTR_UNSETUP(device_get_parent(dev));
	mtx_destroy(&sc->lock);

	while (!STAILQ_EMPTY(&sc->tlcs)) {
		tlc = STAILQ_FIRST(&sc->tlcs);
		STAILQ_REMOVE_HEAD(&sc->tlcs, link);
                free(tlc, M_DEVBUF);
        }

	return (0);
}

static int
hidbus_read_ivar(device_t bus, device_t child, int which, uintptr_t *result)
{
	struct hidbus_ivar *info = device_get_ivars(child);

	switch (which) {
	case HIDBUS_IVAR_INDEX:
		*result = info->index;
		break;
	case HIDBUS_IVAR_USAGE:
		*result = info->usage;
		break;
	case HIDBUS_IVAR_DEVINFO:
		*result = (uintptr_t)device_get_ivars(bus);
		break;
	default:
		return (EINVAL);
	}
        return (0);
}

static int
hidbus_write_ivar(device_t bus, device_t child, int which, uintptr_t value)
{
	struct hidbus_ivar *info = device_get_ivars(child);

	switch (which) {
	case HIDBUS_IVAR_INDEX:
		info->index = value;
		break;
	case HIDBUS_IVAR_USAGE:
		info->usage = value;
		break;
	case HIDBUS_IVAR_DEVINFO:
	default:
		return (EINVAL);
	}
        return (0);
}

/* Location hint for devctl(8) */
static int
hidbus_child_location_str(device_t bus, device_t child, char *buf,
    size_t buflen)
{
	struct hidbus_ivar *info = device_get_ivars(child);

	snprintf(buf, buflen, "index=%hhu", info->index);
        return (0);
}

/* PnP information for devctl(8) */
static int
hidbus_child_pnpinfo_str(device_t bus, device_t child, char *buf,
    size_t buflen)
{
	struct hidbus_ivar *devinfo = device_get_ivars(child);
	struct hid_device_info *businfo = device_get_ivars(bus);

	snprintf(buf, buflen, "page=0x%04x usage=0x%04x bus=0x%02hx "
	    "vendor=0x%04hx product=0x%04hx version=0x%04hx",
	    devinfo->usage >> 16, devinfo->usage & 0xFFFF, businfo->idBus,
	    businfo->idVendor, businfo->idProduct, businfo->idVersion);
	return (0);
}

device_t
hidbus_find_child(device_t bus, uint32_t usage)
{
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivar *tlc;

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->usage == usage)
			return (tlc->child);
	}

	return (NULL);
}

struct mtx *
hid_get_lock(device_t child)
{
	struct hidbus_softc *sc = device_get_softc(device_get_parent(child));

	return (&sc->lock);
}

void
hid_set_intr(device_t child, hid_intr_t intr)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivar *tlc;

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->child == child)
			tlc->intr = intr;
	}
}

void
hidbus_intr(void *context, void *buf, uint16_t len)
{
	struct hidbus_softc *sc = context;
	struct hidbus_ivar *tlc;

	mtx_assert(&sc->lock, MA_OWNED);

	/*
	 * Broadcast input report to all subscribers.
	 * TODO: Add check for input report ID.
	 */
	 STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->open) {
			KASSERT(tlc->intr != NULL,
			    ("hidbus: interrupt handler is NULL"));
			tlc->intr(tlc->child, buf, len);
		}
	}
}

int
hid_start(device_t child)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivar *tlc;
	bool open = false;

	mtx_assert(&sc->lock, MA_OWNED);

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		open = open || tlc->open;
		if (tlc->child == child)
			tlc->open = true;
	}

	if (open)
		return (0);

	return (HID_INTR_START(device_get_parent(bus)));
}

int
hid_stop(device_t child)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivar *tlc;
	bool open = false;

	mtx_assert(&sc->lock, MA_OWNED);

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->child == child)
			tlc->open = false;
		open = open || tlc->open;
	}

	if (open)
		return (0);

	return (HID_INTR_STOP(device_get_parent(bus)));
}

/*
 * HID interface
 */
int
hid_get_report_descr(device_t bus, void **data, uint16_t *len)
{

	return (HID_GET_REPORT_DESCR(device_get_parent(bus), data, len));
}

int
hid_get_input_report(device_t bus, void *data, uint16_t len)
{

	return (HID_GET_INPUT_REPORT(device_get_parent(bus), data, len));
}

int
hid_set_output_report(device_t bus, void *data, uint16_t len)
{

	return (HID_SET_OUTPUT_REPORT(device_get_parent(bus), data, len));
}

int
hid_get_report(device_t bus, void *data, uint16_t len, uint8_t type,
    uint8_t id)
{

	return (HID_GET_REPORT(device_get_parent(bus), data, len, type, id));
}

int
hid_set_report(device_t bus, void *data, uint16_t len, uint8_t type,
    uint8_t id)
{

	return (HID_SET_REPORT(device_get_parent(bus), data, len, type, id));
}

int
hid_set_idle(device_t bus, uint16_t duration, uint8_t id)
{

	return (HID_SET_IDLE(device_get_parent(bus), duration, id));
}

int
hid_set_protocol(device_t bus, uint16_t protocol)
{

	return (HID_SET_PROTOCOL(device_get_parent(bus), protocol));
}

static device_method_t hidbus_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe,         hidbus_probe),
	DEVMETHOD(device_attach,        hidbus_attach),
	DEVMETHOD(device_detach,        hidbus_detach),
	DEVMETHOD(device_suspend,       bus_generic_suspend),
	DEVMETHOD(device_resume,        bus_generic_resume),

	/* bus interface */
	DEVMETHOD(bus_add_child,	hidbus_add_child),
	DEVMETHOD(bus_read_ivar,	hidbus_read_ivar),
	DEVMETHOD(bus_write_ivar,	hidbus_write_ivar),
	DEVMETHOD(bus_child_pnpinfo_str,hidbus_child_pnpinfo_str),
	DEVMETHOD(bus_child_location_str,hidbus_child_location_str),

	/* hid interface */
	DEVMETHOD(hid_get_report_descr,	hid_get_report_descr),
	DEVMETHOD(hid_get_input_report,	hid_get_input_report),
	DEVMETHOD(hid_set_output_report,hid_set_output_report),
	DEVMETHOD(hid_get_report,       hid_get_report),
	DEVMETHOD(hid_set_report,       hid_set_report),
	DEVMETHOD(hid_set_idle,		hid_set_idle),
	DEVMETHOD(hid_set_protocol,	hid_set_protocol),

        DEVMETHOD_END
};


driver_t hidbus_driver = {
	"hidbus",
	hidbus_methods,
	sizeof(struct hidbus_softc),
};

devclass_t hidbus_devclass;

MODULE_VERSION(hidbus, 1);
DRIVER_MODULE(hidbus, usbhid, hidbus_driver, hidbus_devclass, 0, 0);
DRIVER_MODULE(hidbus, iichid, hidbus_driver, hidbus_devclass, 0, 0);
