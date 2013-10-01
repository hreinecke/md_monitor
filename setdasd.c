/*
 * setdasd.c
 *
 * Set and unset DASD ioctl flags
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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <getopt.h>
#include <errno.h>

#include <libudev.h>

#include "md_debug.h"
#include "dasd_ioctl.h"

static int log_priority = LOG_INFO;

void log_fn(int priority, const char *format, ...)
{
	va_list ap;

	if (log_priority < priority)
		return;

	va_start(ap, format);
	if (priority == LOG_ERR)
		vfprintf(stderr, format, ap);
	else
		vfprintf(stdout, format, ap);
	va_end(ap);
}

void usage(void)
{
	err("Usage: setdasd [--timeout=<set>|-t <set>] "
	    "[--quiesce=<set>|-q <set>]"
	    "[--device=<devnode>|-d <devnode>] "
	    "[--sysfs=<devpath>|-s <devnode>] "
	    "[--log-priority=<prio>|-p <prio>]\n"
	    "  --timeout=<set>                set or unset timeout ioctl\n"
	    "  --quiesce=<set>                quiesce or resume ioctl\n"
	    "  --device=<devnode>             use device node <devno>\n"
	    "  --sysfs=<devpath>              use sysfs path <devpath>\n"
	    "  --log-priority=<prio>          set logging priority to <prio>\n"
	    "  --verbose                      increase logging priority\n"
	    "  --help\n");
}

int main(int argc, char **argv)
{
	struct udev *udev;
	struct udev_device *dev = NULL;
	int prio, option, rc;
	char *devpath = NULL;
	char *devnode = NULL;
	char *p;
	unsigned int timeout = -1;
	unsigned int quiesce = -1;

	static const struct option options[] = {
		{ "timeout", required_argument, NULL, 't' },
		{ "quiesce", required_argument, NULL, 'q' },
		{ "device", required_argument, NULL, 'd' },
		{ "sysfs", required_argument, NULL, 's' },
		{ "log-priority", required_argument, NULL, 'p' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};

	udev = udev_new();
	if (!udev)
		exit(1);

	while (1) {
		option = getopt_long(argc, argv, "d:p:q:s:t:vh",
				     options, NULL);
		if (option == -1) {
			break;
		}

		switch (option) {
		case 'd':
			devnode = optarg;
			break;
		case 's':
			devpath = optarg;
			break;
		case 'p':
			prio = strtoul(optarg, &p, 10);
			if (optarg == p) {
				err("Invalid logging priority '%s'",
				    optarg);
				exit(1);
			} else if (prio > LOG_DEBUG) {
				err("Invalid logging priority %d (max %d)",
				    prio, LOG_DEBUG);
				exit(1);
			}
			log_priority = prio;
			break;
		case 'v':
			if (log_priority < LOG_DEBUG)
				log_priority++;
			break;
		case 'q':
			quiesce = strtoul(optarg, &p, 10);
			if (optarg == p) {
				err("Invalid 'quiesce' parameter '%s'",
				    optarg);
				exit(1);
			} else if (quiesce > 2) {
				err("Invalid 'quiesce' parameter '%d', "
				    "only '0' and '1' are allowed", quiesce);
			}
			break;
		case 't':
			timeout = strtoul(optarg, &p, 10);
			if (optarg == p) {
				err("Invalid 'timeout' parameter '%s'",
				    optarg);
				exit(1);
			} else if (timeout > 2) {
				err("Invalid 'timeout' parameter '%d', "
				    "only '0' and '1' are allowed", timeout);
			}
			break;
		case 'h':
			usage();
			exit(0);
			break;
		default:
			usage();
			exit(1);
		}
	}
	if (devpath && devnode) {
		err("Cannot specify both --device and --sysfs");
		usage();
		exit(1);
	}
	if (timeout == -1 && quiesce == -1) {
		err("Need to specify --timeout or --quiesce");
		usage();
		exit(1);
	}

	if (devnode) {
		p = strrchr(devnode, '/');
		if (!p)
			p = devnode;
		else
			p++;
		dbg("Looking for 'block' '%s'", p);
		dev = udev_device_new_from_subsystem_sysname(udev, "block", p);
		if (!dev) {
			err("Device node '%s' not found", devnode);
			rc = -ENXIO;
			goto out;
		}
	}
	if (devpath) {
		if (!strncmp(devpath, "/sys", 4)) {
			p = devpath + 5;
		} else {
			p = devpath;
		}
		dev = udev_device_new_from_syspath(udev, devpath);
		if (!dev) {
			err("Sysfs path '%s' not found", devpath);
			rc = -ENXIO;
			goto out;
		}
	}
	if (!dev) {
		err("No device specified");
		usage();
		rc = -EINVAL;
		goto out;
	} else {
		const char *devname = udev_device_get_sysname(dev);
		if (!devname || strncmp(devname, "dasd", 4)) {
			err("Device '%s' is not a DASD", devname);
			rc = -ENOSYS;
			goto out;
		}
	}
	if (quiesce == 0 || quiesce == 1)
		rc = dasd_quiesce_ioctl(dev, quiesce);
	else
		rc = dasd_timeout_ioctl(dev, timeout);
	udev_device_unref(dev);
out:
	udev_unref(udev);
	return rc < 0 ? 1 : 0;
}
