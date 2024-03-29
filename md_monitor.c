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
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <limits.h>

#include <poll.h>
#include <linux/netlink.h>
#include <linux/major.h>
#include <linux/raid/md_p.h>
#include <linux/raid/md_u.h>
/* Disk timeout might not be defined */
#ifndef MD_DISK_TIMEOUT
#define MD_DISK_TIMEOUT 11
#endif
#include <pthread.h>
#include <dirent.h>
#include <libaio.h>

#include <libudev.h>

#include "list.h"
#include "md_monitor.h"
#include "md_debug.h"
#include "mpath_util.h"
#include "dasd_util.h"
#include "dasd_ioctl.h"

const char version_str[] = "md_monitor version 6.6";

LIST_HEAD(md_list);
LIST_HEAD(device_list);
LIST_HEAD(pending_list);
pthread_mutex_t md_lock;
pthread_mutex_t device_lock;
pthread_mutex_t pending_lock;
pthread_cond_t pending_cond;
pthread_attr_t monitor_attr;
pthread_attr_t cli_attr;

static int udev_exit;
static int monitor_timeout;
static int failfast_timeout = 5;
static int failfast_retries = 2;
sigset_t thread_sigmask;
static int daemonize_monitor;
static int log_priority = LOG_INFO;
static char logname[MD_NAMELEN];
static int use_syslog;
static int fail_mirror_side = 1;
static int stop_on_sync = 1;
static unsigned long checker_timeout = 1;
static pid_t monitor_pid;
FILE *logfd;

void log_fn(int priority, const char *format, ...)
{
	va_list ap;
	time_t curtime;
	struct tm *cur_tm;

	if (log_priority < priority)
		return;

	va_start(ap, format);

	if (use_syslog)
		vsyslog(priority, format, ap);
	else {
		char timestr[32];

		time(&curtime);
		cur_tm = gmtime(&curtime);
		strftime(timestr, 32, "%a %d %T ", cur_tm);
		fprintf(logfd, "%s", timestr);
		vfprintf(logfd, format, ap);
		fflush(logfd);
	}
	va_end(ap);
}

struct md_rdev_state_t {
	enum md_rdev_status state;
	char desc_short;
	char *desc;
} md_rdev_state_list[] = {
	{ UNKNOWN, '.', "unknown"},
	{ IN_SYNC, 'A', "in_sync"},
	{ FAULTY, 'W', "faulty"},
	{ TIMEOUT, 'T', "timeout"},
	{ SPARE, 'S', "spare"},
	{ RECOVERY, 'R', "recovery"},
	{ REMOVED, '-', "removed"},
	{ PENDING, 'P', "pending"},
	{ BLOCKED, 'B', "blocked"},
	{ STOPPED, 'X', "stopped"},
	{ RESERVED, 0, NULL }
};

char *md_rdev_print_state(enum md_rdev_status state)
{
	struct md_rdev_state_t *md_rdev_state_ptr = md_rdev_state_list;

	while (md_rdev_state_ptr->desc &&
	       md_rdev_state_ptr->state != state)
		md_rdev_state_ptr++;
	return md_rdev_state_ptr->desc;
}

const char md_rdev_print_state_short(enum md_rdev_status state)
{
	struct md_rdev_state_t *md_rdev_state_ptr = md_rdev_state_list;

	while (md_rdev_state_ptr->desc_short &&
	       md_rdev_state_ptr->state != state)
		md_rdev_state_ptr++;
	return md_rdev_state_ptr->desc_short;
}

struct device_io_state_t {
	enum device_io_status state;
	char desc_short;
	char *desc;
} device_io_state_list[] = {
	{ IO_UNKNOWN, '.', "unknown"},
	{ IO_ERROR, 'X', "internal error"},
	{ IO_OK, 'A', "I/O ok"},
	{ IO_FAILED, 'W', "I/O failed"},
	{ IO_PENDING, 'R', "I/O pending"},
	{ IO_TIMEOUT, 'T', "I/O timeout"},
	{ IO_RETRY, 'I', "I/O retry"},
	{ IO_RESERVED, 0, NULL }
};

char *device_io_print_state(enum device_io_status state)
{
	struct device_io_state_t *device_io_state_ptr = device_io_state_list;

	while (device_io_state_ptr->desc &&
	       device_io_state_ptr->state != state)
		device_io_state_ptr++;
	return device_io_state_ptr->desc;
}

const char device_io_print_state_short(enum device_io_status state)
{
	struct device_io_state_t *device_io_state_ptr = device_io_state_list;

	while (device_io_state_ptr->desc_short &&
	       device_io_state_ptr->state != state)
		device_io_state_ptr++;
	return device_io_state_ptr->desc_short;
}

static void add_component(struct md_monitor *, struct device_monitor *,
			  const char *);
static void remove_component(struct device_monitor *);
static int fail_component(struct device_monitor *, enum md_rdev_status);
static int reset_component(struct device_monitor *);
static void fail_mirror(struct device_monitor *, enum md_rdev_status);
static void reset_mirror(struct device_monitor *);
static void discover_md_components(struct md_monitor *md);
static void remove_md_component(struct md_monitor *md_dev,
				struct device_monitor *dev);
static void monitor_device(struct device_monitor *);

struct timeval start_time, sum_time;
int num_time = 0;

static void lock_device_list(void)
{
	pthread_mutex_lock(&device_lock);
	if (gettimeofday(&start_time, NULL) != 0)
		start_time.tv_sec = 0;
}

static void unlock_device_list(void)
{
	struct timeval end_time, diff_time;

	if (start_time.tv_sec &&
	    gettimeofday(&end_time, NULL) == 0) {
		timersub(&end_time, &start_time, &diff_time);
		timeradd(&sum_time, &diff_time, &sum_time);
		num_time++;
	}
	pthread_mutex_unlock(&device_lock);
}

void sig_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		udev_exit = 1;
}

static struct md_monitor *lookup_md(const char *mdname, int remove)
{
	struct md_monitor *tmp, *md = NULL;

	if (!mdname)
		return NULL;

	pthread_mutex_lock(&md_lock);
	list_for_each_entry(tmp, &md_list, entry) {
		const char *tmpname = udev_device_get_sysname(tmp->device);
		if (!strcmp(tmp->dev_name, mdname)) {
			md = tmp;
			break;
		}
		if (!strcmp(tmpname, mdname)) {
			md = tmp;
			break;
		}
	}
	if (remove && md)
		list_del_init(&md->entry);
	pthread_mutex_unlock(&md_lock);
	return md;
}

static struct md_monitor *lookup_md_alias(const char *mdpath)
{
	struct md_monitor *tmp, *md = NULL;
	const char *mdname;
	const char *tmpname;

	if (!mdpath || strlen(mdpath) == 0)
		return NULL;

	mdname = strrchr(mdpath,'/');
	if (!mdname)
		mdname = mdpath;
	else
		mdname++;

	pthread_mutex_lock(&md_lock);
	list_for_each_entry(tmp, &md_list, entry) {
		if (strlen(tmp->dev_name) && !strcmp(tmp->dev_name, mdname)) {
			md = tmp;
			break;
		}
		tmpname = udev_device_get_property_value(tmp->device,
							 "MD_DEVNAME");
		if (tmpname && !strcmp(tmpname, mdname)) {
			md = tmp;
			break;
		}
		tmpname = udev_device_get_sysname(tmp->device);
		if (tmpname && !strcmp(tmpname, mdname)) {
			md = tmp;
			break;
		}
	}
	pthread_mutex_unlock(&md_lock);
	return md;
}

static struct device_monitor * lookup_md_component(struct md_monitor *md_dev,
						   const char *devname)
{
	struct device_monitor *tmp, *found = NULL;
	int lookup_symlinks = 0;

	if (!md_dev)
		return NULL;

	if (!devname)
		return NULL;
	if (strncmp(devname, "dasd", 4) &&
	    strncmp(devname, "dm-", 3)) {
		lookup_symlinks = 1;
	}
	pthread_mutex_lock(&md_dev->status_lock);
	if (!md_dev->device) {
		pthread_mutex_unlock(&md_dev->status_lock);
		return NULL;
	}
	pthread_mutex_unlock(&md_dev->status_lock);
	pthread_mutex_lock(&md_dev->device_lock);
	list_for_each_entry(tmp, &md_dev->children, siblings) {
		/* No locking required, tmp->device is static */
		if (lookup_symlinks) {
			struct udev_list_entry *entry;
			const char *tmpname, *ptr;

			udev_list_entry_foreach(entry,
				udev_device_get_devlinks_list_entry(tmp->device)) {
				tmpname = udev_list_entry_get_name(entry);
				ptr = strrchr(tmpname, '/');
				if (!ptr) {
					info("%s: invalid symlink %s",
					     md_dev->dev_name, tmpname);
					continue;
				}
				ptr++;
				if (!strncmp(ptr, devname, strlen(ptr))){
					found = tmp;
					goto out;
				}
			}
		}
		pthread_mutex_lock(&tmp->lock);
		if (!strncmp(devname, tmp->md_name,
			     strlen(devname))) {
			found = tmp;
			pthread_mutex_unlock(&tmp->lock);
			break;
		}
		if (!strncmp(devname, tmp->dev_name,
			     strlen(devname))) {
			found = tmp;
			pthread_mutex_unlock(&tmp->lock);
			break;
		}
		pthread_mutex_unlock(&tmp->lock);
	}
out:
	pthread_mutex_unlock(&md_dev->device_lock);
	return found;
}

static struct md_monitor *lookup_md_new(struct udev_device *md_dev)
{
	const char *mdname = udev_device_get_sysname(md_dev);
	const char *alias_name;
	struct md_monitor *tmp, *md = NULL;

	alias_name = udev_device_get_property_value(md_dev, "MD_DEVICE");
	pthread_mutex_lock(&md_lock);
	list_for_each_entry(tmp, &md_list, entry) {
		const char *tmpname;

		/* Check if the alias matches (if set in the properties) */
		if (alias_name && !strcmp(tmp->dev_name, alias_name)) {
			md = tmp;
			break;
		}
		/* Check if the name matches the stored device name */
		if (!strcmp(tmp->dev_name, mdname)) {
			md = tmp;
			break;
		}
		/* Check if the name matches the actual device name */
		if (!tmp->device)
			continue;
		tmpname = udev_device_get_sysname(tmp->device);
		if (tmpname && !strcmp(tmpname, mdname)) {
			md = tmp;
			break;
		}
	}
	if (!md) {
		md = malloc(sizeof(struct md_monitor));
		if (md)
			memset(md, 0, sizeof(struct md_monitor));
		else
			goto out_unlock;
		if (alias_name)
			mdname = alias_name;
		if (strlen(mdname) > MD_NAMELEN) {
			warn("%s: MD name overflow, truncated", mdname);
		}
		strncpy(md->dev_name, mdname, MD_NAMELEN);
		md->dev_name[MD_NAMELEN - 1] = '\0';
		md->raid_disks = -1;
		INIT_LIST_HEAD(&md->children);
		INIT_LIST_HEAD(&md->pending);
		pthread_mutex_init(&md->status_lock, NULL);
		pthread_mutex_init(&md->device_lock, NULL);
		list_add(&md->entry, &md_list);
		info("%s: create new array", mdname);
	}
	if (!md->device) {
		md->device = md_dev;
		udev_device_ref(md_dev);
	}
out_unlock:
	pthread_mutex_unlock(&md_lock);
	return md;
}

struct device_monitor *lookup_device_devname(const char *devname)
{
	struct device_monitor *tmp, *found = NULL;

	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		if (!strlen(tmp->dev_name))
			continue;
		if (!strcmp(tmp->dev_name, devname)) {
			found = tmp;
			break;
		}
	}
	unlock_device_list();

	return found;
}

static struct device_monitor *device_monitor_get(struct device_monitor *dev)
{
	if (!dev)
		return NULL;

	pthread_mutex_lock(&dev->lock);
	dev->ref++;
	pthread_mutex_unlock(&dev->lock);

	return dev;
}

static void device_monitor_put(struct device_monitor *dev)
{
	if (!dev)
		return;

	pthread_mutex_lock(&dev->lock);
	dev->ref--;
	if (dev->ref == 0) {
		udev_device_unref(dev->device);
		dev->device = NULL;
		pthread_mutex_unlock(&dev->lock);
		pthread_mutex_destroy(&dev->lock);
		pthread_cond_destroy(&dev->io_cond);
		free(dev);
		return;
	}
	pthread_mutex_unlock(&dev->lock);
}

static struct device_monitor *allocate_device(struct udev_device *udev_dev)
{
	struct device_monitor *dev;
	const char *devname;

	devname = udev_device_get_sysname(udev_dev);
	if (strlen(devname) > MD_NAMELEN) {
		warn("%s: DASD device name too long", devname);
		return NULL;
	}
	dev = malloc(sizeof(struct device_monitor));
	if (!dev) {
		err("%s: out of memory allocating device", devname);
		return NULL;
	}
	memset(dev, 0, sizeof(struct device_monitor));
	dev->device = udev_dev;
	dev->ref = 1;
	dev->md_slot = dev->md_slot_saved = -1;
	dev->md_index = -1;
	dev->md_side = -1;
	dev->io_status = IO_UNKNOWN;
	pthread_mutex_init(&dev->lock, NULL);
	pthread_cond_init(&dev->io_cond, NULL);
	INIT_LIST_HEAD(&dev->siblings);
	udev_device_ref(dev->device);
	strcpy(dev->dev_name, devname);

	return dev;
}

static void attach_device(struct udev_device *udev_dev)
{
	struct md_monitor *found_md = NULL;
	struct device_monitor *tmp, *found = NULL;
	struct udev_device *raid_dev;
	const char *devtype, *alias, *status;
	const char *devname = udev_device_get_sysname(udev_dev);
	char devpath[256];
	DIR *dirp;
	struct dirent *dirfd;
	dev_t udev_devt, tmp_devt, raid_devt;

	devtype = udev_device_get_devtype(udev_dev);
	udev_devt = udev_device_get_devnum(udev_dev);
	dbg("dev %s devtype %s", devname, devtype);
	if (!strncmp(devname, "dasd", 4)) {
		if (!devtype || !strncmp(devtype, "disk", 4)) {
			/* Not a partition, ignore */
			info("%s: not a partition, ignore", devname);
			return;
		}
		raid_dev = udev_device_get_parent(udev_dev);
		raid_devt = udev_device_get_devnum(raid_dev);
		status = udev_device_get_sysattr_value(raid_dev, "status");
		info("%s: device in state %s", devname, status);
		if (status && strcmp(status, "online")) {
			/*
			 * Device not online. The kernel will send out
			 * an event once the device is online, so
			 * we can safely skip it here.
			 */
			info("%s: device in state %s, ignore", devname, status);
			return;
		}
		alias = udev_device_get_sysattr_value(raid_dev, "alias");
		if (alias && alias[0] == '1') {
			/* Alias device, ignore */
			info("%s: aliased device, ignore", devname);
			return;
		}
	} else if (!strncmp(devname, "dm-", 3)) {
		const char *uuid;

		raid_dev = udev_dev;
		raid_devt = udev_devt;
		uuid = udev_device_get_sysattr_value(udev_dev, "dm/uuid");
		if (!uuid) {
			warn("%s: no device-mapper uuid, ignore", devname);
			return;
		}
		if (strncmp(uuid, "mpath-", 6)) {
			info("%s: unhandled DM uuid '%s', ignore",
			     devname, uuid);
			return;
		}
		devname = udev_device_get_sysattr_value(udev_dev,
							"dm/name");
	} else {
		warn("%s: unhandled device, ignore", devname);
		return;
	}
	sprintf(devpath, "%s/holders",
		udev_device_get_syspath(udev_dev));
	dirp = opendir(devpath);
	if (dirp) {
		while ((dirfd = readdir(dirp))) {
			if (dirfd->d_name[0] == '.')
				continue;
			found_md = lookup_md_alias(dirfd->d_name);
			if (found_md)
				break;
		}
		closedir(dirp);
	}
	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		tmp_devt = udev_device_get_devnum(tmp->device);
		if (tmp_devt == raid_devt) {
			found = tmp;
			break;
		}
	}
	if (found) {
		info("%s: already attached", found->dev_name);
	} else {
		found = allocate_device(raid_dev);
		if (!found) {
			err("%s: out of memory allocating device",
			    udev_device_get_sysname(raid_dev));
			unlock_device_list();
			return;
		}
		list_add(&found->entry, &device_list);
		info("%s: attached '%s'", found->dev_name,
		     udev_device_get_devpath(raid_dev));
	}
	unlock_device_list();
	if (found_md) {
		const char *mdname = udev_device_get_sysname(found_md->device);

		pthread_mutex_lock(&found_md->device_lock);
		if (!list_empty(&found->siblings)) {
			warn("%s: Already monitoring %s",
			     mdname, found->md_name);
		} else {
			add_component(found_md, found, devname);
			info("%s: Start monitoring %s", mdname, found->md_name);
			list_add(&found->siblings, &found_md->children);
			monitor_device(found);
		}
		pthread_mutex_unlock(&found_md->device_lock);
	} else {
		dbg("%s: no md array found", devname);
	}
}

static void detach_device(struct udev_device *udev_dev)
{
	struct device_monitor *tmp, *found = NULL;
	const char *devname;

	devname = udev_device_get_sysname(udev_dev);
	if (!devname)
		return;
	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		if (!strcmp(tmp->dev_name, devname)) {
			list_del_init(&tmp->entry);
			found = tmp;
			break;
		}
	}
	unlock_device_list();
	if (found) {
		info("%s: Detach %s", found->dev_name,
		     udev_device_get_devpath(found->device));
		if (!list_empty(&found->siblings)) {
			struct md_monitor *md_dev;
			const char *mdname = NULL;

			pthread_mutex_lock(&found->lock);
			if (found->parent)
				mdname = udev_device_get_sysname(found->parent);
			pthread_mutex_unlock(&found->lock);
			md_dev = lookup_md(mdname, 0);
			if (md_dev) {
				remove_md_component(md_dev, found);
				pthread_mutex_lock(&md_dev->device_lock);
				list_del_init(&found->siblings);
				pthread_mutex_unlock(&md_dev->device_lock);
				remove_component(found);
			}
		}
		device_monitor_put(found);
	} else {
		warn("%s: no device to detach", devname);
	}
}

static void discover_devices(struct udev *udev)
{
	struct udev_enumerate *device_enumerate;
	struct udev_list_entry *entry;
	struct udev_device *dev;
	const char *devpath;

	device_enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(device_enumerate, "block");
	udev_enumerate_add_match_sysname(device_enumerate, "dasd*");
	udev_enumerate_add_match_sysname(device_enumerate, "dm-*");
	udev_enumerate_add_match_is_initialized(device_enumerate);
	udev_enumerate_scan_devices(device_enumerate);

	udev_list_entry_foreach(entry,
				udev_enumerate_get_list_entry(device_enumerate)) {
		devpath = udev_list_entry_get_name(entry);
		dev = udev_device_new_from_syspath(udev, devpath);
		if (dev) {
			attach_device(dev);
			udev_device_unref(dev);
		}
	}
	udev_enumerate_unref(device_enumerate);
}

static void md_rdev_update_index(struct md_monitor *md,
				 struct device_monitor *dev)
{
	const char *mdname = udev_device_get_sysname(md->device);
	int ioctl_fd, i;
	mdu_disk_info_t info;
	char mdpath[256];
	dev_t mon_devt, tmp_devt;
	struct udev *udev;

	if (!md) {
		dbg("No MD array found");
		return;
	}

	sprintf(mdpath, "/dev/%s", mdname);
	ioctl_fd = open(mdpath, O_RDONLY|O_NONBLOCK);
	if (ioctl_fd < 0) {
		warn("%s: Couldn't open %s: %m", md->dev_name, mdpath);
		return;
	}
	udev = udev_device_get_udev(md->device);
	for (i = 0; i < 4096; i++) {
		info.number = i;
		if (ioctl(ioctl_fd, GET_DISK_INFO, &info) < 0) {
			warn("%s: ioctl GET_DISK_INFO for disk %d failed: %m",
			     md->dev_name, i);
			continue;
		}
		if (info.major == 0 && info.minor == 0)
			continue;
		/* Skip devices on arrays other than RAID10 */
		if (md->level != 10)
			continue;

		/*
		 * Figure out the corresponding block device.
		 * It can either be the device itself or
		 * (if the MD array is running on a partition)
		 * the parent of the device.
		 */
		tmp_devt = udev_device_get_devnum(dev->device);
		mon_devt = makedev(info.major, info.minor);
		if (tmp_devt != mon_devt) {
			struct udev_device *mon_dev;

			mon_dev = udev_device_new_from_devnum(udev, 'b', mon_devt);
			mon_devt = udev_device_get_devnum(udev_device_get_parent(mon_dev));
			udev_device_unref(mon_dev);
		}

		if (tmp_devt == mon_devt) {
			if (!dev->parent)
				dev->parent = md->device;

			dev->md_index = i;
			if (info.raid_disk > -1) {
				dev->md_slot = info.raid_disk;
				if (dev->md_slot_saved < 0 && dev->md_slot >= 0)
					dev->md_slot_saved = dev->md_slot;
			}
			if (dev->md_side < 0)
				dev->md_side = dev->md_slot % (md->layout & 0xFF);
			info("%s: update index on %s (%d/%d)", md->dev_name,
			     dev->md_name, dev->md_index, dev->md_slot);
			break;
		}
	}
	close(ioctl_fd);
}

enum md_rdev_status md_rdev_check_state(struct device_monitor *dev, int *md_slot)
{
	int ioctl_fd;
	char attrpath[256];
	mdu_disk_info_t info;
	enum md_rdev_status md_status = SPARE;
	const char *sysname;

	if (!dev)
		return UNKNOWN;
	sysname = udev_device_get_sysname(dev->parent);
	if (!sysname)
		return UNKNOWN;
	sprintf(attrpath, "/dev/%s", sysname);
	ioctl_fd = open(attrpath, O_RDONLY|O_NONBLOCK);
	if (ioctl_fd < 0) {
		warn("%s: cannot open %s for MD ioctl: %m",
		     dev->dev_name, attrpath);
		return UNKNOWN;
	}
	info.number = dev->md_index;
	if (ioctl(ioctl_fd, GET_DISK_INFO, &info) < 0) {
		err("%s: ioctl GET_DISK_INFO failed: %m",
		    dev->dev_name);
		md_status = UNKNOWN;
	}
	close(ioctl_fd);

	if (md_status == UNKNOWN)
		return md_status;

	if ((info.state & (1 << MD_DISK_ACTIVE)) &&
	    (info.state & (1 << MD_DISK_SYNC)))
		md_status = IN_SYNC;
	else if (info.state & (1 << MD_DISK_FAULTY)) {
		if (info.state & (1 << MD_DISK_TIMEOUT))
			md_status = TIMEOUT;
		else
			md_status = FAULTY;
	} else if (info.state & (1 << MD_DISK_REMOVED))
		md_status = REMOVED;

	*md_slot = info.raid_disk;
	info("%s: MD rdev (%d/%d) state %s (%x) slot %d",
	     dev->dev_name, dev->md_index, dev->md_slot,
	     md_rdev_print_state(md_status), info.state,
	     *md_slot);

	return md_status;
}

enum md_rdev_status md_rdev_update_state(struct device_monitor *dev,
					 enum md_rdev_status md_status, int md_slot)
{
	enum md_rdev_status old_status = dev->md_status;
	int old_slot = dev->md_slot;

	switch (old_status) {
	case PENDING:
		/*
		 * PENDING indicates we've sent a 'fail' request
		 * to mdadm for a device which was 'in_sync'. So
		 * we should only update status if it's not in_sync
		 * anymore.
		 */
		if (md_status == FAULTY ||
		    md_status == SPARE ||
		    md_status == TIMEOUT) {
			dev->md_status = md_status;
		} else {
			warn("%s: not update pending status to %s",
			     dev->dev_name, md_rdev_print_state(md_status));
			md_status = dev->md_status;
		}
		break;
	case RECOVERY:
		/*
		 * RECOVERY indicates we've send a 'remove' and
		 * 're-add' sequence to mdadm for a failed device.
		 * So the device must not be in 'faulty' anymore
		 */
		if (md_status != FAULTY && md_status != TIMEOUT) {
			dev->md_status = md_status;
		} else {
			md_status = dev->md_status;
		}
		break;
	case TIMEOUT:
		/*
		 * TIMEOUT implies that the DASD timeout flag is set,
		 * hence all I/O will be failed. But that does not
		 * imply that the path itself has failed.
		 */
		if (md_status != FAULTY)
			dev->md_status = md_status;
		else
			md_status = TIMEOUT;
		break;
	default:
		dev->md_status = md_status;
		break;
	}
	if (old_status != dev->md_status)
		info("%s: md state update from %s to %s", dev->dev_name,
		     md_rdev_print_state(old_status),
		     md_rdev_print_state(dev->md_status));
	if (md_slot != old_slot) {
		dev->md_slot = md_slot;
		if (dev->md_slot_saved < 0 && dev->md_slot >= 0)
			dev->md_slot_saved = dev->md_slot;
		info("%s: md slot number update from %d to %d",
		     dev->dev_name, old_slot, dev->md_slot);
	}
	return md_status;
}

int md_set_attribute(struct md_monitor *md_dev, const char *attr,
		     const char *value)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	int attr_fd;
	char attrpath[256];
	char status[64];
	ssize_t len, status_len = 64;
	int rc = 0;

	sprintf(attrpath, "%s/md/%s", udev_device_get_syspath(md_dev->device),
		attr);
	attr_fd = open(attrpath, O_RDWR);
	if (attr_fd < 0) {
		warn("%s: failed to open '%s' attribute for %s: %m",
		     md_name, attr, attrpath);
		rc = errno;
		goto remove;
	}

	memset(status, 0, status_len);
	len = read(attr_fd, status, status_len);
	if (len < 0) {
		warn("%s: cannot read '%s' attribute: %m",
		     md_name, attr);
		rc = errno;
		goto remove;
	}
	if (len == 0) {
		warn("%s: EOF on reading '%s' attribute", md_name, attr);
		goto remove;
	}
	if (len == status_len) {
		warn("%s: Overflow on reading '%s' attribute", md_name, attr);
		goto remove;
	}
	if (status[len - 1] == '\n') {
		status[len - 1] = '\0';
		len--;
	}
	status[status_len - 1] = '\0';

	if (!strlen(status)) {
		warn("%s: empty '%s' attribute", md_name, attr);
		goto remove;
	}

	len = write(attr_fd, value, strlen(value));
	if (len < 0) {
		warn("%s: cannot set '%s' attribute to '%s': %m",
		     md_name, attr, value);
		rc = errno;
	}
	info("%s: '%s' = '%s' -> '%s'", md_name, attr, status, value);
	rc = 0;
remove:
	if (attr_fd >= 0)
		close(attr_fd);
	return rc;
}

void device_monitor_cleanup(void *data)
{
	struct device_monitor *dev = data;

	dasd_cleanup_aio(dev);
	if (dev->buf) {
		free(dev->buf);
		dev->buf = NULL;
	}

	info("%s: shutdown device monitor thread", dev->dev_name);
	pthread_mutex_lock(&dev->lock);
	dev->running = 0;
	dev->thread = 0;
	pthread_cond_signal(&dev->io_cond);
	pthread_mutex_unlock(&dev->lock);
	device_monitor_put(dev);
}

enum md_rdev_status
device_monitor_update(struct device_monitor *dev,
		      enum device_io_status io_status,
		      enum md_rdev_status new_status)

{
	if (io_status != IO_OK) {
		switch (new_status) {
		case RECOVERY:
			warn("%s: failing device in recovery",
			     dev->dev_name);
			fail_mirror(dev, FAULTY);
			break;
		case IN_SYNC:
			warn("%s: failing device in_sync",
			     dev->dev_name);
			new_status = FAULTY;
			/* Fallthrough */
		case FAULTY:
			/*
			 * I/O timeout might not have been
			 *  acknowledged by md yet.
			 */
			if (io_status == IO_TIMEOUT)
				new_status = TIMEOUT;
			warn("%s: failing %s device",
			     dev->dev_name, md_rdev_print_state(new_status));
			/* Fallthrough */
		case PENDING:
		case TIMEOUT:
			fail_mirror(dev, new_status);
			break;
		case UNKNOWN:
			break;
		default:
			warn("%s: Invalid array state", dev->dev_name);
			break;
		}
	} else {
		switch (new_status) {
		case IN_SYNC:
			pthread_mutex_lock(&dev->lock);
			if (dev->running && stop_on_sync) {
				info("%s: path ok, stopping monitor",
				     dev->dev_name);
				dev->running = 0;
			}
			pthread_mutex_unlock(&dev->lock);
			break;
		case RECOVERY:
		case BLOCKED:
		case FAULTY:
		case TIMEOUT:
		case SPARE:
			reset_mirror(dev);
			/* Fallthrough */
		default:
			break;
		}
	}
	return new_status;
}

void *device_monitor_thread (void *ctx)
{
	struct device_monitor *dev = ctx;
	unsigned long pgsize = getpagesize();
	enum device_io_status io_status;
	enum md_rdev_status md_status, new_status;
	struct timespec tmo;
	int rc, aio_timeout = 0, sig_timeout = checker_timeout;

	device_monitor_get(dev);
	/* Reset any stale ioctl flags */
	dasd_timeout_ioctl(dev->device, 0);

	pthread_cleanup_push(device_monitor_cleanup, dev);
	rc = dasd_setup_aio(dev);
	if (rc) {
		err("%s: setup async I/O failed with %d",
		    dev->dev_name, rc);
		pthread_exit(&rc);
	}
	dev->buf = (unsigned char *)malloc(dev->blksize + pgsize);
	if (!dev->buf) {
		err("%s: cannot allocate io buffer", dev->dev_name);
		rc = 3;
		pthread_exit(&rc);
	}

	pthread_mutex_lock(&dev->lock);
	while (dev->running) {
		dbg("%s: check aio state, timeout %d secs",
		    dev->dev_name, aio_timeout);
		if (dev->md_status == TIMEOUT) {
			dasd_timeout_ioctl(dev->device, 0);
			dev->md_status = UNKNOWN;
		}
		pthread_mutex_unlock(&dev->lock);
		io_status = dasd_check_aio(dev, aio_timeout);
		if (io_status == IO_ERROR) {
			warn("%s: error during aio submission, exit",
			     dev->dev_name);
			pthread_mutex_lock(&dev->lock);
			break;
		}
		pthread_testcancel();
		if (io_status != IO_TIMEOUT) {
			int md_slot = -1;

			/* Re-check; status might have been changed during aio */
			md_status = md_rdev_check_state(dev, &md_slot);
			if (md_status == UNKNOWN) {
				/* array has been stopped */
				info("%s: stopping monitor in status %s",
				     dev->dev_name,
				     md_rdev_print_state(md_status));
				pthread_mutex_lock(&dev->lock);
				break;
			}

			/* Write status back */
			pthread_mutex_lock(&dev->lock);
			new_status = md_rdev_update_state(dev, md_status, md_slot);
		} else {
			pthread_mutex_lock(&dev->lock);
			new_status = TIMEOUT;
		}
		/* dev->lock held */
		if (io_status == IO_PENDING) {
			/*
			 * io_getevents or sigtimedwait
			 * got interrupted by a signal.
			 * Check whether we need to fail the mirror.
			 */
			pthread_mutex_unlock(&dev->lock);
			info("%s: path checker interrupted, new state %s",
			     dev->dev_name, md_rdev_print_state(new_status));
			if (new_status == FAULTY || new_status == TIMEOUT) {
				fail_mirror(dev, new_status);
			}
			aio_timeout = monitor_timeout;
			pthread_mutex_lock(&dev->lock);
			dev->io_status = io_status;
			pthread_cond_signal(&dev->io_cond);
			continue;
		}
		/* dev->lock held */
		if (io_status == IO_UNKNOWN) {
			/*
			 * First round, we cannot really make any sane
			 * decisions yet. Wait until we got the I/O
			 * results.
			 */
			info("%s: path checker not started, retry",
			     dev->dev_name);
			aio_timeout = monitor_timeout;
			continue;
		}
		dev->io_status = io_status;
		pthread_cond_signal(&dev->io_cond);
		pthread_mutex_unlock(&dev->lock);
		new_status = device_monitor_update(dev, io_status, new_status);
		info("%s: state %s / %s",
		     dev->dev_name, md_rdev_print_state(new_status),
		     device_io_print_state(io_status));
		if (new_status == STOPPED) {
			pthread_mutex_lock(&dev->lock);
			break;
		}
		tmo.tv_sec = sig_timeout;
		tmo.tv_nsec = 0;
		info("%s: waiting %ld seconds ...",
		     dev->dev_name, (long)tmo.tv_sec);
		rc = sigtimedwait(&thread_sigmask, NULL, &tmo);
		pthread_mutex_lock(&dev->lock);
		if (rc < 0) {
			if (errno == EINTR) {
				info("%s: ignore signal",
				     dev->dev_name);
			} else if (errno != EAGAIN) {
				info("%s: wait failed: %s",
				     dev->dev_name, strerror(errno));
				break;
			}
			aio_timeout = monitor_timeout;
		} else {
			info("%s: wait interrupted",
			     dev->dev_name);
			aio_timeout = 0;
		}
	}
	pthread_mutex_unlock(&dev->lock);

	pthread_cleanup_pop(1);
	return ((void *)0);
}

static void monitor_device(struct device_monitor *dev)
{
	int rc;

	if (strncmp(dev->dev_name, "dasd", 4))
		return;

	device_monitor_get(dev);
	pthread_mutex_lock(&dev->lock);
	if (dev->running) {
		/* check if thread is still alive */
		if (dev->thread) {
			pthread_t thread = dev->thread;

			/* Yes, everything is okay */
			info("%s: notify monitor thread",
			     dev->dev_name);
			/* Release the lock to avoid deadlocking */
			pthread_mutex_unlock(&dev->lock);
			device_monitor_put(dev);
			pthread_kill(thread, SIGHUP);
			return;
		}
		info("%s: Re-start monitor", dev->dev_name);
		dev->running = 0;
		pthread_mutex_unlock(&dev->lock);
		/* Yield lock here to give stale threads time to react */
		pthread_yield();
	} else {
		pthread_mutex_unlock(&dev->lock);
		/* Start new monitor thread */
		info("%s: Start new monitor", dev->dev_name);
	}
	pthread_mutex_lock(&dev->lock);
	dev->running = 1;
	pthread_mutex_unlock(&dev->lock);
	rc = pthread_create(&dev->thread, &monitor_attr,
			    device_monitor_thread, dev);
	if (rc) {
		pthread_mutex_lock(&dev->lock);
		dev->running = 0;
		dev->io_status = IO_UNKNOWN;
		dev->thread = 0;
		pthread_mutex_unlock(&dev->lock);
		warn("%s: Failed to start monitor thread, error %d",
		     dev->dev_name, rc);
	}
	device_monitor_put(dev);
}

static void add_component(struct md_monitor *md, struct device_monitor *dev,
	const char *md_name)
{
	size_t md_namelen;
	int is_dasd = 0;

	if (!md_name)
		return;

	md_namelen = strlen(md_name);
	if (md_namelen > MD_NAMELEN)
		md_namelen = MD_NAMELEN;

	pthread_mutex_lock(&dev->lock);
	info("%s: Add component %s (%d/%d)", dev->dev_name, md_name,
	     dev->md_index, dev->md_slot);
	if (!dev->parent) {
		udev_device_ref(md->device);
		dev->parent = md->device;
	}
	memset(dev->md_name, 0x0, MD_NAMELEN);
	strncpy(dev->md_name, md_name, md_namelen);
	if (dev->md_index < 0)
		md_rdev_update_index(md, dev);
	if (!strncmp(dev->dev_name, "dasd", 4))
		is_dasd = 1;
	pthread_mutex_unlock(&dev->lock);
	if (is_dasd) {
		dasd_set_attribute(dev, "failfast", 1);
		if (dasd_set_attribute(dev, "timeout",
			(failfast_retries + 1) * failfast_timeout) < 0) {
			dasd_set_attribute(dev, "failfast_retries",
					   failfast_retries);
			dasd_set_attribute(dev, "failfast_expires",
					   failfast_timeout);
		}
	}
}

static void remove_component(struct device_monitor *dev)
{
	info("%s: Remove component (%d/%d)",
	     dev->dev_name, dev->md_index, dev->md_slot);

	pthread_mutex_lock(&dev->lock);
	if (dev->parent)
		udev_device_unref(dev->parent);
	dev->parent = NULL;
	pthread_mutex_unlock(&dev->lock);
}

static int fail_component(struct device_monitor *dev,
			  enum md_rdev_status new_status)
{
	enum md_rdev_status md_status, old_status;
	int rc = 0, md_slot = -1;
	pthread_t thread;

	/* Check state if we need to do anything here */
	old_status = md_rdev_check_state(dev, &md_slot);
	pthread_mutex_lock(&dev->lock);
	dev->md_status = old_status;
	md_status = md_rdev_update_state(dev, new_status, md_slot);
	if (md_status == new_status) {
		pthread_mutex_unlock(&dev->lock);
		info("%s: already in state '%s'",
		     dev->dev_name, md_rdev_print_state(md_status));
		return rc;
	}

	thread = dev->thread;
	if (dev->running && thread) {
		if (new_status == REMOVED)
			dev->running = 0;
		pthread_mutex_unlock(&dev->lock);
		info("%s: notify monitor thread for new status %s",
		     dev->dev_name, md_rdev_print_state(new_status));
		pthread_kill(thread, SIGHUP);
		rc = EBUSY;
	} else {
		pthread_mutex_unlock(&dev->lock);
	}

	return rc;
}

static int reset_component(struct device_monitor *dev)
{
	pthread_mutex_lock(&dev->lock);
	if (dev->io_status != IO_OK) {
		info("%s: I/O status %s, do not reset device", dev->dev_name,
		     device_io_print_state(dev->io_status));
		pthread_mutex_unlock(&dev->lock);
		return -EIO;
	}

	if (!strncmp(dev->dev_name, "dasd", 4)) {
		dasd_timeout_ioctl(dev->device, 0);
		dasd_set_attribute(dev, "failfast", 1);
	} else
		mpath_modify_queueing(dev, 1, monitor_timeout);


	switch (dev->md_status) {
	case FAULTY:
	case TIMEOUT:
	case REMOVED:
	case SPARE:
		dev->md_status = RECOVERY;
		break;
	case BLOCKED:
		info("%s: unblock device", dev->dev_name);
		dev->md_status = IN_SYNC;
		break;
	default:
		info("%s: do not reset device in state '%s'", dev->dev_name,
		     md_rdev_print_state(dev->md_status));
		break;
	}
	pthread_mutex_unlock(&dev->lock);

	return 0;
}

static void fail_mirror(struct device_monitor *dev, enum md_rdev_status status)
{
	struct md_monitor *md_dev;
	struct device_monitor *tmp;
	const char *md_name = NULL;

	pthread_mutex_lock(&dev->lock);
	if (dev->parent)
		md_name = udev_device_get_sysname(dev->parent);
	pthread_mutex_unlock(&dev->lock);
	md_dev = lookup_md(md_name, 0);
	if (!md_dev) {
		warn("%s: No md device found", dev->dev_name);
		return;
	}
	md_name = udev_device_get_sysname(md_dev->device);
	if (md_dev->in_discovery) {
		info("%s: md in discovery, not failing mirror", md_name);
		return;
	}
	if (!fail_mirror_side || status == REMOVED) {
		fail_component(dev, status);
		return;
	}

	pthread_mutex_lock(&md_dev->status_lock);
	if (!md_dev->device) {
		pthread_mutex_unlock(&md_dev->status_lock);
		return;
	}
	if (md_dev->pending_status) {
		info("%s: %s already scheduled, not failing", md_name,
		     md_rdev_print_state(md_dev->pending_status));
		pthread_mutex_unlock(&md_dev->status_lock);
		return;
	}

	if (md_dev->degraded & (1 << dev->md_side)) {
		/* Mirror side is already failed, nothing to be done here */
		info("%s: mirror side %d is already failed", md_name, dev->md_side);
		pthread_mutex_unlock(&md_dev->status_lock);
	} else if (md_dev->degraded) {
		/* Mirror is already degraded, do not notify md */
		info("%s: other mirror side for %d is already failed",
		     md_name, dev->md_side);
		md_dev->degraded |= (1 << dev->md_side);
		pthread_mutex_unlock(&md_dev->status_lock);
		pthread_mutex_lock(&md_dev->device_lock);
		list_for_each_entry(tmp, &md_dev->children, siblings) {
			if (tmp->md_side == dev->md_side) {
				pthread_mutex_lock(&tmp->lock);
				tmp->md_status = BLOCKED;
				pthread_mutex_unlock(&tmp->lock);
			}
		}
		pthread_mutex_unlock(&md_dev->device_lock);
	} else {
		info("%s: Failing all devices on side %d, status %s",
		     md_name, dev->md_side, md_rdev_print_state(status));
		if (list_empty(&md_dev->pending)) {
			pthread_mutex_lock(&pending_lock);
			md_dev->pending_status = status;
			md_dev->pending_side = (1 << dev->md_side);
			list_add(&md_dev->pending, &pending_list);
			pthread_cond_signal(&pending_cond);
			pthread_mutex_unlock(&pending_lock);
		} else {
			info("%s: fail already scheduled", md_name);
		}
		pthread_mutex_unlock(&md_dev->status_lock);
	}

}

static void reset_mirror(struct device_monitor *dev)
{
	struct md_monitor *md_dev;
	int ready_devices;
	struct device_monitor *tmp;
	const char *md_name = NULL;

	pthread_mutex_lock(&dev->lock);
	if (dev->parent)
		md_name = udev_device_get_sysname(dev->parent);
	pthread_mutex_unlock(&dev->lock);
	md_dev = lookup_md(md_name, 0);
	if (!md_dev) {
		warn("%s: No md device found", dev->dev_name);
		return;
	}
	pthread_mutex_lock(&md_dev->status_lock);
	if (!md_dev->device) {
		pthread_mutex_unlock(&md_dev->status_lock);
		return;
	}
	md_name = udev_device_get_sysname(md_dev->device);
	if (md_dev->in_recovery) {
		pthread_mutex_unlock(&md_dev->status_lock);
		info("%s: array in recovery, skip reset", md_name);
		return;
	}
	if (md_dev->pending_status) {
		info("%s: %s already scheduled, not resetting", md_name,
		     md_rdev_print_state(md_dev->pending_status));
		pthread_mutex_unlock(&md_dev->status_lock);
		return;
	}
	pthread_mutex_unlock(&md_dev->status_lock);

	info("%s: reset mirror side %d", md_name, dev->md_side);
	pthread_mutex_lock(&md_dev->device_lock);
	ready_devices = 0;
	list_for_each_entry(tmp, &md_dev->children, siblings) {
		int recheck = 0;
		pthread_t thread;

		pthread_mutex_lock(&tmp->lock);
		dbg("%s: dev %s side %d state %s / %s slot %d", md_name, tmp->dev_name,
		     tmp->md_side, md_rdev_print_state(tmp->md_status),
		     device_io_print_state(tmp->io_status));
		if (tmp->md_status == RECOVERY) {
			pthread_mutex_unlock(&tmp->lock);
			continue;
		}
		if (tmp->io_status == IO_UNKNOWN ||
		    tmp->io_status == IO_FAILED ||
		    tmp->io_status == IO_RETRY) {
			pthread_mutex_unlock(&tmp->lock);
			continue;
		}
		if (tmp->md_side != dev->md_side)
			ready_devices++;
		else if (tmp->io_status == IO_OK) {
			if (tmp->md_slot >= 0) {
				/* Notify monitor thread to re-check */
				if (tmp->running && tmp->thread) {
					thread = tmp->thread;
					recheck = 1;
				}
			}
			if (tmp->md_slot < 0)
				ready_devices++;
		}
		pthread_mutex_unlock(&tmp->lock);
		if (recheck) {
			info("%s: notify monitor thread to recheck slot",
			     tmp->dev_name);
			pthread_kill(thread, SIGHUP);
		}
	}
	pthread_mutex_unlock(&md_dev->device_lock);
	/* Not enough devices, don't reset mirror side */
	if (ready_devices != md_dev->raid_disks) {
		info("%s: not enough devices to reset (%d/%d)", md_name,
		     ready_devices, md_dev->raid_disks);
		return;
	}
	info("%s: reset mirror, %d of %d devices ready", md_name,
	     ready_devices, md_dev->raid_disks);

	pthread_mutex_lock(&md_dev->status_lock);
	if (list_empty(&md_dev->pending)) {
		pthread_mutex_lock(&pending_lock);
		md_dev->pending_status = IN_SYNC;
		md_dev->pending_side = (1 << dev->md_side);
		list_add(&md_dev->pending, &pending_list);
		pthread_cond_signal(&pending_cond);
		pthread_mutex_unlock(&pending_lock);
	} else {
		info("%s: reset already scheduled", md_name);
	}
	pthread_mutex_unlock(&md_dev->status_lock);
}

static void fail_md_component(struct md_monitor *md_dev,
			      struct device_monitor *dev)
{
	enum md_rdev_status md_status, new_status;
	int md_slot = -1;

	/*
	 * Fail does not necessarily indicate the device has gone,
	 * so just invoke a state check.
	 */
	info("%s: fail component in state %s", dev->dev_name,
	     md_rdev_print_state(dev->md_status));

	md_status = md_rdev_check_state(dev, &md_slot);
	if (md_status == UNKNOWN ||
	    md_status == RECOVERY ||
	    md_status == SPARE ||
	    md_status == BLOCKED) {
		/*
		 * UNKNOWN is set if the path checkers hasn't
		 * run yet.
		 * RECOVERY is set if a mdadm --re-add has
		 * been scheduled.
		 * SPARE is set if the device is marked as 'spare',
		 * ie doesn't participate in the currently active array.
		 * BLOCKED is set if the other side of an already
		 * degraded MD array returns an I/O failure.
		 * In either case we shouldn't fail the array.
		 */
		warn("%s: device status %s, ignore state change",
		     dev->dev_name, md_rdev_print_state(md_status));
		return;
	} else if (md_status != TIMEOUT)
		md_status = FAULTY;
	pthread_mutex_lock(&dev->lock);
	new_status = md_rdev_update_state(dev, md_status, md_slot);
	if (new_status == TIMEOUT)
		dev->io_status = IO_TIMEOUT;
	else if (new_status == FAULTY)
		dev->io_status = IO_FAILED;
	else
		dev->io_status = IO_OK;
	pthread_mutex_unlock(&dev->lock);
	if (new_status != IN_SYNC)
		fail_mirror(dev, new_status);

	monitor_device(dev);
}

static void sync_md_component(struct md_monitor *md_dev,
			      struct device_monitor *dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);

	pthread_mutex_lock(&dev->lock);
	if (dev->md_status == PENDING) {
		warn("%s: mdadm call still pending", dev->dev_name);
	}
	info("%s: setting '%s' to IN_SYNC",
	     md_name, dev->dev_name);
	/*
	 * Not using md_rdev_update_state here;
	 * IN_SYNC should override previous state.
	 */
	dev->md_status = IN_SYNC;
	pthread_mutex_unlock(&dev->lock);
	monitor_device(dev);
}

static void remove_md_component(struct md_monitor *md_dev,
				struct device_monitor *dev)
{
	pthread_t thread;

	pthread_mutex_lock(&dev->lock);
	if (dev->md_status == PENDING) {
		warn("%s: mdadm call still pending", dev->dev_name);
	}
	info("%s: setting '%s' to REMOVED",
	     md_dev->dev_name, dev->dev_name);
	dev->md_status = REMOVED;
	thread = dev->thread;
	if (dev->running && thread) {
		info("%s: shutdown monitor thread",
		     dev->dev_name);
		dev->running = 0;
		pthread_mutex_unlock(&dev->lock);
		pthread_kill(thread, SIGHUP);
		if (pthread_cancel(thread) == 0)
			pthread_join(thread, NULL);
	} else {
		pthread_mutex_unlock(&dev->lock);
	}
}

static void discover_md_components(struct md_monitor *md)
{
	const char *mdname = udev_device_get_sysname(md->device);
	int ioctl_fd, i;
	mdu_disk_info_t info;
	struct device_monitor *tmp, *found = NULL;
	char mdpath[256];
	struct udev *udev;
	struct list_head update_list;
	const char *devtype, *sysname;

	if (!mdname) {
		dbg("No MD array found");
		return;
	}

	info("%s: discover", mdname);
	md->in_discovery = 1;
	udev = udev_device_get_udev(md->device);
	sprintf(mdpath, "/dev/%s", mdname);
	ioctl_fd = open(mdpath, O_RDONLY|O_NONBLOCK);
	if (ioctl_fd < 0) {
		warn("%s: Couldn't open %s: %m", mdname, mdpath);
		md->in_discovery = 0;
		return;
	}
	/* Temporarily move children devices onto a separate list */
	INIT_LIST_HEAD(&update_list);
	pthread_mutex_lock(&md->device_lock);
	list_splice_init(&md->children, &update_list);
	for (i = 0; i < 4096; i++) {
		dev_t raid_devt, mon_devt, tmp_devt;
		struct udev_device *raid_dev, *mon_dev;

		info.number = i;
		if (ioctl(ioctl_fd, GET_DISK_INFO, &info) < 0) {
			warn("%s: ioctl GET_DISK_INFO for disk %d failed: %m",
			     mdname, i);
			continue;
		}
		if (info.major == 0 && info.minor == 0)
			continue;
		if (md->level != 10) {
			info("%s: skip disk %d on RAID%d array", mdname, i,
			     md->level);
			continue;
		}
		info("%s: discover raid disk %d (%d:%d)", mdname, i,
		     info.major, info.minor);

		found = NULL;
		/*
		 * Figure out the corresponding block device.
		 * It can either be the device itself or
		 * (if the MD array is running on a partition)
		 * the parent of the device.
		 */
		raid_devt = makedev(info.major, info.minor);
		raid_dev = udev_device_new_from_devnum(udev, 'b', raid_devt);
		if (!raid_dev) {
			warn("%s: raid disk %d (%d:%d) not found",
			     mdname, i, info.major, info.minor);
			continue;
		}
		devtype = udev_device_get_devtype(raid_dev);
		if (!devtype || !strncmp(devtype, "disk", 4)) {
			mon_dev = raid_dev;
			mon_devt = raid_devt;
		} else {
			mon_dev = udev_device_get_parent(raid_dev);
			mon_devt = udev_device_get_devnum(mon_dev);
		}
		/* Restart monitoring on existing devices */
		list_for_each_entry(tmp, &update_list, siblings) {
			tmp_devt = udev_device_get_devnum(tmp->device);
			if (tmp_devt == mon_devt) {
				found = tmp;
				break;
			}
		}
		if (found) {
			pthread_mutex_lock(&found->lock);
			if (!found->parent)
				found->parent = md->device;

			info("%s: Restart monitoring %s", mdname,
			     found->md_name);
			/* Be on the safe side and update indices */
			found->md_index = i;
			found->md_slot = info.raid_disk;
			if (found->md_slot_saved < 0 && found->md_slot >= 0)
				found->md_slot_saved = found->md_slot;
			pthread_mutex_unlock(&found->lock);
			list_move(&found->siblings, &md->children);
			monitor_device(found);
			found = NULL;
			udev_device_unref(raid_dev);
			continue;
		}
		/* Start monitoring a new device */
		lock_device_list();
		list_for_each_entry(tmp, &device_list, entry) {
			tmp_devt = udev_device_get_devnum(tmp->device);
			if (tmp_devt == mon_devt) {
				found = tmp;
				break;
			}
		}
		unlock_device_list();
		if (!found) {
			warn("%s: raid disk %d (%d:%d) not attached", mdname,
			     i, info.major, info.minor);
			found = allocate_device(mon_dev);
			if (!found) {
				err("%s: out of memory allocating device (%d:%d)",
				    mdname, info.major, info.minor);
				udev_device_unref(raid_dev);
				continue;
			}
			lock_device_list();
			list_add(&found->entry, &device_list);
			info("%s: attached '%s'", found->dev_name,
			     udev_device_get_devpath(mon_dev));
			unlock_device_list();
		}
		pthread_mutex_lock(&found->lock);
		found->md_index = i;
		found->md_slot = info.raid_disk;
		if (found->md_slot_saved < 0 && found->md_slot >= 0)
			found->md_slot_saved = found->md_slot;
		found->md_side = found->md_slot % (md->layout & 0xFF);
		pthread_mutex_unlock(&found->lock);
		sysname = udev_device_get_sysname(raid_dev);
		if (!strncmp(sysname, "dm-", 3)) {
			sysname = udev_device_get_sysattr_value(raid_dev,
								"dm/name");
			if (!sysname) {
				warn("%s: no device-mapper name", sysname);
				udev_device_unref(raid_dev);
				continue;
			}
		}
		if (!list_empty(&found->siblings)) {
			warn("%s: Already monitoring %s",
			     mdname, found->md_name);
			udev_device_unref(raid_dev);
		} else {
			add_component(md, found, sysname);
			info("%s: Start monitoring %s", mdname, found->md_name);
			udev_device_unref(raid_dev);
			list_add(&found->siblings, &md->children);
			monitor_device(found);
		}
		found = NULL;
	}
	pthread_mutex_unlock(&md->device_lock);
	pthread_mutex_lock(&md->status_lock);
	if (!md->in_recovery) {
		/* Cleanup stale devices */
		pthread_mutex_unlock(&md->status_lock);
		list_for_each_entry_safe(found, tmp, &update_list, siblings) {
			info("%s: Remove stale device",
			     found->dev_name);
			remove_md_component(md, found);
			list_del_init(&found->siblings);
			remove_component(found);
		}
		pthread_mutex_lock(&md->status_lock);
	} else {
		info("%s: skip stale device detection, array in recovery",
		     mdname);
	}
	md->in_discovery = 0;
	pthread_mutex_unlock(&md->status_lock);
	close(ioctl_fd);
}

static int fail_md(struct md_monitor *md_dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	char cmdline[256];
	int rc, pending_side;
	enum md_rdev_status pending_status;
	struct device_monitor *dev;

	pthread_mutex_lock(&md_dev->status_lock);
	pending_side = md_dev->pending_side;
	pending_status = md_dev->pending_status;

	if (!md_dev->pending_side) {
		pthread_mutex_unlock(&md_dev->status_lock);
		warn("%s: no pending side", md_name);
		return 0;
	}
	if (md_dev->degraded & md_dev->pending_side) {
		info("%s: mirror side %d already failed", md_name,
		     (md_dev->pending_side >> 1));
		pthread_mutex_unlock(&md_dev->status_lock);
		return 0;
	}
	/* Set DASD timeout to abort all outstanding I/O */
	if (md_dev->pending_status == TIMEOUT) {
		pthread_mutex_unlock(&md_dev->status_lock);
		pthread_mutex_lock(&md_dev->device_lock);
		list_for_each_entry(dev, &md_dev->children, siblings) {
			if (dev->md_side == (pending_side >> 1)) {
				if (!strncmp(dev->dev_name, "dasd", 4))
					dasd_timeout_ioctl(dev->device, 1);
				else
					mpath_modify_queueing(dev, 0,
							      monitor_timeout);
			}
		}
		pthread_mutex_unlock(&md_dev->device_lock);
	} else {
		pthread_mutex_unlock(&md_dev->status_lock);
	}

	sprintf(cmdline, "mdadm --manage /dev/%s --fail set-%c", md_name,
		(pending_side >> 1) ? 'B' : 'A' );
	dbg("%s: call 'system' '%s'", md_name, cmdline);
	rc = system(cmdline);
	if (rc) {
		warn("%s: cannot fail mirror, error %d", md_name, rc);
	} else {
		dbg("%s: mirror set-%c failed", md_name,
		    (pending_side >> 1) ? 'B' : 'A');
		pthread_mutex_lock(&md_dev->device_lock);
		/*
		 * When failing one side we need to disable the
		 * 'failfast' setting on the other, as the array
		 * is required to wait for I/O in this case.
		 */
		list_for_each_entry(dev, &md_dev->children, siblings) {
			if (dev->md_side == (pending_side >> 1)) {
				fail_component(dev, pending_status);
			} else {
				dasd_set_attribute(dev, "failfast", 0);
			}
		}
		pthread_mutex_unlock(&md_dev->device_lock);
	}
	if (!rc || rc == 512) {
		pthread_mutex_lock(&md_dev->status_lock);
		md_dev->degraded |= md_dev->pending_side;
		md_dev->pending_side = 0;
		md_dev->pending_status = UNKNOWN;
		pthread_mutex_unlock(&md_dev->status_lock);
	}
	return rc == 512 ? -EBUSY : -EIO;
}

static int reset_md(struct md_monitor *md_dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	char cmdline[256];
	int rc, ret = 0;
	struct device_monitor *dev;

	pthread_mutex_lock(&md_dev->status_lock);
	if (!md_dev->pending_side) {
		pthread_mutex_unlock(&md_dev->status_lock);
		warn("%s: no pending side", md_name);
		return 0;
	}
	pthread_mutex_unlock(&md_dev->status_lock);
	pthread_mutex_lock(&md_dev->device_lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		if (reset_component(dev) < 0) {
			pthread_mutex_unlock(&md_dev->device_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&md_dev->device_lock);
	if (!md_name)
		return -EINVAL;

	sprintf(cmdline, "mdadm --manage /dev/%s --re-add faulty", md_name);
	dbg("%s: call 'system' '%s'", md_name, cmdline);
	rc = system(cmdline);
	if (rc) {
		warn("%s: cannot reset mirror, error %d",
		     md_name, rc);
		if (rc == 512)
			ret = -EBUSY;
		else
			ret = -EIO;
	} else {
		pthread_mutex_lock(&md_dev->status_lock);
		md_dev->degraded = 0;
		md_dev->pending_side = 0;
		md_dev->pending_status = UNKNOWN;
		pthread_mutex_unlock(&md_dev->status_lock);
	}
	return ret;
}

static void remove_md(struct md_monitor *md_dev)
{
	struct udev_device *device = md_dev->device;
	struct device_monitor *dev, *tmp;
	struct list_head remove_list;

	INIT_LIST_HEAD(&remove_list);
	pthread_mutex_lock(&md_dev->device_lock);
	list_splice_init(&md_dev->children, &remove_list);
	pthread_mutex_unlock(&md_dev->device_lock);

	list_for_each_entry_safe(dev, tmp, &remove_list, siblings) {
		info("%s: Remove MD component device %s",
		     md_dev->dev_name, dev->dev_name);
		remove_md_component(md_dev, dev);
		list_del_init(&dev->siblings);
		remove_component(dev);
	}

	/* Synchronize with other threads */
	pthread_mutex_lock(&md_dev->status_lock);
	md_dev->device = NULL;
	info("%s: Stop monitoring", md_dev->dev_name);
	if (device)
		udev_device_unref(device);
	pthread_mutex_unlock(&md_dev->status_lock);
	pthread_mutex_destroy(&md_dev->status_lock);
	pthread_mutex_destroy(&md_dev->device_lock);
	free(md_dev);
}

static int check_md(struct md_monitor *md_dev, mdu_array_info_t *info)
{
	const char *mdname = udev_device_get_sysname(md_dev->device);
	char devpath[256];
	int ioctl_fd, rc = 0;

	if (!md_dev)
		return ENODEV;

	sprintf(devpath, "/dev/%s", mdname);
	ioctl_fd = open(devpath, O_RDONLY|O_NONBLOCK);
	if (ioctl_fd >= 0) {
		if (ioctl(ioctl_fd, GET_ARRAY_INFO, info) < 0) {
			rc = errno;
			if (rc == ENODEV)
				info("%s: array stopped, ignoring",
				     md_dev->dev_name);
			else
				err("%s: ioctl GET_ARRAY_INFO failed: %m",
				    md_dev->dev_name);
			info->raid_disks = 0;
		} else if (info->level != 10) {
			warn("%s: Not a RAID10 array, ignoring",
			     md_dev->dev_name);
			rc = EINVAL;
		} else if (info->raid_disks == 0) {
			warn("%s: no RAID disks, ignoring", md_dev->dev_name);
			rc = EAGAIN;
		} else if (info->size == 0) {
			warn("%s: array inactive, ignoring",
			     md_dev->dev_name);
			info->raid_disks = 0;
			rc = EAGAIN;
		}
		close(ioctl_fd);
	} else {
		rc = errno;
		err("%s: could not open %s: %m", md_dev->dev_name, devpath);
		info->raid_disks = 0;
	}
	if (info->raid_disks < 1)
		return rc;

	if (info->level != 10) {
		err("%s: not a RAID10 array", md_dev->dev_name);
		return EINVAL;
	}
	return 0;
}

static int monitor_md(struct udev_device *md_dev)
{
	int rc;
	mdu_array_info_t info;
	struct md_monitor *found = NULL;

	found = lookup_md_new(md_dev);
	if (!found)
		return ENOMEM;

	memset(&info, 0, sizeof(mdu_array_info_t));
	rc = check_md(found, &info);
	if (rc)
		return rc;

	if (found->raid_disks < 0) {
		warn("%s: Start monitoring %s", found->dev_name,
		       udev_device_get_devpath(md_dev));
		found->raid_disks = info.raid_disks;
		found->layout = info.layout;
		found->level = info.level;
		discover_md_components(found);
	} else {
		const char *alias_name;

		alias_name = udev_device_get_property_value(md_dev,
							    "MD_DEVNAME");
		if (alias_name && strcmp(found->dev_name, alias_name)) {
			info("%s: updating alias to %s\n",
			     found->dev_name, alias_name);
			strncpy(found->dev_name, alias_name, MD_NAMELEN);
			found->dev_name[MD_NAMELEN - 1] = '\0';
		} else
			warn("%s: Already monitoring %s", found->dev_name,
			     udev_device_get_devpath(found->device));
	}
	return 0;
}

static void unmonitor_md(const char *md_name)
{
	struct md_monitor *found_md = NULL;

	found_md = lookup_md(md_name, 1);
	if (found_md)
		remove_md(found_md);
}

static void discover_md(struct udev *udev)
{
	struct udev_enumerate *md_enumerate;
	struct udev_list_entry *entry;
	struct udev_device *md_dev;
	const char *md_devpath;

	md_enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_sysname(md_enumerate, "md*");
	udev_enumerate_scan_devices(md_enumerate);

	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(md_enumerate)) {
		md_devpath = udev_list_entry_get_name(entry);
		md_dev = udev_device_new_from_syspath(udev, md_devpath);
		warn("Testing %s", md_devpath);
		if (md_dev) {
			if (monitor_md(md_dev) != 0) {
				md_devpath = udev_device_get_sysname(md_dev);
				unmonitor_md(md_devpath);
				/* udev_device_unref() is already called in unmonitor_md() */
				continue;
            }
			udev_device_unref(md_dev);
		}
	}
	udev_enumerate_unref(md_enumerate);
}

static int display_md_status(struct md_monitor *md_dev, char *buf, int buflen)
{
	struct device_monitor *dev;
	int slot, max_slot = -1;
	int len = 0;
	char status;

	memset(buf, '.', buflen - 1);
	pthread_mutex_lock(&md_dev->device_lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		slot = dev->md_slot_saved;
		if (slot < 0)
			continue;
		if (slot >= max_slot)
			max_slot = slot;
		if (slot >= buflen)
			continue;
		pthread_mutex_lock(&dev->lock);
		status = md_rdev_print_state_short(dev->md_status);
		pthread_mutex_unlock(&dev->lock);
		buf[slot] = status;
		if (slot + 1> len)
			len = slot + 1;
	}
	pthread_mutex_unlock(&md_dev->device_lock);
	max_slot++;
	if (md_dev->raid_disks < buflen && md_dev->raid_disks > max_slot)
		max_slot = md_dev->raid_disks;
	if (max_slot >= buflen) {
		warn("%s: CLI buffer too small, min %d\n",
		     md_dev->dev_name, max_slot);
		max_slot = buflen - 1;
	}
	buf[max_slot] = '\0';
	info("%s: md status %s", md_dev->dev_name, buf);
	return max_slot;
}

static int display_io_status(struct md_monitor *md_dev, char *buf, int buflen)
{
	struct device_monitor *dev;
	int slot, max_slot = -1;
	int len = 0;
	char status;

	memset(buf, '.', buflen - 1);

	pthread_mutex_lock(&md_dev->device_lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		slot = dev->md_slot_saved;
		if (slot < 0)
			continue;
		if (slot >= max_slot)
			max_slot = slot;
		if (slot >= buflen)
			continue;
		pthread_mutex_lock(&dev->lock);
		while (dev->ioctx && dev->io_status == IO_UNKNOWN)
			pthread_cond_wait(&dev->io_cond, &dev->lock);

		status = device_io_print_state_short(dev->io_status);
		pthread_mutex_unlock(&dev->lock);

		buf[slot] = status;
		if (slot + 1> len)
			len = slot + 1;
	}
	pthread_mutex_unlock(&md_dev->device_lock);

	max_slot++;
	if (md_dev->raid_disks < buflen && md_dev->raid_disks > max_slot)
		max_slot = md_dev->raid_disks;
	if (max_slot >= buflen) {
		warn("%s: CLI buffer too small, min %d",
		     md_dev->dev_name, max_slot);
		max_slot = buflen - 1;
	}
	buf[max_slot] = '\0';

	info("%s: io status %s", md_dev->dev_name, buf);
	return max_slot;
}

static int display_md(struct md_monitor *md_dev, char *buf)
{
	const char *mdname = udev_device_get_sysname(md_dev->device);
	struct device_monitor *dev;
	mdu_array_info_t info;
	char status[4096];
	int bufsize = 0, len = 0;
	int rc;

	rc = check_md(md_dev, &info);
	if (rc) {
		return -rc;
	}
	pthread_mutex_lock(&md_dev->device_lock);
	buf[0] = '\0';
	list_for_each_entry(dev, &md_dev->children, siblings) {
		enum md_rdev_status md_status;
		int md_slot = -1;

		md_status = md_rdev_check_state(dev, &md_slot);
		pthread_mutex_lock(&dev->lock);
		md_rdev_update_state(dev, md_status, md_slot);
		while (dev->ioctx && dev->io_status == IO_UNKNOWN)
			pthread_cond_wait(&dev->io_cond, &dev->lock);

		pthread_mutex_unlock(&dev->lock);
		len = sprintf(status, "%s: dev %s slot %d/%d status %s %s\n",
			      mdname, dev->dev_name,
			      dev->md_slot, md_dev->raid_disks,
			      md_rdev_print_state(dev->md_status),
			      device_io_print_state(dev->io_status));
		if ((bufsize + len) > CLI_BUFLEN) {
			warn("%s: CLI buffer too small, min %d",
			     md_dev->dev_name, bufsize + len);
			memset(buf, 0, CLI_BUFLEN);
			len = -ENOMEM;
			break;
		}
		strcat(buf + bufsize, status);
		bufsize += len;
	}
	pthread_mutex_unlock(&md_dev->device_lock);
	/* Strip trailing newline */
	if (bufsize > 0)
		bufsize--;
	return bufsize;
}

static void reset_devices(struct udev_device *dev)
{
	const char *devno;
	struct device_monitor *tmp, *found = NULL;

	devno = udev_device_get_sysname(dev);
	info("%s: reset device", devno);

	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		const char *tmppath;

		tmppath = udev_device_get_devpath(tmp->device);
		if (strstr(tmppath, devno)) {
			found = tmp;
			break;
		}
	}
	unlock_device_list();
	if (found) {
		reset_mirror(found);
	} else {
		info("%s: no device found for reset",
		     udev_device_get_sysname(dev));
	}
}

static void handle_event(struct udev_device *device)
{
	const char *devname = udev_device_get_sysname(device);
	const char *action = udev_device_get_action(device);

	if (!devname || !action)
		return;
	if (!strcmp(action, "add")) {
		if (!strncmp(devname, "dasd", 4) ||
		    !strncmp(devname, "dm-", 3)) {
			attach_device(device);
		}
	} else if (!strcmp(action, "change")) {
		if (!strncmp(devname, "md", 2)) {
			if (monitor_md(device) != 0)
				unmonitor_md(devname);
		}
		if (!strncmp(devname, "dasd", 4) ||
		    !strncmp(devname, "dm-", 3)) {
			attach_device(device);
		}
	} else if (!strcmp(action, "remove")) {
		if (!strncmp(devname, "md", 2)) {
			unmonitor_md(devname);
		}
		if (!strncmp(devname, "dasd" , 4) ||
		    !strncmp(devname, "dm-", 3)) {
			detach_device(device);
		}
	} else if (!strcmp(action, "move")) {
		const char *devpath, *devpath_old;

		/* Move event, generated by z/VM ATTACH/DETACH */
		devpath = udev_device_get_devpath(device);
		devpath_old = udev_device_get_property_value(device, "DEVPATH_OLD");
		/* bsc#953510: udev mangles devpath on CP ATTACH */
		if (strstr(udev_device_get_devpath(device),"defunct") &&
		    strcmp(devpath, devpath_old)) {
			/* Device detached; wait for I/O error */
			warn("%s: device detached", devname);
		} else {
			warn("%s: device attached", devname);
			reset_devices(device);
		}
	}
}

static void print_device(struct udev_device *device)
{
	int prop = 0;

	warn("%-8s %s (%s)",
	       udev_device_get_action(device),
	       udev_device_get_devpath(device),
	       udev_device_get_subsystem(device));
	if (prop) {
		struct udev_list_entry *list_entry;

		udev_list_entry_foreach(list_entry, udev_device_get_properties_list_entry(device))
			warn("%s=%s",
			       udev_list_entry_get_name(list_entry),
			       udev_list_entry_get_value(list_entry));
		warn("");
	}
}

static void *mdadm_exec_thread (void *ctx)
{
	struct mdadm_exec *thr = ctx;
	struct timespec tmo;
	struct timeval start_time, end_time, diff;
	struct list_head active_list;
	struct md_monitor *md_dev, *tmp;

	while (thr->running) {
		INIT_LIST_HEAD(&active_list);
		pthread_mutex_lock(&pending_lock);
		if (list_empty(&pending_list)) {
			int rc;

			info("md_exec: no requests, waiting %ld seconds",
			     failfast_timeout);
			if (gettimeofday(&start_time, NULL)) {
				err("md_exec: failed to get time: %m");
				pthread_mutex_unlock(&pending_lock);
				break;
			}
			tmo.tv_sec = start_time.tv_sec + failfast_timeout;
			tmo.tv_nsec = 0;
			rc = pthread_cond_timedwait(&pending_cond,
						    &pending_lock,
						    &tmo);
			if (rc < 0) {
				pthread_mutex_unlock(&pending_lock);
				if (rc == ETIMEDOUT) {
					dbg("md_exec: timeout");
					continue;
				} else {
					info("md_exec: cannot wait on "
					     "condition, error %d", rc);
					break;
				}
			}
		}
		list_splice_init(&pending_list, &active_list);
		pthread_mutex_unlock(&pending_lock);
		if (list_empty(&active_list))
			continue;
		list_for_each_entry_safe(md_dev, tmp, &active_list, pending) {
			int do_fail = 0, rc = 0;

			if (gettimeofday(&start_time, NULL) < 0)
				start_time.tv_sec = 0;

			pthread_mutex_lock(&md_dev->status_lock);
			list_del_init(&md_dev->pending);
			if (md_dev->pending_status == UNKNOWN) {
				pthread_mutex_unlock(&md_dev->status_lock);
				dbg("%s: task already completed",
				    md_dev->dev_name);
			} else if (md_dev->pending_status != IN_SYNC) {
				pthread_mutex_unlock(&md_dev->status_lock);
				do_fail = 1;
				rc = fail_md(md_dev);
			} else {
				pthread_mutex_unlock(&md_dev->status_lock);
				rc = reset_md(md_dev);
			}

			if (rc < 0) {
				info("%s: mdadm returned %d",
				     md_dev->dev_name, rc);
			} else {
				if (start_time.tv_sec &&
				    gettimeofday(&end_time, NULL) == 0) {
					timersub(&end_time, &start_time, &diff);
					info("%s: all devices %s after "
					     "%lu.%06lu secs", md_dev->dev_name,
					     do_fail ? "failed" : "reset",
					     diff.tv_sec, diff.tv_usec);
				} else {
					info("%s: all devices %s",
					     md_dev->dev_name,
					     do_fail ? "failed" : "reset");
				}
			}
		}
	}

	return ((void *)0);
}

struct mdadm_exec *start_mdadm_exec(void)
{
	struct mdadm_exec *mdx;
	int rc = 0;

	info("Start mdadm exec thread");

	mdx = malloc(sizeof(struct mdadm_exec));
	memset(mdx, 0, sizeof(struct mdadm_exec));
	mdx->running = 1;
	rc = pthread_create(&mdx->thread, &cli_attr, mdadm_exec_thread, mdx);
	if (rc) {
		mdx->thread = 0;
		mdx->running = 0;
		err("Failed to start mdadm exec thread: %m");
		free(mdx);
		mdx = NULL;
	}

	return mdx;
}

void cli_monitor_cleanup(void *ctx)
{
	struct cli_monitor *cli = ctx;

	if (cli->sock >= 0) {
		close(cli->sock);
		cli->sock = 0;
	}
	cli->thread = 0;
	cli->running = 0;
}

void *cli_monitor_thread(void *ctx)
{
	struct cli_monitor *cli = ctx;
	struct md_monitor *md_dev = NULL;
	struct device_monitor *dev = NULL;
	sigset_t mask;

	cli->running = 1;
	pthread_cleanup_push(cli_monitor_cleanup, cli);
	sigemptyset(&mask);
	if (pthread_sigmask(SIG_BLOCK, NULL, &mask)) {
		info("failed to get current signal mask, err %d", errno);
		goto out;
	}
	while (cli->running) {
		int fdcount;
		fd_set readfds;
		struct msghdr smsg;
		struct iovec iov;
		char cred_msg[CMSG_SPACE(sizeof(struct ucred))];
		struct cmsghdr *cmsg;
		struct ucred *cred;
		static char buf[CLI_BUFLEN];
		struct sockaddr_un sun;
		socklen_t addrlen;
		ssize_t buflen;
		char *event, *mdstr, *devstr, *devname;

		FD_ZERO(&readfds);
		FD_SET(cli->sock, &readfds);
		fdcount = pselect(cli->sock + 1, &readfds,
				  NULL, NULL, NULL, &mask);
		if (fdcount < 0) {
			if (errno != EINTR)
				warn("error receiving message");
			continue;
		}
		memset(buf, 0x00, sizeof(buf));
		iov.iov_base = buf;
		iov.iov_len = CLI_BUFLEN;
		memset(&sun, 0x00, sizeof(struct sockaddr_un));
		addrlen = sizeof(struct sockaddr_un);
		memset(&smsg, 0x00, sizeof(struct msghdr));
		smsg.msg_name = &sun;
		smsg.msg_namelen = addrlen;
		smsg.msg_iov = &iov;
		smsg.msg_iovlen = 1;
		smsg.msg_control = cred_msg;
		smsg.msg_controllen = sizeof(cred_msg);

		buflen = recvmsg(cli->sock, &smsg, 0);

		if (buflen < 0) {
			if (errno != EINTR)
				err("error receiving cli message: %m");
			continue;
		}
		cmsg = CMSG_FIRSTHDR(&smsg);
		if (cmsg == NULL) {
			warn("no cli credentials, ignore message");
			continue;
		}
		if (cmsg->cmsg_type != SCM_CREDENTIALS) {
			warn("invalid cli credentials %d/%d, ignore message",
			     cmsg->cmsg_type, cmsg->cmsg_level);
			continue;
		}
		cred = (struct ucred *)CMSG_DATA(cmsg);
		if (cred->uid != 0) {
			warn("sender uid=%d, ignore message", cred->uid);
			continue;
		}
		info("received %d/%d bytes from %s", buflen, sizeof(buf),
		     &sun.sun_path[1]);

		event = buf;
		if (!strncmp(event, "Shutdown", 8)) {
			kill(monitor_pid, SIGTERM);
			cli->running = 0;
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strncmp(event, "Help", 4)) {
			iov.iov_len = sprintf(buf,
					      "Possible commands:\n\tShutdown\n"
					      "\tArrayStatus:/dev/mdX\n"
					      "\tMirrorStatus:/dev/mdX\n"
					      "\tMonitorStatus:/dev/mdX\n"
					      "\tRemove:/dev/mdX@/dev/dasdY");
			goto send_msg;
		}
		mdstr = strchr(buf, ':');
		if (!mdstr || strlen(mdstr) < 2) {
			warn("invalid message '%s' len %d", buf, buflen);
			buf[0] = ENOMSG;
			iov.iov_len = 1;
			goto send_msg;
		}
		*mdstr = '\0';
		mdstr++;
		devstr = strchr(mdstr, '@');
		if (devstr) {
			if (strlen(devstr) > 1) {
				*devstr = '\0';
				devstr++;
			} else if (strlen(devstr)) {
				*devstr = '\0';
				devstr = NULL;
			}
		}
		info("CLI event '%s' md %s device '%s'",
		     event, mdstr, devstr ? devstr : "<NULL>");

		md_dev = lookup_md_alias(mdstr);
		if (!md_dev && strcmp(event, "NewArray")) {
			info("%s: skipping event, array not monitored", mdstr);
			buf[0] = ENODEV;
			iov.iov_len = 1;
			goto send_msg;
		}
		if (!strcmp(event, "RebuildStarted")) {
			info("%s: Rebuild started", md_dev->dev_name);
			md_dev->in_recovery = 1;
			discover_md_components(md_dev);
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "RebuildFinished")) {
			info("%s: Rebuild finished", md_dev->dev_name);
			md_dev->in_recovery = 0;
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "DeviceDisappeared")) {
			struct md_monitor *tmp;

			/*
			 * The device might have been
			 * removed by the time we get here.
			 * So double-check.
			 */
			pthread_mutex_lock(&md_lock);
			md_dev = NULL;
			list_for_each_entry(tmp, &md_list, entry) {
				const char *tmpname;

				if (!strcmp(tmp->dev_name, mdstr)) {
					md_dev = tmp;
					break;
				}
				tmpname = udev_device_get_sysname(tmp->device);
				if (tmpname && !strcmp(tmpname, mdstr)) {
					md_dev = tmp;
					break;
				}
			}
			if (md_dev)
				list_del_init(&md_dev->entry);
			pthread_mutex_unlock(&md_lock);
			if (md_dev) {
				info("%s: array stopped", md_dev->dev_name);
				remove_md(md_dev);
			} else {
				info("%s: array already stopped, ignoring",
				     mdstr);
			}
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "NewArray")) {
			/* No useful information */
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "ArrayStatus")) {
			buflen = display_md(md_dev, buf);
			if (buflen < 0) {
				iov.iov_len = 1;
				buf[0] = -buflen;
			} else {
				iov.iov_len = buflen;
			}
			goto send_msg;
		}
		if (!strcmp(event, "MirrorStatus")) {
			info("%s: display mirror status for %d devices, len %d",
			     mdstr, md_dev->raid_disks, buflen);
			memset(buf, 0x00, CLI_BUFLEN);
			event = NULL;
			mdstr = NULL;
			devstr = NULL;
			buflen = display_md_status(md_dev, buf, CLI_BUFLEN);
			if (buflen < 0) {
				iov.iov_len = 1;
				buf[0] = -buflen;
			} else {
				iov.iov_len = buflen;
			}
			goto send_msg;
		}
		if (!strcmp(event, "MonitorStatus")) {
			info("%s: display monitor status for %d devices, len %d",
			     mdstr, md_dev->raid_disks, buflen);
			memset(buf, 0x00, sizeof(buf));
			event = NULL;
			mdstr = NULL;
			devstr = NULL;
			buflen = display_io_status(md_dev, buf, CLI_BUFLEN);
			if (buflen < 0) {
				iov.iov_len = 1;
				buf[0] = -buflen;
			} else {
				iov.iov_len = buflen;
			}
			goto send_msg;
		}
		if (devstr) {
			devname = strrchr(devstr, '/');
			if (devname)
				devname++;
			else
				devname = devstr;
		} else {
			devname = NULL;
		}
		dev = lookup_md_component(md_dev, devname);
		if (!strcmp(event, "FailSpare") ||
		    !strcmp(event, "Fail")) {
			if (dev) {
				fail_md_component(md_dev, dev);
				buf[0] = 0;
				iov.iov_len = 0;
			} else {
				info("%s: No device for event '%s'",
				     mdstr, event);
				buf[0] = ENODEV;
				iov.iov_len = 1;
			}
		} else if (!strcmp(event, "Remove")) {
			if (dev) {
				remove_md_component(md_dev, dev);
				pthread_mutex_lock(&md_dev->device_lock);
				list_del_init(&dev->siblings);
				pthread_mutex_unlock(&md_dev->device_lock);
				remove_component(dev);
				buf[0] = 0;
				iov.iov_len = 0;
			} else {
				info("%s: No device for event '%s'",
				     mdstr, event);
				buf[0] = ENODEV;
				iov.iov_len = 1;
			}
		} else if (!strcmp(event, "SpareActive")) {
			if (dev)
				sync_md_component(md_dev, dev);
			else
				discover_md_components(md_dev);
			buf[0] = 0;
			iov.iov_len = 0;
		} else {
			info("%s: Unhandled event '%s'", mdstr, event);
			buf[0] = EINVAL;
			iov.iov_len = 1;
		}
	send_msg:
		if (sendmsg(cli->sock, &smsg, 0) < 0)
			err("sendmsg failed: %m");
	}
 out:
	info("shutdown cli monitor");
	pthread_cleanup_pop(1);
	return ((void *)0);
}

struct cli_monitor *monitor_cli(void)
{
	struct cli_monitor *cli;
	struct sockaddr_un sun;
	socklen_t addrlen;
	const int feature_on = 1;
	int rc = 0;

	info("Start cli monitor");

	cli = malloc(sizeof(struct cli_monitor));
	memset(cli, 0, sizeof(struct cli_monitor));

	memset(&sun, 0x00, sizeof(struct sockaddr_un));
	sun.sun_family = AF_LOCAL;
	strcpy(&sun.sun_path[1], "/org/kernel/md/md_monitor");
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(sun.sun_path + 1) + 1;
	cli->sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (cli->sock < 0) {
		err("cannot open cli socket: %m");
		free(cli);
		return NULL;
	}
	if (bind(cli->sock, (struct sockaddr *) &sun, addrlen) < 0) {
		err("cannot bind cli socket: %m");
		close(cli->sock);
		free(cli);
		return NULL;
	}
	if (setsockopt(cli->sock, SOL_SOCKET, SO_PASSCRED,
		       &feature_on, sizeof(feature_on)) < 0) {
		err("cannot enable credentials passing: %m");
		close(cli->sock);
		free(cli);
		return NULL;
	}

	rc = pthread_create(&cli->thread, &cli_attr, cli_monitor_thread, cli);
	if (rc) {
		cli->thread = 0;
		close(cli->sock);
		err("Failed to start cli monitor: %m");
		free(cli);
		cli = NULL;
	}

	return cli;
}

#define POLL_TIMEOUT 10

int cli_command(char *cmd)
{
	struct sockaddr_un sun, local;
	socklen_t addrlen;
	struct msghdr smsg;
	char cred_msg[CMSG_SPACE(sizeof(struct ucred))];
	struct cmsghdr *cmsg;
	struct ucred *cred;
	struct iovec iov;
	int cli_sock, feature_on = 1;
	char buf[CLI_BUFLEN];
	int buflen = 0;
	char status;
	int fdcount = -1;

	cli_sock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (cli_sock < 0) {
		err("cannot open cli socket");
		return 3;
	}
	memset(&local, 0x00, sizeof(struct sockaddr_un));
	local.sun_family = AF_LOCAL;
	sprintf(&local.sun_path[1], "/org/kernel/md/md_monitor/%d", getpid());
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(local.sun_path + 1) + 1;
	if (bind(cli_sock, (struct sockaddr *) &local, addrlen) < 0) {
		err("bind to local cli address failed: %m");
		close(cli_sock);
		return 4;
	}
	if (setsockopt(cli_sock, SOL_SOCKET, SO_PASSCRED,
		       &feature_on, sizeof(feature_on)) < 0) {
		err("enabling credential passing failed: %m");
		close(cli_sock);
		return 6;
	}
	memset(&sun, 0x00, sizeof(struct sockaddr_un));
	sun.sun_family = AF_LOCAL;
	strcpy(&sun.sun_path[1], "/org/kernel/md/md_monitor");
	addrlen = offsetof(struct sockaddr_un, sun_path) +
		strlen(sun.sun_path + 1) + 1;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = cmd;
	iov.iov_len = strlen(cmd) + 1;

	memset(&smsg, 0x00, sizeof(struct msghdr));
	smsg.msg_name = &sun;
	smsg.msg_namelen = addrlen;
	smsg.msg_iov = &iov;
	smsg.msg_iovlen = 1;
	smsg.msg_control = cred_msg;
	smsg.msg_controllen = sizeof(cred_msg);
	memset(cred_msg, 0, sizeof(cred_msg));

	cmsg = CMSG_FIRSTHDR(&smsg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;

	cred = (struct ucred *)CMSG_DATA(cmsg);
	cred->pid = getpid();
	cred->uid = getuid();
	cred->gid = getgid();

	if (sendmsg(cli_sock, &smsg, 0) < 0) {
		if (errno == ECONNREFUSED) {
			err("sendmsg failed, md_monitor is not running");
		} else {
			err("sendmsg failed: %m");
		}
		close(cli_sock);
		return 5;
	}

	while (fdcount < 0) {
		struct timeval tmo;
		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(cli_sock, &readfds);
		tmo.tv_sec = POLL_TIMEOUT;
		tmo.tv_usec = 0;
		fdcount = select(cli_sock + 1, &readfds, NULL, NULL, &tmo);
		if (fdcount < 0) {
			if (errno != EINTR) {
				buflen = -1;
				fdcount = 0;
				break;
			}
			continue;
		} else if (fdcount == 0) {
			/* timeout */
			errno = ETIMEDOUT;
			buflen = -1;
			break;
		}
		memset(buf, 0x00, sizeof(buf));
		iov.iov_base = buf;
		iov.iov_len = CLI_BUFLEN;
		buflen = recvmsg(cli_sock, &smsg, MSG_DONTWAIT);
		if (buflen >= 0 || errno != EAGAIN)
			break;
		dbg("No data received, retrying");
	}
	if (buflen < 0) {
		if (errno == EAGAIN) {
			err("recvmsg failed, md_monitor does not respond");
		} else if (errno == ETIMEDOUT) {
			err("timeout receiving CLI reply");
		} else {
			err("recvmsg failed: %s", strerror(errno));
		}
		status = errno;
	} else if (buflen < 1) {
		/* command ok */
		status = 0;
	} else if (buflen < 2) {
		/* Status message */
		status = buf[0];
		err("CLI message '%s' failed: %s", cmd, strerror(status));
	} else {
		printf("%s\n", buf);
		status = 0;
	}

	close(cli_sock);
	return status;
}

void
setup_thread_attr(pthread_attr_t *attr, size_t stacksize, int detached)
{
	if (pthread_attr_init(attr)) {
		fprintf(stderr, "can't initialize thread attr: %m\n");
		exit(1);
	}
	if (stacksize < PTHREAD_STACK_MIN)
		stacksize = PTHREAD_STACK_MIN;

	if (pthread_attr_setstacksize(attr, stacksize)) {
		fprintf(stderr, "can't set thread stack size to %lu: %m\n",
			(unsigned long)stacksize);
		exit(1);
	}
	if (detached &&
	    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED)) {
		fprintf(stderr, "can't set thread to detached: %m\n");
		exit(1);
	}
}

pid_t daemonize(void)
{
	pid_t pid;
	struct rlimit rl;
	struct sigaction act;
	int i, fd0, fd1, fd2;

	umask(0);
	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		err("cannot get file limit: %m");
		exit (1);
	}
	if ((pid = fork()) < 0) {
		err("fork failed: %m");
		exit(errno);
	} else if (pid != 0)
		exit(0);

	setsid();

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (sigaction(SIGHUP, &act, NULL) < 0) {
		err("sigaction(SIGHUP) failed: %m");
		exit(errno);
	}

	if ((pid = fork()) < 0) {
		err("fork failed: %m");
		exit(errno);
	} else if (pid != 0) {
		fprintf(stdout, "%d\n", pid);
		exit(0);
	}

	if (chdir("/") < 0) {
		err("Cannot chdir to '/': %m");
		exit(1);
	}

	if (rl.rlim_max == RLIM_INFINITY)
		rl.rlim_max = 1024;
	for (i = 0; i < rl.rlim_max; i++)
		close(i);

	fd0 = open("/dev/null", O_RDWR);
	fd1 = dup(0);
	fd2 = dup(0);

	if (fd0 != 0 || fd1 != 1 || fd2 != 2) {
		err("unexpected filedescriptors %d %d %d", fd0, fd1, fd2);
		exit(1);
	}
	return pid;
}

void usage(void)
{
	err("Usage: md_monitor [--daemonize|-d] [--logfile=<file>|-f <file>]"
	    "[--expires=<num>|-e <num>] [--retries=<num>|-r <num>]"
	    "[--command=<cmd>|-c <cmd>] [--daemonize|-d] "
	    "[--expires=<num>|-e <num>] [--logfile=<file>|-l <file>] "
	    "[--process-limit=<num>|-P <num>] [--open-file-limit=<num>|-O <num>] "
	    "[--log-priority=<prio>|-p <prio>] [--retries=<num>|-r <num>] "
	    "[--fail-mirror|-m] [--fail-disk|-o] "
	    "[--syslog|-s] [--verbose|-v] [--version|-V] "
	    "[--check-in-sync|y] [--check-timeout=<secs>|-t <secs>] [--help|-h]\n"
	    "  --command=<cmd>                send command <cmd> to daemon\n"
	    "  --daemonize                    start monitor in background\n"
	    "  --expires=<num>                set failfast_expires to <num>\n"
	    "  --logfile=<file>               use <file> for logging\n"
	    "  --process-limit=<num>          max number of processes\n"
	    "  --open-file-limit=<num>        max number of open files; default is 4096\n"
	    "  --log-priority=<prio>          set logging priority to <prio>\n"
	    "  --retries=<num>                set failfast_retries to <num>\n"
	    "  --fail-mirror                  fail entire mirror side\n"
	    "  --fail-disk                    fail affected disk only\n"
	    "  --syslog                       use syslog for logging\n"
	    "  --check-timeout=<secs>         run path checker every <secs> seconds\n"
	    "  --check-in-sync                run path checker for in_sync devices\n"
	    "  --verbose                      increase logging priority\n"
	    "  --version                      print md_monitor version number\n"
	    "  --help\n");
}

int main(int argc, char *argv[])
{
	struct sigaction act;
	sigset_t mask;
	int option;
	struct udev *udev;
	struct udev_monitor *udev_monitor = NULL;
	struct udev_monitor *kernel_monitor = NULL;
	struct md_monitor *tmp_md, *found_md;
	struct device_monitor *tmp_dev, *found_dev;
	struct cli_monitor *cli = NULL;
	struct mdadm_exec *mdx = NULL;
	unsigned long max_proc, max_files = 4096;
	struct rlimit cur;
	char *command_to_send = NULL;
	char *logfile = NULL;
	struct pollfd readfd;
	int monitor_fd;
	int rc = 0;

	static const struct option options[] = {
		{ "command", required_argument, NULL, 'c' },
		{ "daemonize", no_argument, NULL, 'd' },
		{ "expires", required_argument, NULL, 'e' },
		{ "logfile", required_argument, NULL, 'f' },
		{ "fail-mirror", no_argument, NULL, 'm' },
		{ "open-file-limit", required_argument, NULL, 'O' },
		{ "fail-disk", no_argument, NULL, 'o' },
		{ "process-limit", required_argument, NULL, 'P' },
		{ "log-priority", required_argument, NULL, 'p' },
		{ "retries", required_argument, NULL, 'r' },
		{ "syslog", no_argument, NULL, 's' },
		{ "check-timeout", required_argument, NULL, 't' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "check-in-sync", no_argument, NULL, 'y' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{}
	};

	udev = udev_new();
	if (udev == NULL)
		exit(1);

	logfd = stdout;

	while (1) {
		option = getopt_long(argc, argv, "c:de:f:mO:o:p:P:r:st:vyhV",
				     options, NULL);
		if (option == -1) {
			break;
		}

		switch (option) {
		case 'c':
			command_to_send = optarg;
			break;
		case 'd':
			daemonize_monitor = 1;
			break;
		case 'e':
			failfast_timeout = strtoul(optarg, NULL, 10);
			if (failfast_timeout < 1) {
				err("Invalid expires setting '%s'",
				    optarg);
				exit(1);
			}
			break;
		case 'f':
			logfile = optarg;
			break;
		case 'P':
			max_proc = strtoul(optarg, NULL, 10);
			if (max_proc < 1) {
				err("Invalid limit '%s' for max_processes",
				    optarg);
				exit(1);
			}
			if (getrlimit(RLIMIT_NPROC, &cur) < 0) {
				err("Cannot get current process limit: %m");
				exit(1);
			}
			if (cur.rlim_cur > max_proc) {
				warn("Current process limit "
				     "higher than requested "
				     "(cur %d, req %d)",
				     cur.rlim_cur, max_proc);
				cur.rlim_cur = max_proc;
			} else if (cur.rlim_max > max_proc) {
				info("Increasing soft process limit to %d",
				     max_proc);
				cur.rlim_cur = max_proc;
			} else {
				info("Increasing hard process limit to %d",
				     max_proc);
				cur.rlim_cur = max_proc;
				cur.rlim_max = max_proc;
			}
			if (setrlimit(RLIMIT_NPROC, &cur) < 0) {
				err("Cannot modify process limit: %m");
			}
			break;
		case 'm':
			fail_mirror_side = 1;
			break;
		case 'O':
			max_files = strtoul(optarg, NULL, 10);
			if (max_files < 1) {
				err("Invalid limit '%s' for open-file-limit",
				    optarg);
				exit(1);
			}
			break;
		case 'o':
			fail_mirror_side = 0;
			break;
		case 'p':
			log_priority = strtoul(optarg, NULL, 10);
			if (log_priority > LOG_DEBUG) {
				err("Invalid logging priority %d (max %d)",
				    log_priority, LOG_DEBUG);
				exit(1);
			}
			break;
		case 'r':
			failfast_retries = strtoul(optarg, NULL, 10);
			if (failfast_retries < 2) {
				err("Invalid retries setting '%s'",
				    optarg);
				exit(1);
			}
			break;
		case 's':
			use_syslog = 1;
			break;
		case 't':
			checker_timeout = strtoul(optarg, NULL, 10);
			if (checker_timeout < 1 &&
			    checker_timeout == ULONG_MAX) {
				err("Invalid checker timeout '%s'",
				    optarg);
				exit(1);
			}
			break;
		case 'v':
			log_priority++;
			break;
		case 'y':
			stop_on_sync = 0;
			break;
		case 'V':
			printf("%s\n", version_str);
			return 0;
		case 'h':
		default:
			usage();
			goto out;
		}
	}

	if (use_syslog && logfile) {
		err("Cannot specify both --syslog and --logfile");
		usage();
		exit(1);
	}

	if (command_to_send) {
		return cli_command(command_to_send);
	}

	if (daemonize_monitor)
		daemonize();

	monitor_pid = getpid();
	monitor_timeout = failfast_timeout * (failfast_retries + 1);

	if (use_syslog) {
		sprintf(logname, "md_monitor[%d]", monitor_pid);
		openlog(logname, LOG_CONS, LOG_DAEMON);
	} else if (logfile) {
		int fd2;

		logfd = freopen(logfile, "a", stdout);
		if (!logfd) {
			/* Hehe. We failed to reassign stdout, so we
			 * need to write to stderr. */
			fprintf(stderr, "Cannot open logfile '%s': %m\n",
				logfile);
			exit (1);
		}
		close(2);
		fd2 = dup(1);
		if (fd2 < 0) {
			err("Cannot reassign stderr");
			exit (1);
		}
	}

	setup_thread_attr(&monitor_attr, 64 * 1024, 1);
	setup_thread_attr(&cli_attr, 64 * 1024, 0);

	pthread_mutex_init(&md_lock, NULL);
	pthread_mutex_init(&device_lock, NULL);
	pthread_mutex_init(&pending_lock, NULL);
	pthread_cond_init(&pending_cond, NULL);

	/* set signal handlers */
	memset(&act, 0x00, sizeof(struct sigaction));
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (sigaction(SIGHUP, &act, NULL)) {
		err("cannot restore SIGHUP defaults");
		rc = 3;
		goto out;
	}
	sigemptyset(&thread_sigmask);
	sigaddset(&thread_sigmask, SIGHUP);
	rc = pthread_sigmask(SIG_BLOCK, &thread_sigmask, NULL);
	if (rc) {
		err("cannot set sigmask: %m");
		goto out;
	}
	/* Increase number of open files */
	if (getrlimit(RLIMIT_NOFILE, &cur) < 0) {
		err("Cannot get current number of open files: %m");
		exit(1);
	}
	if (cur.rlim_cur > max_files) {
		warn("Current number of open files "
		     "higher than requested (cur %d, req %d)",
		     cur.rlim_cur, max_files);
		cur.rlim_cur = max_files;
	} else if (cur.rlim_max > max_files) {
		info("Increasing soft open files limit to %d", max_files);
		cur.rlim_cur = max_files;
	} else {
		info("Increasing hard open files limit to %d", max_files);
		cur.rlim_cur = max_files;
		cur.rlim_max = max_files;
	}
	if (setrlimit(RLIMIT_NOFILE, &cur) < 0) {
		err("Cannot modify open files limit: %m");
	}

	info("startup");
	cli = monitor_cli();
	if (!cli)
		goto out;
	mdx = start_mdadm_exec();
	if (!mdx)
		goto out;

	if (start_mpath_check(checker_timeout) < 0)
		goto out;

	/* Discover existing devices */
	discover_devices(udev);

	/* Discover existing MD arrays */
	discover_md(udev);

	udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (udev_monitor == NULL) {
		err("unable to create netlink socket");
		rc = 1;
		goto out;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "block", NULL) < 0) {
		err("unable to apply 'block' subsystem filter");
	}
	if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "ccw", NULL) < 0) {
		err("unable to apply 'ccw' subsystem filter");
	}
	if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "css", NULL) < 0) {
		err("unable to apply 'css' subsystem filter");
	}

	if (udev_monitor_enable_receiving(udev_monitor) < 0) {
		err("unable to subscribe to udev events");
		rc = 2;
		goto out;
	}
	monitor_fd = udev_monitor_get_fd(udev_monitor);
	info("waiting for events");
	while (!udev_exit) {
		int fdcount;
		struct timespec tmo;

		readfd.fd = monitor_fd;
		readfd.events = POLLIN;
		tmo.tv_sec = 1;
		tmo.tv_nsec = 0;
		fdcount = ppoll(&readfd, 1, &tmo, &thread_sigmask);

		if (fdcount < 0) {
			if (errno != EINTR)
				warn("error receiving uevent message: %m");
			continue;
		}
		/* Timeout, just continue */
		if (fdcount == 0)
			continue;

		if (readfd.revents & POLLIN) {
			struct udev_device *device;

			device = udev_monitor_receive_device(udev_monitor);
			if (device == NULL)
				continue;
			print_device(device);
			handle_event(device);
			udev_device_unref(device);
		}
	}

out:
	info("shutting down");
	pthread_mutex_lock(&md_lock);
	list_for_each_entry_safe(found_md, tmp_md, &md_list, entry) {
		list_del_init(&found_md->entry);
		remove_md(found_md);
	}
	pthread_mutex_unlock(&md_lock);

	lock_device_list();
	list_for_each_entry_safe(found_dev, tmp_dev, &device_list, entry) {
		if (found_dev->device)
			info("%s: detached '%s'", found_dev->dev_name,
			     udev_device_get_devpath(found_dev->device));
		list_del_init(&found_dev->entry);
		if (found_dev->device)
			device_monitor_put(found_dev);
	}
	unlock_device_list();

	if (cli && cli->thread) {
		pthread_cancel(cli->thread);
		pthread_join(cli->thread, NULL);
	}
	free(cli);

	stop_mpath_check();

	if (mdx && mdx->thread) {
		pthread_cancel(mdx->thread);
		pthread_join(mdx->thread, NULL);
	}
	free(mdx);

	pthread_attr_destroy(&monitor_attr);
	pthread_attr_destroy(&cli_attr);
	udev_monitor_unref(udev_monitor);
	udev_monitor_unref(kernel_monitor);
	udev_unref(udev);

	if (num_time > 0) {
		unsigned long rem, usec;

		rem = sum_time.tv_sec % num_time;
		usec = sum_time.tv_usec + rem * 1000000;
		info("avg device lookup time %lu.%06lu",
		     sum_time.tv_sec / num_time, usec / num_time);
	}
	info("shutdown, rc %d", rc);
	if (use_syslog)
		closelog();
	return rc;
}
