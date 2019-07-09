/*-
 * Copyright (c) 2018-2019 Marc Priggemeyer <marc.priggemeyer@gmail.com>
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
#include <sys/callout.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <dev/acpica/acpivar.h>

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iic.h>
#include <dev/iicbus/iiconf.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbhid.h>

#include "iichid.h"

static ACPI_STATUS
iichid_res_walk_cb(ACPI_RESOURCE *res, void *context)
{
	struct iichid_hw *hw = context;

	switch(res->Type) {
	case ACPI_RESOURCE_TYPE_SERIAL_BUS:
		if (res->Data.CommonSerialBus.Type !=
		    ACPI_RESOURCE_SERIAL_TYPE_I2C) {
			device_printf(hw->acpi_dev,
			    "wrong bus type, should be %d is %d\n",
			    ACPI_RESOURCE_SERIAL_TYPE_I2C,
			    res->Data.CommonSerialBus.Type);
			return (AE_TYPE);
		} else {
			hw->device_addr =
			    le16toh(res->Data.I2cSerialBus.SlaveAddress);
		}
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		if (res->Data.ExtendedIrq.InterruptCount > 0) {
			hw->irq = res->Data.ExtendedIrq.Interrupts[0];
		}
		break;
	case ACPI_RESOURCE_TYPE_GPIO:
		if (res->Data.Gpio.ConnectionType ==
		    ACPI_RESOURCE_GPIO_TYPE_INT) {
			hw->gpio_pin = res->Data.Gpio.PinTable[0];
		}
		break;
	case ACPI_RESOURCE_TYPE_END_TAG:
		break;

	default:
		device_printf(hw->acpi_dev, "unexpected type %d while parsing "
		    "Current Resource Settings (_CSR)\n", res->Type);
		break;
	}

	return AE_OK;
}

static int
iichid_get_hw(device_t dev, ACPI_HANDLE handle, struct iichid_hw *hw)
{
	ACPI_OBJECT *result;
	ACPI_OBJECT_LIST acpi_arg;
	ACPI_BUFFER acpi_buf;
	ACPI_STATUS status;
	ACPI_DEVICE_INFO *device_info;

	hw->acpi_dev = dev;

	/*
	 * function (_DSM) to be evaluated to retrieve the address of
	 * the configuration register of the HID device
	 */
	/* 3cdff6f7-4267-4555-ad05-b30a3d8938de */
	static uint8_t dsm_guid[] = {
		0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
		0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE,
	};

	/* prepare 4 arguments */
	static ACPI_OBJECT args[] = {{
		.Buffer.Type = ACPI_TYPE_BUFFER,
		.Buffer.Length = sizeof(dsm_guid),
		.Buffer.Pointer = dsm_guid,
	}, {
		.Integer.Type = ACPI_TYPE_INTEGER,
		.Integer.Value = 1,
	}, {
		.Integer.Type = ACPI_TYPE_INTEGER,
		.Integer.Value = 1,
	}, {
		.Package.Type = ACPI_TYPE_PACKAGE,
		.Package.Count = 0,
	}};

	/* _CRS holds device addr and irq and needs a callback to evaluate */
	status = AcpiWalkResources(handle, "_CRS", iichid_res_walk_cb, hw);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "could not evaluate _CRS\n");
		return (ENXIO);
	}

	/* Evaluate _DSM method to obtain HID Descriptor address */
	acpi_arg.Pointer = args;
	acpi_arg.Count = nitems(args);

	acpi_buf.Pointer = NULL;
	acpi_buf.Length = ACPI_ALLOCATE_BUFFER;

	status = AcpiEvaluateObject(handle, "_DSM", &acpi_arg, &acpi_buf);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "error evaluating _DSM\n");
		if (acpi_buf.Pointer != NULL)
			AcpiOsFree(acpi_buf.Pointer);
		return (ENXIO);
	}

	/* the result will contain the register address (int type) */
	result = (ACPI_OBJECT *) acpi_buf.Pointer;
	if (result->Type != ACPI_TYPE_INTEGER) {
		device_printf(dev, "_DSM should return descriptor register address as integer\n");
		AcpiOsFree(result);
		return (ENXIO);
	}

	/* take it (much work done for one byte -.-) */
	hw->config_reg = result->Integer.Value;

	AcpiOsFree(result);

	/* get ACPI HID. It is a base part of the evdev device name */
	status = AcpiGetObjectInfo(handle, &device_info);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "error evaluating AcpiGetObjectInfo\n");
		return (ENXIO);
	}

	if (device_info->Valid & ACPI_VALID_HID)
		strlcpy(hw->hid, device_info->HardwareId.String,
		    sizeof(hw->hid));

	AcpiOsFree(device_info);
/*
	device_printf(dev, "  ACPI Hardware ID: %s\n", hw->hid);
	device_printf(dev, "  IICbus addr: 0x%02x\n", hw->device_addr);
	device_printf(dev, "  IRQ: %d\n", hw->irq);
	device_printf(dev, "  GPIO pin: %02X\n", hw->gpio_pin);
	device_printf(dev, "  HID descriptor register: 0x%hx\n", hw->config_reg);
*/
	return (0);
}

static ACPI_STATUS
iichid_get_device_hw_cb(ACPI_HANDLE handle, UINT32 level, void *context,
    void **status)
{
	struct iichid_hw buf;
	struct iichid_hw *hw = context;
	device_t iicbus = hw->acpi_dev;
	uint16_t addr = hw->device_addr;
	UINT32 sta;

	bzero(&buf, sizeof(buf));
	/*
	 * If no _STA method or if it failed, then assume that
	 * the device is present.
	 */
	if ((acpi_MatchHid(handle, "PNP0C50") ||
	     acpi_MatchHid(handle, "ACPI0C50")) &&
	    (ACPI_FAILURE(acpi_GetInteger(handle, "_STA", &sta)) ||
	     ACPI_DEVICE_PRESENT(sta)) &&
	    iichid_get_hw(iicbus, handle, &buf) == 0) {

		if (addr == hw->device_addr)
			/* XXX: need to break walking loop as well */
			bcopy(&buf, hw, sizeof(struct iichid_hw));
	}

	return (AE_OK);
}

static int
iichid_get_device_hw(device_t dev, uint16_t addr, struct iichid_hw *hw)
{
	ACPI_HANDLE ctrl_handle;
	device_t iicbus = device_get_parent(dev);
	hw->device_addr = iicbus_get_addr(dev);

	ctrl_handle = acpi_get_handle(device_get_parent(iicbus));
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, ctrl_handle,
	    1, iichid_get_device_hw_cb, NULL, hw, NULL);

	return (0);
}

static int
iichid_fetch_buffer(device_t dev, void* cmd, int cmdlen, void *buf, int buflen)
{
	uint16_t addr = iicbus_get_addr(dev);
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd },
	    { addr << 1, IIC_M_RD, buflen, buf },
	};

	return (iicbus_transfer(dev, msgs, nitems(msgs)));
}

static int
iichid_fetch_input_report(struct iichid* sc, uint8_t *data, int len, int *actual_len)
{
	uint16_t cmd = sc->desc.wInputRegister;
	int cmdlen = sizeof(cmd);
	uint8_t buf[len];

	int error = iichid_fetch_buffer(sc->dev, &cmd, cmdlen, buf, len);
	if (error != 0) {
		device_printf(sc->dev, "could not retrieve input report (%d)\n", error);
		return (error);
	}

	memcpy(data, buf, len);
	*actual_len = data[0] | data[1] << 8;

	return (0);
}

static int
iichid_fetch_hid_descriptor(device_t dev, uint16_t cmd, struct i2c_hid_desc *hid_desc)
{

	return (iichid_fetch_buffer(dev, &cmd, sizeof(cmd), hid_desc, sizeof(struct i2c_hid_desc)));
}

int
iichid_fetch_report_descriptor(struct iichid* sc, uint8_t **buf, int *len)
{
	int error;
	uint8_t *tmpbuf;

	tmpbuf = malloc(sc->desc.wReportDescLength, M_TEMP, M_WAITOK | M_ZERO);
	error = iichid_fetch_buffer(sc->dev, &sc->desc.wReportDescRegister,
	    sizeof(sc->desc.wReportDescRegister), tmpbuf,
	    le16toh(sc->desc.wReportDescLength));
	if (error) {
		free (tmpbuf, M_TEMP);
		return (error);
	}

	*buf = tmpbuf;
	*len = le16toh(sc->desc.wReportDescLength);

	sc->input_size = hid_report_size(tmpbuf, *len, hid_input, NULL) + 2;
	if (sc->input_size != le16toh(sc->desc.wMaxInputLength))
		device_printf(sc->dev, "determined (len=%d) and described (len=%d)"
		    " input report lengths mismatch\n",
		    sc->input_size, le16toh(sc->desc.wMaxInputLength));

	return (0);
}

int
iichid_get_report(struct iichid* sc, uint8_t *buf, int len, uint8_t type,
    uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	uint16_t dtareg = htole16(sc->desc.wDataRegister);
	uint16_t cmdreg = htole16(sc->desc.wCommandRegister);
	uint8_t cmd[] =	{   /*________|______id>=15_____|______id<15______*/
						  cmdreg & 0xff		   ,
						   cmdreg >> 8		   ,
			    (id >= 15 ? 15 | (type << 4): id | (type << 4)),
					      I2C_HID_CMD_GET_REPORT	   ,
			    (id >= 15 ?		id	:   dtareg & 0xff ),
			    (id >= 15 ?   dtareg & 0xff	:   dtareg >> 8   ),
			    (id >= 15 ?   dtareg >> 8	:	0	  ),
			};
	int cmdlen    =	    (id >= 15 ?		7	:	6	  );
	int report_id_len = (id >= 15 ?		2	:	1	  );
	int report_len = len + 2;
	int d, err;
	uint8_t *tmprep;

	device_printf(sc->dev, "HID command I2C_HID_CMD_GET_REPORT %d "
	    "(type %d, len %d)\n", id, type, len);

	/*
	 * 7.2.2.2 - Response will be a 2-byte length value, the report
	 * id with length determined above, and then the report.
	 * Allocate len + 2 + 2 bytes, read into that temporary
	 * buffer, and then copy only the report back out to buf.
	 */
	report_len += report_id_len;
	tmprep = malloc(report_len, M_DEVBUF, M_WAITOK | M_ZERO);

	/* type 3 id 8: 22 00 38 02 23 00 */
	err = iichid_fetch_buffer(sc->dev, &cmd, cmdlen, tmprep, report_len);
	if (err != 0) {
		free(tmprep, M_DEVBUF);
		return (EIO);
	}

	d = tmprep[0] | tmprep[1] << 8;
	if (d != report_len)
		device_printf(sc->dev,
		    "response size %d != expected length %d\n", d, report_len);

	if (report_id_len == 2)
		d = tmprep[2] | tmprep[3] << 8;
	else
		d = tmprep[2];

	if (d != id) {
		device_printf(sc->dev, "response report id %d != %d\n",
		    d, id);
		free(tmprep, M_DEVBUF);
		return (1);
	}

	device_printf(sc->dev, "response: %*D\n", report_len, tmprep, " ");

	memcpy(buf, tmprep + 2 + report_id_len, len);
	free(tmprep, M_DEVBUF);

	return (0);
}

static void
iichid_intr(void *context)
{
	struct iichid *sc = context;

	taskqueue_enqueue(sc->taskqueue, &sc->event_task);
}

static void
iichid_event_task(void *context, int pending)
{
	struct iichid *sc = context;
	int actual = 0;
	int error;

	error = iichid_fetch_input_report(sc, sc->input_buf, sc->input_size, &actual);
	if (error != 0) {
		device_printf(sc->dev, "an error occured\n");
		goto out;
	}

	if (actual <= 2) {
//		device_printf(sc->dev, "no data received\n");
		goto out;
	}

	sc->intr_handler(sc->intr_sc, ((uint8_t *)sc->input_buf) + 2, actual - 2);

out:
	mtx_lock(&sc->lock);
	if (sc->callout_setup && sc->sampling_rate > 0)
		callout_reset(&sc->periodic_callout, hz / sc->sampling_rate,
		    iichid_intr, sc);
	mtx_unlock(&sc->lock);
}

static int
iichid_setup_callout(struct iichid *sc)
{

	if (sc->sampling_rate < 0) {
		device_printf(sc->dev, "sampling_rate is below 0, can't setup callout\n");
		return (EINVAL);
	}

	callout_init_mtx(&sc->periodic_callout, &sc->lock, 0);
	sc->callout_setup=true;
	device_printf(sc->dev, "successfully setup callout\n");
	return (0);
}

static int
iichid_reset_callout(struct iichid *sc)
{

	if (sc->sampling_rate <= 0) {
		device_printf(sc->dev, "sampling_rate is below or equal to 0, can't reset callout\n");
		return (EINVAL);
	}

	if (sc->callout_setup)
		callout_reset(&sc->periodic_callout, hz / sc->sampling_rate, iichid_intr, sc);
	else
		return (EINVAL);
	return (0);
}

static void
iichid_teardown_callout(struct iichid *sc)
{
	callout_stop(&sc->periodic_callout);
	sc->callout_setup=false;
	device_printf(sc->dev, "tore callout down\n");
}

static int
iichid_setup_interrupt(struct iichid *sc)
{
	sc->irq_rid = 0;
	sc->irq_cookie = 0;
	sc->irq_res = 0;
	sc->irq_res = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &sc->irq_rid, RF_ACTIVE);

	if( sc->irq_res != NULL )
	{
		device_printf(sc->dev, "allocated irq at 0x%lx and rid %d\n", (uint64_t)sc->irq_res, sc->irq_rid);
		int error = bus_setup_intr(sc->dev, sc->irq_res, INTR_TYPE_TTY | INTR_MPSAFE, NULL, iichid_intr, sc, &sc->irq_cookie);
		if (error != 0)
		{
			device_printf(sc->dev, "Could not setup interrupt handler\n");
			bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq_res);
			return error;
		} else
			device_printf(sc->dev, "successfully setup interrupt\n");

	} else {
		device_printf(sc->dev, "could not allocate IRQ resource\n");
	}

	return (0);
}

static void
iichid_teardown_interrupt(struct iichid *sc)
{
	if (sc->irq_cookie)
		bus_teardown_intr(sc->dev, sc->irq_res, sc->irq_cookie);

	if (sc->irq_res)
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq_res);

	sc->irq_rid = 0;
	sc->irq_cookie = 0;
	sc->irq_res = 0;
}

static int
iichid_sysctl_sampling_rate_handler(SYSCTL_HANDLER_ARGS)
{
	int err, value, oldval;
	struct iichid *sc;

	sc = arg1;      

	mtx_lock(&sc->lock);

	value = sc->sampling_rate;
	oldval = sc->sampling_rate;
	err = sysctl_handle_int(oidp, &value, 0, req);

	if (err != 0 || req->newptr == NULL || value == sc->sampling_rate) {
		mtx_unlock(&sc->lock);
		return (err);
	}

	sc->sampling_rate = value;

	if (oldval < 0 && value >= 0) {
		iichid_teardown_interrupt(sc);
		iichid_setup_callout(sc);
	} else if (oldval >= 0 && value < 0) {
		iichid_teardown_callout(sc);
		iichid_setup_interrupt(sc);
	}

	if (value > 0)
		iichid_reset_callout(sc);

	device_printf(sc->dev, "new sampling_rate value: %d\n", value);

	mtx_unlock(&sc->lock);

	return (0);
}

int
iichid_set_intr(struct iichid *sc, iichid_intr_t intr, void *intr_sc)
{
	int error;

	sc->intr_handler = intr;
	sc->intr_sc = intr_sc;

	TASK_INIT(&sc->event_task, 0, iichid_event_task, sc);
	sc->taskqueue = taskqueue_create("imt_tq", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &sc->taskqueue);
	if (sc->taskqueue == NULL)
		return (ENXIO);

#if 0
	if (sc->hw.irq > 0) {
		/* move IRQ resource to the initialized device */
		u_long irq = bus_get_resource_start(sc->hw.acpi_dev, SYS_RES_IRQ, 0);
		bus_delete_resource(sc->hw.acpi_dev, SYS_RES_IRQ, 0);
		bus_set_resource(sc->dev, SYS_RES_IRQ, 0, irq, 1);
	}
#endif

	/*
	 * Fallback to HID descriptor input length
	 * if report descriptor has not been fetched yet
	 */
	if (sc->input_size == 0)
		sc->input_size = le16toh(sc->desc.wMaxInputLength);

	sc->input_buf = malloc(sc->input_size, M_DEVBUF, M_WAITOK | M_ZERO);
	taskqueue_start_threads(&sc->taskqueue, 1, PI_TTY, "%s taskq", device_get_nameunit(sc->dev));

	sc->sampling_rate = sc->hw.irq > 0 ? -1 : IICHID_DEFAULT_SAMPLING_RATE;

	if (sc->sampling_rate < 0) {
		error = iichid_setup_interrupt(sc);
		if (error != 0) {
			device_printf(sc->dev,
			    "Interrupt setup failed. Fallback to sampling.\n");
			sc->sampling_rate = IICHID_DEFAULT_SAMPLING_RATE;
		}
	}
	if (sc->sampling_rate >= 0) {
		iichid_setup_callout(sc);
		iichid_reset_callout(sc);
	}

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sc->dev),
		SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)),
		OID_AUTO, "sampling_rate", CTLTYPE_INT | CTLFLAG_RWTUN,
		sc, 0,
		iichid_sysctl_sampling_rate_handler, "I", "sampling rate in num/second");

	return (0);
}

int
iichid_init(struct iichid *sc, device_t dev)
{
//	device_t parent;
	uint16_t addr = iicbus_get_addr(dev);
	int error;

	if (addr == 0)
		return (ENXIO);

	sc->dev = dev;
	sc->input_buf = NULL;

	/* Fetch hardware settings from ACPI */
	error = iichid_get_device_hw(dev, addr, &sc->hw);
	if (error)
		return (ENXIO);

	error = iichid_fetch_hid_descriptor(dev, sc->hw.config_reg, &sc->desc);
	if (error) {
		device_printf(dev, "could not retrieve HID descriptor from device: %d\n", error);
		return (ENXIO);
	}

	if (le16toh(sc->desc.wHIDDescLength) != 30 ||
	    le16toh(sc->desc.bcdVersion) != 0x100) {
		device_printf(dev, "HID descriptor is broken\n");
		return (ENXIO);
	}

	return (0);
}

void
iichid_destroy(struct iichid *sc)
{

	if (sc->input_buf)
		free(sc->input_buf, M_DEVBUF);

	mtx_lock(&sc->lock);

	iichid_teardown_callout(sc);
	iichid_teardown_interrupt(sc);

	if (sc->taskqueue) {
		taskqueue_block(sc->taskqueue);
		taskqueue_drain(sc->taskqueue, &sc->event_task);
		taskqueue_free(sc->taskqueue);
	}

	mtx_unlock(&sc->lock);
#if 0
	if (sc->hw.irq > 0) {
		/* return IRQ resource back to the ACPI driver */
		u_long irq = bus_get_resource_start(sc->dev, SYS_RES_IRQ, 0);
		bus_delete_resource(sc->dev, SYS_RES_IRQ, 0);
		bus_set_resource(sc->hw.acpi_dev, SYS_RES_IRQ, 0, irq, 1);
	}
#endif
}

static ACPI_STATUS
iichid_identify_cb(ACPI_HANDLE handle, UINT32 level, void *context,
    void **status)
{
	struct iichid_hw hw;
	device_t iicbus = context;
	device_t child, *children;
	UINT32 sta;
	int ccount, i;

	/*
	 * If no _STA method or if it failed, then assume that
	 * the device is present.
	 */
	if ((acpi_MatchHid(handle, "PNP0C50") ||
	     acpi_MatchHid(handle, "ACPI0C50")) &&
	    (ACPI_FAILURE(acpi_GetInteger(handle, "_STA", &sta)) ||
	     ACPI_DEVICE_PRESENT(sta)) &&
	    iichid_get_hw(iicbus, handle, &hw) == 0) {

		/* get a list of all children below iicbus */
		if (device_get_children(iicbus, &children, &ccount) != 0)
			return (AE_OK);

		/* scan through to find out if I2C addr is already in use */
		for (i = 0; i < ccount; i++) {
			if (iicbus_get_addr(children[i]) == hw.device_addr) {
				free(children, M_TEMP);
				return (AE_OK);
			}
		}
		free(children, M_TEMP);

		/* No I2C devices tied to the addr found. Add a child */
		child = BUS_ADD_CHILD(iicbus, 0, NULL, -1);
		if (child != NULL)
			iicbus_set_addr(child, hw.device_addr);
		else
			device_printf(iicbus, "add child failed\n");
	}

	return (AE_OK);
}

void
iichid_identify(driver_t *driver, device_t parent)
{
	ACPI_HANDLE	ctrl_handle;

	ctrl_handle = acpi_get_handle(device_get_parent(parent));
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, ctrl_handle,
	    1, iichid_identify_cb, NULL, parent, NULL);
}

MODULE_DEPEND(iichid, acpi, 1, 1, 1);
MODULE_DEPEND(iichid, usb, 1, 1, 1);
MODULE_VERSION(iichid, 1);
