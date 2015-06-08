/*
 * DASD Monitor
 *
 * Monitor DASD devices when MD is active
 *
 * Copyright (C) 2011-2013 Hannes Reinecke <hare@suse.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <syslog.h>

#include <libudev.h>
#include <libaio.h>

#include "list.h"
#include "md_monitor.h"
#include "md_debug.h"
#include "dasd_ioctl.h"

int dasd_set_attribute(struct device_monitor *dev, const char *attr, int value)
{
	struct udev_device *parent;
	int attr_fd;
	char attrpath[256];
	char status[64], *eptr;
	ssize_t len, status_len = 64;
	int oldvalue;
	int rc = 0;

	parent = udev_device_get_parent(dev->device);
	if (!parent)
		return 0;
	sprintf(attrpath, "%s/%s", udev_device_get_syspath(parent), attr);
	attr_fd = open(attrpath, O_RDWR);
	if (attr_fd < 0) {
		info("%s: failed to open '%s' attribute for %s: %m",
		     dev->dev_name, attr, attrpath);
		return 0;
	}

	memset(status, 0, status_len);
	len = read(attr_fd, status, status_len);
	if (len < 0) {
		warn("%s: cannot read '%s' attribute: %m",
		     dev->dev_name, attr);
		rc = errno;
		goto remove;
	}
	if (len == 0) {
		warn("%s: EOF on reading '%s' attribute", dev->dev_name, attr);
		goto remove;
	}
	if (len == status_len) {
		warn("%s: Overflow on reading '%s' attribute",
		     dev->dev_name, attr);
		goto remove;
	}
	if (status[len - 1] == '\n') {
		status[len - 1] = '\0';
		len--;
	}
	status[status_len - 1] = '\0';

	if (!strlen(status)) {
		warn("%s: empty '%s' attribute", dev->dev_name, attr);
		goto remove;
	}
	oldvalue = strtoul(status, &eptr, 10);
	if (status == eptr) {
		warn("%s: invalid '%s' attribute value '%s'",
		     dev->dev_name, attr, status);
		goto remove;
	}
	if (oldvalue != value) {
		sprintf(status, "%d", value);
		len = write(attr_fd, status, strlen(status));
		if (len < 0) {
			warn("%s: cannot set '%s' attribute to '%s': %m",
			     dev->dev_name, attr, status);
			rc = errno;
		}
		info("%s: '%s' = '%s'", dev->dev_name, attr, status);
		rc = 0;
	}
remove:
	if (attr_fd >= 0)
		close(attr_fd);
	return rc;
}

int dasd_setup_aio(struct device_monitor *dev)
{
	const char *devnode;
	char devnode_s[256];
	int rc, flags;

	dev->aio_active = 0;
	devnode = udev_device_get_devnode(dev->device);
	if (!devnode) {
		warn("%s: no device node from udev", dev->dev_name);
		sprintf(devnode_s, "/dev/%s", dev->dev_name);
		devnode = devnode_s;
	}
	if (!strlen(devnode)) {
		warn("%s: no device node found", dev->dev_name);
		return ENXIO;
	}
	rc = io_setup(1, &dev->ioctx);
	if (rc != 0) {
		warn("%s: io_setup failed with %d",
		     dev->dev_name, -rc);
		dev->ioctx = 0;
		return -rc;
	}
	dev->fd = open(devnode, O_RDONLY);
	if (dev->fd  < 0) {
		warn("%s: cannot open %s for aio: %m",
		     dev->dev_name, devnode);
		return errno;
	}
	flags = fcntl(dev->fd, F_GETFL);
	if (flags < 0) {
		warn("%s: fcntl GETFL failed: %m", dev->dev_name);
		return errno;
	}

	if (!(flags & O_DIRECT)) {
		flags |= O_DIRECT;
		rc = fcntl(dev->fd, F_SETFL, flags);
	}

	if (ioctl(dev->fd, BLKBSZGET, &dev->blksize) < 0)
		dev->blksize = 512;

	if (dev->blksize > 4096) {
		/*
		 * Sanity check for DASD; BSZGET is broken
		 */
		dev->blksize = 4096;
	}
	return 0;
}

void dasd_cleanup_aio(struct device_monitor *dev)
{
	int rc;

	if (dev->ioctx) {
		rc = io_destroy(dev->ioctx);
		if (rc) {
			warn("%s: io_destroy failed with %d",
			     dev->dev_name, rc);
		}
		dev->ioctx = 0;
		dev->aio_active = 0;
	}
	if (dev->fd >= 0) {
		if (!strncmp(dev->dev_name, "dasd", 4)) {
			/* Reset any stale ioctl flags */
			dasd_timeout_ioctl(dev->device, 0);
		}
		close(dev->fd);
		dev->fd = -1;
	}
}

enum device_io_status dasd_check_aio(struct device_monitor *dev, int timeout)
{
	struct iocb *ios[1] = { &dev->io };
	unsigned long pgsize = getpagesize();
	unsigned char *ioptr;
	struct io_event event;
	struct timespec	tmo = { .tv_sec = timeout };
	int rc;
	enum device_io_status io_status = IO_UNKNOWN;
	sigset_t newmask, oldmask;
	struct sigaction act, oact;

	if (!dev->ioctx) {
		dbg("%s: no io context", dev->dev_name);
		return io_status;
	}

	if (timeout && !dev->aio_active) {
		info("%s: start new request",
		     dev->dev_name);
		memset(&dev->io, 0, sizeof(struct iocb));
		ioptr = (unsigned char *) (((unsigned long)dev->buf +
					    pgsize - 1) & (~(pgsize - 1)));
		io_prep_pread(&dev->io, dev->fd, ioptr,
			      4096, 0);
		if (gettimeofday(&dev->aio_start_time, NULL)) {
			warn("%s: failed to get time: %m", dev->dev_name);
			dev->aio_start_time.tv_sec = 0;
		}
		if (io_submit(dev->ioctx, 1, ios) != 1) {
			warn("%s: io_submit failed: %m", dev->dev_name);
			return IO_ERROR;
		}
		dev->aio_active = 1;
	}
	/* Unblock SIGHUP */
	memset(&act, 0x00, sizeof(struct sigaction));
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &act, &oact);
	sigemptyset(&newmask);
	sigaddset(&newmask, SIGHUP);
	pthread_sigmask(SIG_UNBLOCK, &newmask, &oldmask);
	errno = 0;
	rc = io_getevents(dev->ioctx, 1L, 1L, &event, &tmo);
	sigaction(SIGHUP, &oact, NULL);
	pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
	if (rc < 0) {
		if (rc != -EINTR) {
			info("%s: async io returned %d",
			     dev->dev_name, rc);
			rc = io_cancel(dev->ioctx, ios[0], &event);
			if (rc < 0) {
				warn("%s: io_cancel returned %d",
				     dev->dev_name, rc);
			}
			dev->aio_active = 0;
			io_status = IO_ERROR;
		} else {
			info("%s: io_getevents interrupted", dev->dev_name);
			io_status = IO_PENDING;
		}
	} else if (rc < 1L) {
		if (timeout) {
			warn("%s: path timeout", dev->dev_name);
			io_status = IO_TIMEOUT;
		} else if (dev->aio_active) {
			dbg("%s: no events", dev->dev_name);
			io_status = IO_PENDING;
		} else {
			dbg("%s: no async io submitted", dev->dev_name);
			io_status = IO_UNKNOWN;
		}
	} else {
		struct timeval diff, end_time;

		if (dev->aio_start_time.tv_sec &&
		    gettimeofday(&end_time, NULL) == 0) {
			timersub(&end_time, &dev->aio_start_time, &diff);
		} else {
			diff.tv_sec = 0;
			diff.tv_usec = 0;
		}
		dev->aio_active = 0;
		if (event.res != dev->blksize) {
			warn("%s: path failed, %lu.%06lu secs", dev->dev_name,
			     diff.tv_sec, diff.tv_usec);
			io_status = IO_FAILED;
		} else {
			info("%s: path ok, %lu.%06lu secs", dev->dev_name,
			     diff.tv_sec, diff.tv_usec);
			io_status = IO_OK;
		}
	}
	return io_status;
}
