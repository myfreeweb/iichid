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
#define DPRINTF(x)	if (uhiddebug) printf x
#define DPRINTFN(n,x)	if (uhiddebug>(n)) printf x
int	uhiddebug = 0;
SYSCTL_NODE(_hw_usb, OID_AUTO, uhid, CTLFLAG_RW, 0, "USB uhid");
SYSCTL_INT(_hw_usb_uhid, OID_AUTO, debug, CTLFLAG_RW,
	   &uhiddebug, 0, "uhid debug level");
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

struct uhid_softc {
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
	u_char sc_state;	/* driver state */
#define	UHID_OPEN	0x01	/* device is open */
#define	UHID_ASLP	0x02	/* waiting for device data */
#define UHID_NEEDCLEAR	0x04	/* needs clearing endpoint stall */
#define UHID_IMMED	0x08	/* return read data immediately */

	int sc_refcnt;
	u_char sc_dying;

	struct cdev *dev;
};

#define	UHIDUNIT(dev)	(minor(dev))
#define	UHID_CHUNK	128	/* chunk size for read */
#define	UHID_BSIZE	1020	/* buffer size */
#define	UHID_INDEX	0xFF	/* Arbitrary high value */

d_open_t	uhidopen;
d_close_t	uhidclose;
d_read_t	uhidread;
d_write_t	uhidwrite;
d_ioctl_t	uhidioctl;
d_poll_t	uhidpoll;


static struct cdevsw uhid_cdevsw = {
	.d_version =	D_VERSION,
	.d_flags =	D_NEEDGIANT,
	.d_open =	uhidopen,
	.d_close =	uhidclose,
	.d_read =	uhidread,
	.d_write =	uhidwrite,
	.d_ioctl =	uhidioctl,
	.d_poll =	uhidpoll,
	.d_name =	"uhid",
};

static void uhid_intr(void *, void *, uint16_t);

static int uhid_do_read(struct uhid_softc *, struct uio *uio, int);
static int uhid_do_write(struct uhid_softc *, struct uio *uio, int);
static int uhid_do_ioctl(struct uhid_softc *, u_long, caddr_t, int,
			      struct thread *);

static device_identify_t uhid_identify;
static device_probe_t uhid_match;
static device_attach_t uhid_attach;
static device_detach_t uhid_detach;

static device_method_t uhid_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,	uhid_identify),
	DEVMETHOD(device_probe,		uhid_match),
	DEVMETHOD(device_attach,	uhid_attach),
	DEVMETHOD(device_detach,	uhid_detach),

	{ 0, 0 }
};

static driver_t uhid_driver = {
	"uhid",
	uhid_methods,
	sizeof(struct uhid_softc)
};

static devclass_t uhid_devclass;

DRIVER_MODULE(uhid, hidbus, uhid_driver, uhid_devclass, NULL, 0);
MODULE_DEPEND(uhid, hidbus, 1, 1, 1);
MODULE_DEPEND(uhid, hid, 1, 1, 1);
MODULE_VERSION(uhid, 1);

static void
uhid_identify(driver_t *driver, device_t parent)
{
	device_t child;

	if (device_find_child(parent, "uhid", -1) == NULL) {
		child = BUS_ADD_CHILD(parent, 0, "uhid", -1);
		hidbus_set_index(child, UHID_INDEX);
	}
}

static int
uhid_match(device_t self)
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
uhid_attach(device_t self)
{
	struct uhid_softc *sc = device_get_softc(self);
	uint16_t size;
	void *desc;
	int error;

	sc->sc_dev = self;

	error = hid_get_report_descr(sc->sc_dev, &desc, &size);
	if (error) {
		printf("%s: no report descriptor\n", device_get_nameunit(sc->sc_dev));
		sc->sc_dying = 1;
		return ENXIO;
	}

	sc->sc_isize = hid_report_size(desc, size, hid_input,   &sc->sc_iid);
	sc->sc_osize = hid_report_size(desc, size, hid_output,  &sc->sc_oid);
	sc->sc_fsize = hid_report_size(desc, size, hid_feature, &sc->sc_fid);

	sc->sc_repdesc = desc;
	sc->sc_repdesc_size = size;
	sc->dev = make_dev(&uhid_cdevsw, device_get_unit(self),
			UID_ROOT, GID_OPERATOR,
			0644, "uhid%d", device_get_unit(self));
	sc->dev->si_drv1 = sc;

	hidbus_set_intr(sc->sc_dev, uhid_intr);

	return 0;
}

static int
uhid_detach(device_t self)
{
	struct uhid_softc *sc = device_get_softc(self);

	DPRINTF(("uhid_detach: sc=%p\n", sc));
	sc->sc_dying = 1;

	mtx_lock(hidbus_get_lock(self));
	if (sc->sc_state & UHID_OPEN) {
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
uhid_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct uhid_softc *sc = device_get_softc(dev);

#ifdef USB_DEBUG
	if (uhiddebug > 5) {
		u_int32_t i;

		DPRINTF(("uhid_intr: len=%d\n", len));
		DPRINTF(("uhid_intr: data ="));
		for (i = 0; i < len; i++)
			DPRINTF((" %02x", ((uint8_t *)buf)[i]));
		DPRINTF(("\n"));
	}
#endif

	(void) b_to_q(buf, sc->sc_isize, &sc->sc_q);

	if (sc->sc_state & UHID_ASLP) {
		sc->sc_state &= ~UHID_ASLP;
		DPRINTFN(5, ("uhid_intr: waking %p\n", &sc->sc_q));
		wakeup(&sc->sc_q);
	}
	selwakeuppri(&sc->sc_rsel, PZERO);
#ifdef NOT_YET
	if (sc->sc_async != NULL) {
		DPRINTFN(3, ("uhid_intr: sending SIGIO %p\n", sc->sc_async));
		PROC_LOCK(sc->sc_async);
		psignal(sc->sc_async, SIGIO);
		PROC_UNLOCK(sc->sc_async);
	}
#endif
}

int
uhidopen(struct cdev *dev, int flag, int mode, struct thread *p)
{
	struct uhid_softc *sc;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	DPRINTF(("uhidopen: sc=%p\n", sc));

	if (sc->sc_dying)
		return (ENXIO);

	if (sc->sc_state & UHID_OPEN)
		return (EBUSY);
	sc->sc_state |= UHID_OPEN;

	clist_alloc_cblocks(&sc->sc_q, UHID_BSIZE, UHID_BSIZE);
	sc->sc_ibuf = malloc(sc->sc_isize, M_USBDEV, M_WAITOK);
	sc->sc_obuf = malloc(sc->sc_osize, M_USBDEV, M_WAITOK);

	/* Set up interrupt pipe. */
	mtx_lock(hidbus_get_lock(sc->sc_dev));
	hidbus_intr_start(sc->sc_dev);
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

	sc->sc_state &= ~UHID_IMMED;

#ifdef NOT_YET
	sc->sc_async = 0;
#endif

	return (0);
}

int
uhidclose(struct cdev *dev, int flag, int mode, struct thread *p)
{
	struct uhid_softc *sc;

	sc = dev->si_drv1;

	DPRINTF(("uhidclose: sc=%p\n", sc));

	/* Disable interrupts. */
	mtx_lock(hidbus_get_lock(sc->sc_dev));
	hidbus_intr_stop(sc->sc_dev);
	mtx_unlock(hidbus_get_lock(sc->sc_dev));

	ndflush(&sc->sc_q, sc->sc_q.c_cc);
	clist_free_cblocks(&sc->sc_q);

	free(sc->sc_ibuf, M_USBDEV);
	free(sc->sc_obuf, M_USBDEV);
	sc->sc_ibuf = sc->sc_obuf = NULL;

	sc->sc_state &= ~UHID_OPEN;

#ifdef NOT_YET
	sc->sc_async = 0;
#endif

	return (0);
}

int
uhid_do_read(struct uhid_softc *sc, struct uio *uio, int flag)
{
	int error = 0;
	size_t length;
	u_char buffer[UHID_CHUNK];

	DPRINTFN(1, ("uhidread\n"));
	if (sc->sc_state & UHID_IMMED) {
		DPRINTFN(1, ("uhidread immed\n"));

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
		sc->sc_state |= UHID_ASLP;
		DPRINTFN(5, ("uhidread: sleep on %p\n", &sc->sc_q));
		error = mtx_sleep(&sc->sc_q, hidbus_get_lock(sc->sc_dev),
		    PZERO | PCATCH, "uhidrea", 0);
		DPRINTFN(5, ("uhidread: woke, error=%d\n", error));
		if (sc->sc_dying)
			error = EIO;
		if (error) {
			sc->sc_state &= ~UHID_ASLP;
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
		DPRINTFN(5, ("uhidread: got %lu chars\n", (u_long)length));

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
uhidread(struct cdev *dev, struct uio *uio, int flag)
{
	struct uhid_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = uhid_do_read(sc, uio, flag);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
uhid_do_write(struct uhid_softc *sc, struct uio *uio, int flag)
{
	int error;
	int size;

	DPRINTFN(1, ("uhidwrite\n"));

	if (sc->sc_dying)
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
uhidwrite(struct cdev *dev, struct uio *uio, int flag)
{
	struct uhid_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = uhid_do_write(sc, uio, flag);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
uhid_do_ioctl(struct uhid_softc *sc, u_long cmd, caddr_t addr, int flag,
	      struct thread *p)
{
	struct usb_gen_descriptor *ugd;
	void *buf;
	int size, id;
	int error;

	DPRINTFN(2, ("uhidioctl: cmd=%lx\n", cmd));

	if (sc->sc_dying)
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
			DPRINTF(("uhid_do_ioctl: FIOASYNC %p\n", sc->sc_async));
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
			sc->sc_state |=  UHID_IMMED;
			mtx_unlock(hidbus_get_lock(sc->sc_dev));
		} else {
			mtx_lock(hidbus_get_lock(sc->sc_dev));
			sc->sc_state &= ~UHID_IMMED;
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
uhidioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, struct thread *p)
{
	struct uhid_softc *sc;
	int error;

	sc = dev->si_drv1;
	sc->sc_refcnt++;
	error = uhid_do_ioctl(sc, cmd, addr, flag, p);
	if (--sc->sc_refcnt < 0)
		{} /*usb_detach_wakeup(sc->sc_dev);*/
	return (error);
}

int
uhidpoll(struct cdev *dev, int events, struct thread *p)
{
	struct uhid_softc *sc;
	int revents = 0;

	sc = dev->si_drv1;
	if (sc->sc_dying)
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
