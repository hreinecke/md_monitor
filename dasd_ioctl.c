/*
 * dasd_ioctl.c
 *
 * Copyright (C) 2013 Hannes Reinecke <hare@suse.de>
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

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>

#include "libudev.h"

#include "md_debug.h"
#include "dasd_ioctl.h"

#ifndef DASD_IOCTL_LETTER
 #define DASD_IOCTL_LETTER 'D'
  #ifndef BIODASDTIMEOUT
  /* TIMEOUT IO on device */
  #define BIODASDTIMEOUT _IO(DASD_IOCTL_LETTER,240)
 #endif
 #ifndef BIODASDRESYNC
 /* Resume IO on device */
 #define BIODASDRESYNC  _IO(DASD_IOCTL_LETTER,241)
 #endif
 #ifndef BIODASDQUIESCE
 /* Quiesce IO on device */
 #define BIODASDQUIESCE  _IO(DASD_IOCTL_LETTER,6)
 #endif
 #ifndef BIODASDRESUME
 /* Resume IO on device */
 #define BIODASDRESUME  _IO(DASD_IOCTL_LETTER,7)
 #endif
#endif

int dasd_timeout_ioctl(struct udev_device *dev, int set)
{
	int ioctl_arg = set ? BIODASDTIMEOUT : BIODASDRESYNC;
	int ioctl_fd;
	const char *devname;
	char devnode[256];
	int rc = 0;

	if (!dev)
		return -EINVAL;

	devname = udev_device_get_sysname(dev);
	if (!devname)
		return -ENXIO;

	devnode[0] = '\0';
	if (udev_device_get_devnode(dev)) {
		strcpy(devnode, udev_device_get_devnode(dev));
	} else {
		sprintf(devnode, "/dev/%s", devname);
	}
	dbg("%s: calling DASD ioctl '%s'", devname,
	    set ? "BIODASDTIMEOUT" : "BIODASDRESYNC");
	if (!strlen(devnode)) {
		warn("%s: device node not found", devname);
		return -ENXIO;
	}
	ioctl_fd = open(devnode, O_RDWR);
	if (ioctl_fd < 0) {
		warn("%s: cannot open %s for DASD ioctl: %m",
		     devname, devnode);
		rc = -errno;
	} else {
		if (ioctl(ioctl_fd, ioctl_arg) < 0) {
			info("%s: cannot %s DASD timeout flag: %m",
			     devname, set ? "set" : "unset");
			rc = -errno;
		} else {
			dbg("%s: %s DASD timeout flag", devname,
			    set ? "set" : "unset");
		}
	}
	close(ioctl_fd);
	return rc;
}

int dasd_quiesce_ioctl(struct udev_device *dev, int set)
{
	int ioctl_arg = set ? BIODASDQUIESCE : BIODASDRESUME;
	int ioctl_fd;
	const char *devname;
	const char *devnode;
	int rc = 0;

	if (!dev)
		return -EINVAL;

	devnode = udev_device_get_devnode(dev);
	devname = udev_device_get_sysname(dev);
	dbg("%s: calling DASD ioctl '%s'", devname,
	    set ? "BIODASDQUIESCE" : "BIODASDRESUME");
	if (!devnode) {
		warn("%s: device removed", devname);
		return -ENXIO;
	}
	ioctl_fd = open(devnode, O_RDWR);
	if (ioctl_fd < 0) {
		warn("%s: cannot open %s for DASD ioctl: %m",
		     devname, devnode);
		rc = -errno;
	} else {
		if (ioctl(ioctl_fd, ioctl_arg) < 0) {
			info("%s: cannot %s DASD: %m",
			     devname, set ? "quiesce" : "resume");
			rc = -errno;
		} else {
			dbg("%s: DASD %s", devname,
			    set ? "quiesced" : "resumed");
		}
	}
	close(ioctl_fd);
	return rc;
}

