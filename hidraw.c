/*	$NetBSD: uhid.c,v 1.46 2001/11/13 06:24:55 lukem Exp $	*/

/* Also already merged from NetBSD:
 *	$NetBSD: uhid.c,v 1.54 2002/09/23 05:51:21 simonb Exp $
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*-
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
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

/*
 * HID spec: http://www.usb.org/developers/devclass_docs/HID1_11.pdf
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/signalvar.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/filio.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/ioccom.h>
#include <sys/conf.h>
#include <sys/tty.h>
#include <sys/selinfo.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include "clist.h"

#include "hid.h"
#include "hidbus.h"
#include <dev/usb/usb_ioctl.h>

#ifdef USB_DEBUG
#define DPRINTF(x)	if (hidrawdebug) printf x
#define DPRINTFN(n,x)	if (hidrawdebug>(n)) printf x
int	hidrawdebug = 0;
SYSCTL_NODE(_hw_usb, OID_AUTO, hidraw, CTLFLAG_RW, 0, "HID raw interface");
SYSCTL_INT(_hw_usb_hidraw, OID_AUTO, debug, CTLFLAG_RW,
	   &hidrawdebug, 0, "hidraw debug level");
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

struct hidraw_softc {
	device_t sc_dev;			/* base device */
	int sc_ep_addr;

	int sc_isize;
	int sc_osize;
	int sc_fsize;
	u_int8_t sc_iid;
	u_int8_t sc_oid;
	u_int8_t sc_fid;

	u_char *sc_ibuf;
	u_char *sc_obuf;

	void *sc_repdesc;
	int sc_repdesc_size;

	struct clist sc_q;
	struct selinfo sc_rsel;
	struct proc *sc_async;	/* process that wants SIGIO */
	struct {			/* driver state */
		bool	open:1;		/* device is open */
		bool	aslp:1;		/* waiting for device data */
		bool	immed:1;	/* return read data immediately */
		bool	dying:1;	/* driver is detaching */
		u_char	reserved:4;
	} sc_state;

	int sc_refcnt;

	struct cdev *dev;
};

#define	UHIDUNIT(dev)	(minor(dev))
#define	UHID_CHUNK	128	/* chunk size for read */
#define	UHID_BSIZE	1020	/* buffer size */
#define	UHID_INDEX	0xFF	/* Arbitrary high value */

d_open_t	hidrawopen;
d_close_t	hidrawclose;
d_read_t	hidrawread;
d_write_t	hidrawwrite;
d_ioctl_t	hidrawioctl;
d_poll_t	hidrawpoll;


static struct cdevsw hidraw_cdevsw = {
	.d_version =	D_VERSION,
	.d_flags =	D_NEEDGIANT,
	.d_open =	hidrawopen,
	.d_close =	hidrawclose,
	.d_read =	hidrawread,
	.d_write =	hidrawwrite,
	.d_ioctl =	hidrawioctl,
	.d_poll =	hidrawpoll,
	.d_name =	"hidraw",
};

static void hidraw_intr(void *, void *, uint16_t);

static int hidraw_do_read(struct hidraw_softc *, struct uio *uio, int);
static int hidraw_do_write(struct hidraw_softc *, struct uio *uio, int);
static int hidraw_do_ioctl(struct hidraw_softc *, u_long, caddr_t, int,
			      struct thread *);

static device_identify_t hidraw_identify;
static device_probe_t hidraw_match;
static device_attach_t hidraw_attach;
static device_detach_t hidraw_detach;

static device_method_t hidraw_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,	hidraw_identify),
	DEVMETHOD(device_probe,		hidraw_match),
	DEVMETHOD(device_attach,	hidraw_attach),
	DEVMETHOD(device_detach,	hidraw_detach),

	{ 0, 0 }
};

static driver_t hidraw_driver = {
	"hidraw",
	hidraw_methods,
	sizeof(struct hidraw_softc)
};

static devclass_t hidraw_devclass;

DRIVER_MODULE(hidraw, hidbus, hidraw_driver, hidraw_devclass, NULL, 0);
MODULE_DEPEND(hidraw, hidbus, 1, 1, 1);
MODULE_DEPEND(hidraw, hid, 1, 1, 1);
MODULE_VERSION(hidraw, 1);

static void
hidraw_identify(driver_t *driver, device_t parent)
{
	device_t child;

	if (device_find_child(parent, "hidraw", -1) == NULL) {
		child = BUS_ADD_CHILD(parent, 0, "hidraw", -1);
		hidbus_set_index(child, UHID_INDEX);
	}
}

static int
hidraw_match(device_t self)
{

	if (hidbus_get_index(self) != UHID_INDEX)
		return (ENXIO);

#ifdef NOT_YET
	if (usbd_get_quirks(uaa->device)->uq_flags & UQ_HID_IGNORE)
		return (ENXIO);
#endif

	return (BUS_PROBE_GENERIC);
}

static int
hidraw_attach(device_t self)
{
	struct hidraw_softc *sc = device_get_softc(self);
	struct make_dev_args mda;
	uint16_t size;
	void *desc;
	int error;

	sc->sc_dev = self;

	error = hid_get_report_descr(sc->sc_dev, &desc, &size);
	if (error) {
		device_printf(self, "no report descriptor\n");
		return ENXIO;
	}

	sc->sc_isize = hid_report_size(desc, size, hid_input,   &sc->sc_iid);
	sc->sc_osize = hid_report_size(desc, size, hid_output,  &sc->sc_oid);
	sc->sc_fsize = hid_report_size(desc, size, hid_feature, &sc->sc_fid);

	sc->sc_repdesc = desc;
	sc->sc_repdesc_size = size;

	make_dev_args_init(&mda);
	mda.mda_flags = MAKEDEV_WAITOK;
	mda.mda_devsw = &hidraw_cdevsw;
	mda.mda_uid = UID_ROOT;
	mda.mda_gid = GID_OPERATOR;
	mda.mda_mode = 0600;
	mda.mda_si_drv1 = sc;

	error = make_dev_s(&mda, &sc->dev, "hidraw%d", device_get_unit(self));
	if (error) {
		device_printf(self, "Can not create character device\n");
		return (error);
	}

	hidbus_set_intr(sc->sc_dev, hidraw_intr);

	return 0;
}

static int
hidraw_detach(device_t self)
{
	struct hidraw_softc *sc = device_get_softc(self);

	DPRINTF(("hidraw_detach: sc=%p\n", sc));

	mtx_lock(hidbus_get_lock(self));
	sc->sc_state.dying = true;
	if (sc->sc_state.open) {
		if (--sc->sc_refcnt >= 0) {
			/* Wake everyone */
			wakeup(&sc->sc_q);
			/* Wait for processes to go away. */
//			usb_detach_wait(sc->sc_dev);
		}
	}
	mtx_unlock(hidbus_get_lock(self));
	destroy_dev(sc->dev);

	return (0);
}

void
hidraw_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hidraw_softc *sc = device_get_softc(dev);

#ifdef USB_DEBUG
	if (hidrawdebug > 5) {
		u_int32_t i;

		DPRINTF(("hidraw_intr: len=%d\n", len));
		DPRINTF(("hidraw_intr: data ="));
		for (i = 0; i < len; i++)
			DPRINTF((" %02x", ((uint8_t *)buf)[i]));
		DPRINTF(("\n"));
	}
#endif

	(void) b_to_q(buf, sc->sc_isize, &sc->sc_q);

	if (sc->sc_state.aslp) {
		sc->sc_state.aslp = false;
		DPRINTFN(5, ("hidraw_intr: waking %p\n", &sc->sc_q));
		wakeup(&sc->sc_q);
	}
	selwakeuppri(&sc->sc_rsel, PZERO);
#ifdef NOT_YET
	if (sc->sc_async != NULL) {
		DPRINTFN(3, ("hidraw_intr: sending SIGIO %p\n", sc->sc_async));
		PROC_LOCK(sc->sc_async);
		psignal(sc->sc_async, SIGIO);
		PROC_UNLOCK(sc->sc_async);
	}
#endif
}

int
hidrawopen(struct cdev *dev, int flag, int mode, struct thread *p)
{
	struct hidraw_softc *sc;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	DPRINTF(("hidrawopen: sc=%p\n", sc));

	if (sc->sc_state.dying)
		return (ENXIO);

	if (sc->sc_state.open)
		return (EBUSY);
	sc->sc_state.open = true;

	clist_alloc_cblocks(&sc->sc_q, UHID_BSIZE, UHID_BSIZE);
	sc->sc_ibuf = malloc(sc->sc_isize, M_USBDEV, M_WAITOK);
	sc->sc_obuf = malloc(sc->sc_osize, M_USBDEV, M_WAITOK);

	/* Set up interrupt pipe. */
	mtx_lock(hidbus_get_lock(sc->sc_dev));
	hidbus_intr_start(sc->sc_dev);
	sc->sc_state.immed = false;
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

#ifdef NOT_YET
	sc->sc_async = 0;
#endif

	return (0);
}

int
hidrawclose(struct cdev *dev, int flag, int mode, struct thread *p)
{
	struct hidraw_softc *sc;

	sc = dev->si_drv1;

	DPRINTF(("hidrawclose: sc=%p\n", sc));

	/* Disable interrupts. */
	mtx_lock(hidbus_get_lock(sc->sc_dev));
	hidbus_intr_stop(sc->sc_dev);
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

	ndflush(&sc->sc_q, sc->sc_q.c_cc);
	clist_free_cblocks(&sc->sc_q);

	free(sc->sc_ibuf, M_USBDEV);
	free(sc->sc_obuf, M_USBDEV);
	sc->sc_ibuf = sc->sc_obuf = NULL;

	mtx_lock(hidbus_get_lock(sc->sc_dev));
	sc->sc_state.open = false;
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

#ifdef NOT_YET
	sc->sc_async = 0;
#endif

	return (0);
}

int
hidraw_do_read(struct hidraw_softc *sc, struct uio *uio, int flag)
{
	int error = 0;
	size_t length;
	u_char buffer[UHID_CHUNK];

	DPRINTFN(1, ("hidrawread\n"));
	if (sc->sc_state.immed) {
		DPRINTFN(1, ("hidrawread immed\n"));

		error = hid_get_report(sc->sc_dev, buffer, sc->sc_isize, NULL,
		    HID_INPUT_REPORT, sc->sc_iid);
		if (error)
			return (EIO);
		return (uiomove(buffer, sc->sc_isize, uio));
	}

	mtx_lock(hidbus_get_lock(sc->sc_dev));
	while (sc->sc_q.c_cc == 0) {
		if (flag & O_NONBLOCK) {
			mtx_unlock(hidbus_get_lock(sc->sc_dev));
			return (EWOULDBLOCK);
		}
		sc->sc_state.aslp = true;
		DPRINTFN(5, ("hidrawread: sleep on %p\n", &sc->sc_q));
		error = mtx_sleep(&sc->sc_q, hidbus_get_lock(sc->sc_dev),
		    PZERO | PCATCH, "hidrawrea", 0);
		DPRINTFN(5, ("hidrawread: woke, error=%d\n", error));
		if (sc->sc_state.dying)
			error = EIO;
		if (error) {
			sc->sc_state.aslp = false;
			break;
		}
	}

	/* Transfer as many chunks as possible. */
	while (sc->sc_q.c_cc > 0 && uio->uio_resid > 0 && !error) {
		length = min(sc->sc_q.c_cc, uio->uio_resid);
		if (length > sizeof(buffer))
			length = sizeof(buffer);

		/* Remove a small chunk from the input queue. */
		(void) q_to_b(&sc->sc_q, buffer, length);
		DPRINTFN(5, ("hidrawread: got %lu chars\n", (u_long)length));

		/* Copy the data to the user process. */
		mtx_unlock(hidbus_get_lock(sc->sc_dev));
		error = uiomove(buffer, length, uio);
		mtx_lock(hidbus_get_lock(sc->sc_dev));
			break;
	}
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

	return (error);
}

int
hidrawread(struct cdev *dev, struct uio *uio, int flag)
{
	struct hidraw_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = hidraw_do_read(sc, uio, flag);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
hidraw_do_write(struct hidraw_softc *sc, struct uio *uio, int flag)
{
	int error;
	int size;

	DPRINTFN(1, ("hidrawwrite\n"));

	if (sc->sc_state.dying)
		return (EIO);

	size = sc->sc_osize;
	error = 0;
	if (uio->uio_resid != size)
		return (EINVAL);
	error = uiomove(sc->sc_obuf, size, uio);
	if (!error) {
		if (sc->sc_oid)
			error = hid_set_report(sc->sc_dev, sc->sc_obuf+1,
			    size-1, UHID_OUTPUT_REPORT, sc->sc_obuf[0]);
		else
			error = hid_set_report(sc->sc_dev, sc->sc_obuf,
			    size, UHID_OUTPUT_REPORT, 0);
	}

	return (error);
}

int
hidrawwrite(struct cdev *dev, struct uio *uio, int flag)
{
	struct hidraw_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = hidraw_do_write(sc, uio, flag);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
hidraw_do_ioctl(struct hidraw_softc *sc, u_long cmd, caddr_t addr, int flag,
	      struct thread *p)
{
	struct usb_gen_descriptor *ugd;
	void *buf;
	int size, id;
	int error;

	DPRINTFN(2, ("hidrawioctl: cmd=%lx\n", cmd));

	if (sc->sc_state.dying)
		return (EIO);

	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;

#ifdef NOT_YET
	case FIOASYNC:
		if (*(int *)addr) {
			if (sc->sc_async != NULL)
				return (EBUSY);
			sc->sc_async = p->td_proc;
			DPRINTF(("hidraw_do_ioctl: FIOASYNC %p\n", sc->sc_async));
		} else
			sc->sc_async = NULL;
		break;

	/* XXX this is not the most general solution. */
	case TIOCSPGRP:
		if (sc->sc_async == NULL)
			return (EINVAL);
		if (*(int *)addr != sc->sc_async->p_pgid)
			return (EPERM);
		break;
#endif
	case USB_GET_REPORT_DESC:
		ugd = (struct usb_gen_descriptor *)addr;
		if (sc->sc_repdesc_size > ugd->ugd_maxlen) {
			size = ugd->ugd_maxlen;
		} else {
			size = sc->sc_repdesc_size;
		}
		ugd->ugd_actlen = size;
		if (ugd->ugd_data == NULL)
			break;		/* descriptor length only */
		error = copyout(sc->sc_repdesc, ugd->ugd_data, size);
		break;

	case USB_SET_IMMED:
		if (*(int *)addr) {
			/* XXX should read into ibuf, but does it matter? */
			error = hid_get_report(sc->sc_dev, sc->sc_ibuf,
			    sc->sc_isize, NULL, UHID_INPUT_REPORT, sc->sc_iid);
			if (error)
				return (EOPNOTSUPP);

			mtx_lock(hidbus_get_lock(sc->sc_dev));
			sc->sc_state.immed = true;
			mtx_unlock(hidbus_get_lock(sc->sc_dev));
		} else {
			mtx_lock(hidbus_get_lock(sc->sc_dev));
			sc->sc_state.immed = false;
			mtx_unlock(hidbus_get_lock(sc->sc_dev));
		}
		break;

	case USB_GET_REPORT:
		ugd = (struct usb_gen_descriptor *)addr;
		switch (ugd->ugd_report_type) {
		case UHID_INPUT_REPORT:
			size = sc->sc_isize;
			id = sc->sc_iid;
			break;
		case UHID_OUTPUT_REPORT:
			size = sc->sc_osize;
			id = sc->sc_oid;
			break;
		case UHID_FEATURE_REPORT:
			size = sc->sc_fsize;
			id = sc->sc_fid;
			break;
		default:
			return (EINVAL);
		}
		if (id != 0)
			copyin(ugd->ugd_data, &id, 1);
		size = imin(ugd->ugd_maxlen, size);
		buf = malloc(size, M_TEMP, M_WAITOK);
		error = hid_get_report(sc->sc_dev, buf, size, NULL,
		    ugd->ugd_report_type, id);
		if (!error)
			copyout(buf, ugd->ugd_data, size);
		free(buf, M_TEMP);
		break;

	case USB_SET_REPORT:
		ugd = (struct usb_gen_descriptor *)addr;
		switch (ugd->ugd_report_type) {
		case UHID_INPUT_REPORT:
			size = sc->sc_isize;
			id = sc->sc_iid;
			break;
		case UHID_OUTPUT_REPORT:
			size = sc->sc_osize;
			id = sc->sc_oid;
			break;
		case UHID_FEATURE_REPORT:
			size = sc->sc_fsize;
			id = sc->sc_fid;
			break;
		default:
			return (EINVAL);
		}
		size = imin(ugd->ugd_maxlen, size);
		buf = malloc(size, M_TEMP, M_WAITOK);
		copyin(ugd->ugd_data, buf, size);
		if (id != 0)
			id = *(uint8_t *)buf;
		error = hid_set_report(sc->sc_dev, buf, size,
		    ugd->ugd_report_type, id);
		free(buf, M_TEMP);
		if (error)
			return (EIO);
		break;

	case USB_GET_REPORT_ID:
		*(int *)addr = 0;	/* XXX: we only support reportid 0? */
		break;

	default:
		return (EINVAL);
	}
	return (0);
}

int
hidrawioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, struct thread *p)
{
	struct hidraw_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = hidraw_do_ioctl(sc, cmd, addr, flag, p);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
hidrawpoll(struct cdev *dev, int events, struct thread *p)
{
	struct hidraw_softc *sc;
	int revents = 0;

	sc = dev->si_drv1;
	if (sc->sc_state.dying)
		return (EIO);

	mtx_lock(hidbus_get_lock(sc->sc_dev));
	if (events & (POLLOUT | POLLWRNORM))
		revents |= events & (POLLOUT | POLLWRNORM);
	if (events & (POLLIN | POLLRDNORM)) {
		if (sc->sc_q.c_cc > 0)
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(p, &sc->sc_rsel);
	}

	mtx_unlock(hidbus_get_lock(sc->sc_dev));
	return (revents);
}
