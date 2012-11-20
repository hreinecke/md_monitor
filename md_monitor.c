/*
 * DASD Monitor
 *
 * Monitor DASD devices when MD is active
 *
 * Copyright (C) 2011 Hannes Reinecke <hare@suse.de>
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
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
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

#include "list.h"

#include <libudev.h>

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
#endif

const char version_str[] = "md_monitor version 4.9";

enum md_rdev_status {
	UNKNOWN,	/* Not checked */
	IN_SYNC,	/* device is in sync */
	FAULTY,		/* device has been marked faulty */
	TIMEOUT,	/* device has been marked faulty and timeout */
	SPARE,		/* device has been marked spare */
	RECOVERY,	/* device is in recovery */
	REMOVED,	/* device is faulty,
			 * 'remove' and 're-add' has been sent */
	PENDING,	/* device is in_sync, 'faulty' has been send */
	BLOCKED,	/* md is blocked */
	RESERVED,	/* end marker */
};

enum dasd_io_status {
	IO_UNKNOWN,
	IO_ERROR,
	IO_OK,
	IO_FAILED,
	IO_PENDING,
	IO_TIMEOUT,
	IO_RESERVED
};

struct mdadm_exec {
	int running;
	pthread_t thread;
};

#define CLI_BUFLEN 1024

struct cli_monitor {
	int running;
	int sock;
	pthread_t thread;
};

struct md_monitor {
	struct list_head entry;
	struct list_head children;
	struct udev_device *device;
	pthread_mutex_t lock;
	struct list_head pending;
	enum md_rdev_status pending_status;
	int pending_side;
	int raid_disks;
	int layout;
	int in_recovery;
	int degraded;
};

struct device_monitor {
	struct list_head entry;
	struct list_head siblings;
	struct udev_device *device;
	struct udev_device *parent;
	char dev_name[64];
	char md_name[64];
	pthread_t thread;
	pthread_mutex_t lock;
	enum md_rdev_status md_status;
	enum dasd_io_status io_status;
	int md_index;
	int md_slot;
	dev_t md_devt;
	int fd;
	int running;
	int aio_active;
	struct timeval aio_start_time;
	struct iocb io;
	io_context_t ioctx;
	int blksize;
	unsigned char *buf;
};

LIST_HEAD(md_list);
LIST_HEAD(device_list);
LIST_HEAD(pending_list);
pthread_mutex_t device_lock;
pthread_mutex_t pending_lock;
pthread_cond_t pending_cond;
pthread_attr_t monitor_attr;
pthread_attr_t cli_attr;

static int udev_exit;
static int monitor_timeout;
static int failfast_timeout = 5;
static int failfast_retries = 2;
static int monitor_round_limit = 200000;
static sigset_t thread_sigmask;
static int daemonize_monitor;
static int log_priority = LOG_INFO;
static char logname[64];
static int use_syslog;
static int fail_mirror_side = 1;
static int stop_on_sync = 1;
static int checker_timeout = 1;
static int adjust_timeout = 0;
static pid_t monitor_pid;
FILE *logfd;

static void log_fn(int priority, const char *format, ...)
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
	}
	va_end(ap);
}

#define dbg(fmt, args...) log_fn(LOG_DEBUG, fmt "\n", ##args)
#define info(fmt, args...) log_fn(LOG_INFO, fmt "\n", ##args)
#define warn(fmt, args...) log_fn(LOG_WARNING, fmt "\n", ##args)
#define err(fmt, args...) log_fn(LOG_ERR, fmt "\n", ##args)

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
	{ RESERVED, 0, NULL }
};

const char *md_rdev_print_state(enum md_rdev_status state)
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

struct dasd_io_state_t {
	enum dasd_io_status state;
	char desc_short;
	char *desc;
} dasd_io_state_list[] = {
	{ IO_UNKNOWN, '.', "unknown"},
	{ IO_ERROR, 'X', "internal error"},
	{ IO_OK, 'A', "I/O ok"},
	{ IO_FAILED, 'W', "I/O failed"},
	{ IO_PENDING, 'R', "I/O pending"},
	{ IO_TIMEOUT, 'T', "I/O timeout"},
	{ IO_RESERVED, 0, NULL }
};

const char *dasd_io_print_state(enum dasd_io_status state)
{
	struct dasd_io_state_t *dasd_io_state_ptr = dasd_io_state_list;

	while (dasd_io_state_ptr->desc &&
	       dasd_io_state_ptr->state != state)
		dasd_io_state_ptr++;
	return dasd_io_state_ptr->desc;
}

const char dasd_io_print_state_short(enum dasd_io_status state)
{
	struct dasd_io_state_t *dasd_io_state_ptr = dasd_io_state_list;

	while (dasd_io_state_ptr->desc_short &&
	       dasd_io_state_ptr->state != state)
		dasd_io_state_ptr++;
	return dasd_io_state_ptr->desc_short;
}

static void add_component(struct md_monitor *, struct device_monitor *,
			  const char *);
static void remove_component(struct device_monitor *);
static int fail_component(struct md_monitor *, struct device_monitor *,
			  enum md_rdev_status);
static void reset_component(struct device_monitor *);
static void fail_mirror(struct device_monitor *, enum md_rdev_status);
static void reset_mirror(struct device_monitor *);
static void remove_md_component(struct md_monitor *md_dev,
				struct device_monitor *dev);
static int dasd_set_attribute(struct device_monitor *dev, const char *attr,
			      int value);
static void monitor_dasd(struct device_monitor *);

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

static void sig_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		fflush(logfd);
		udev_exit = 1;
	}
}

static struct md_monitor *lookup_md(struct udev_device *mddev)
{
	const char *mdname = udev_device_get_sysname(mddev);
	struct md_monitor *tmp, *md = NULL;
	const char *tmpname;

	if (!mdname)
		return NULL;

	list_for_each_entry(tmp, &md_list, entry) {
		tmpname = udev_device_get_sysname(tmp->device);
		if (tmpname && !strcmp(tmpname, mdname)) {
			md = tmp;
			break;
		}
	}

	return md;
}

static struct md_monitor *lookup_md_alias(const char *mdpath)
{
	struct md_monitor *tmp, *md = NULL;
	const char *tmpname;
	const char *mdname;

	if (!mdpath || strlen(mdpath) == 0)
		return NULL;

	mdname = strrchr(mdpath,'/');
	if (!mdname)
		mdname = mdpath;
	else
		mdname++;

	list_for_each_entry(tmp, &md_list, entry) {
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

	return md;
}

static struct device_monitor * lookup_md_component(struct md_monitor *md_dev,
						   const char *devname)
{
	const char *mdname;
	struct device_monitor *tmp, *found = NULL;
	int lookup_symlinks = 0;

	if (!md_dev || !md_dev->device)
		return NULL;
	mdname = udev_device_get_sysname(md_dev->device);

	if (!devname)
		return NULL;
	if (strncmp(devname, "dasd", 4)) {
		lookup_symlinks = 1;
	}
	pthread_mutex_lock(&md_dev->lock);
	list_for_each_entry(tmp, &md_dev->children, siblings) {
		if (lookup_symlinks) {
			struct udev_list_entry *entry;
			const char *tmpname, *ptr;

			udev_list_entry_foreach(entry,
				udev_device_get_devlinks_list_entry(tmp->device)) {
				tmpname = udev_list_entry_get_name(entry);
				ptr = strrchr(tmpname, '/');
				if (!ptr) {
					info("%s: invalid symlink %s",
					     mdname, tmpname);
					continue;
				}
				ptr++;
				if (!strncmp(ptr, devname, strlen(ptr))){
					found = tmp;
					goto out;
				}
			}
		} else {
			if (!strncmp(devname, tmp->md_name,
				     strlen(tmp->md_name))) {
				found = tmp;
				break;
			}
		}
	}
out:
	pthread_mutex_unlock(&md_dev->lock);
	return found;
}

static void attach_dasd(struct udev_device *dev)
{
	struct md_monitor *found_md = NULL;
	struct device_monitor *tmp, *found = NULL;
	const char *dasd_devtype;
	const char *devname = udev_device_get_sysname(dev);
	char devpath[256];
	DIR *dirp;
	struct dirent *dirfd;

	dasd_devtype = udev_device_get_devtype(dev);
	dbg("dev %s devtype %s", devname, dasd_devtype);
	if (!dasd_devtype || !strncmp(dasd_devtype, "disk", 4)) {
		/* Not a partition, ignore */
		info("%s: not a partition, ignore", devname);
		return;
	}

	sprintf(devpath, "%s/holders",
		udev_device_get_syspath(dev));
	dirp = opendir(devpath);
	if (!dirp) {
		/* directory not present, that's okay */
		dbg("%s: no holders directoy in sysfs, skipping",
		    devname);
		return;
	}
	while ((dirfd = readdir(dirp))) {
		if (dirfd->d_name[0] == '.')
			continue;
		found_md = lookup_md_alias(dirfd->d_name);
		if (found_md)
			break;
	}
	closedir(dirp);
	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		if (!strcmp(tmp->md_name, devname)) {
			found = tmp;
			break;
		}
	}
	if (found) {
		info("%s: already attached", found->dev_name);
	} else {
		found = malloc(sizeof(struct device_monitor));
		memset(found, 0, sizeof(struct device_monitor));
		found->device = udev_device_get_parent(dev);
		found->md_slot = -1;
		found->io_status = IO_UNKNOWN;
		pthread_mutex_init(&found->lock, NULL);
		INIT_LIST_HEAD(&found->siblings);
		udev_device_ref(found->device);
		strcpy(found->dev_name, udev_device_get_sysname(dev));
		list_add(&found->entry, &device_list);
		info("%s: attached '%s'", found->dev_name,
		     udev_device_get_devpath(dev));
	}
	unlock_device_list();
	if (found_md) {
		add_component(found_md, found, devname);
		pthread_mutex_lock(&found_md->lock);
		if (list_empty(&found->siblings))
			list_add(&found->siblings, &found_md->children);
		pthread_mutex_unlock(&found_md->lock);
		monitor_dasd(found);
	} else {
		dbg("%s: no md array found", devname);
	}
}

static void detach_dasd(struct udev_device *dev)
{
	struct device_monitor *tmp, *found = NULL;
	const char *dasd_name;

	dasd_name = udev_device_get_sysname(dev);
	if (!dasd_name)
		return;
	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		if (!strncmp(tmp->dev_name, dasd_name, strlen(dasd_name))) {
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

			md_dev = lookup_md(found->parent);
			if (md_dev) {
				remove_md_component(md_dev, found);
				pthread_mutex_lock(&md_dev->lock);
				remove_component(found);
				pthread_mutex_unlock(&md_dev->lock);
			}
		}
		udev_device_unref(found->device);
		pthread_mutex_destroy(&found->lock);
		free(found);
	} else {
		warn("%s: no device to detach", dasd_name);
	}
}

static void discover_dasd(struct udev *udev)
{
	struct udev_enumerate *dasd_enumerate;
	struct udev_list_entry *entry;
	struct udev_device *dasd_dev;
	const char *dasd_devpath;

	dasd_enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_sysname(dasd_enumerate, "dasd*");
	udev_enumerate_scan_devices(dasd_enumerate);

	udev_list_entry_foreach(entry,
				udev_enumerate_get_list_entry(dasd_enumerate)) {
		dasd_devpath = udev_list_entry_get_name(entry);
		dasd_dev = udev_device_new_from_syspath(udev, dasd_devpath);
		if (dasd_dev) {
			attach_dasd(dasd_dev);
			udev_device_unref(dasd_dev);
		}
	}
	udev_enumerate_unref(dasd_enumerate);
}

enum md_rdev_status md_rdev_check_state(struct device_monitor *dev)
{
	int ioctl_fd;
	char attrpath[256];
	mdu_disk_info_t info;
	enum md_rdev_status md_status;
	const char *sysname;

	if (!dev)
		return REMOVED;
	sysname = udev_device_get_sysname(dev->parent);
	if (!sysname)
		return REMOVED;
	sprintf(attrpath, "/dev/%s", sysname);
	ioctl_fd = open(attrpath, O_RDWR|O_NONBLOCK);
	if (ioctl_fd < 0) {
		warn("%s: cannot open %s: %d",
		     dev->dev_name, attrpath, errno);
		return REMOVED;
	}
	info.number = dev->md_index;
	if (ioctl(ioctl_fd, GET_DISK_INFO, &info) < 0) {
		err("%s: ioctl GET_DISK_INFO failed, error %d",
		    dev->dev_name, errno);
		info.state = 1 << MD_DISK_REMOVED;
	}
	close(ioctl_fd);

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
	else
		md_status = SPARE;

	info("%s: MD rdev state %s (%x)",
	     dev->dev_name, md_rdev_print_state(md_status), info.state);

	return md_status;
}

enum md_rdev_status md_rdev_update_state(struct device_monitor *dev,
					 enum md_rdev_status md_status)
{
	enum md_rdev_status old_status = dev->md_status;

	if (dev->md_status == PENDING) {
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
	} else if (dev->md_status == RECOVERY) {
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
	} else {
		dev->md_status = md_status;
	}
	if (old_status != dev->md_status)
		info("%s: md state update from %s to %s", dev->dev_name,
		     md_rdev_print_state(old_status),
		     md_rdev_print_state(dev->md_status));

	return md_status;
}

int md_set_attribute(struct md_monitor *md_dev, const char *attr,
		     const char *value)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	int attr_fd;
	char attrpath[256];
	char status[64];
	size_t len;
	int rc = 0;

	sprintf(attrpath, "%s/md/%s", udev_device_get_syspath(md_dev->device),
		attr);
	attr_fd = open(attrpath, O_RDWR);
	if (attr_fd < 0) {
		warn("%s: failed to open '%s' attribute for %s: %d",
		     md_name, attr, attrpath, errno);
		rc = errno;
		goto remove;
	}

	memset(status, 0, sizeof(status));
	len = read(attr_fd, status, sizeof(status));
	if (len < 0) {
		warn("%s: cannot read '%s' attribute: %d",
		     md_name, attr, errno);
		rc = errno;
		goto remove;
	}
	if (len == 0) {
		warn("%s: EOF on reading '%s' attribute", md_name, attr);
		goto remove;
	}
	if (status[len - 1] == '\n') {
		status[len - 1] = '\0';
		len--;
	}
	if (!strlen(status)) {
		warn("%s: empty '%s' attribute", md_name, attr);
		goto remove;
	}

	len = write(attr_fd, value, strlen(value));
	if (len < 0) {
		warn("%s: cannot set '%s' attribute to '%s': %d",
		     md_name, attr, value, errno);
		rc = errno;
	}
	info("%s: '%s' = '%s' -> '%s'", md_name, attr, status, value);
	rc = 0;
remove:
	if (attr_fd >= 0)
		close(attr_fd);
	return rc;
}

int dasd_set_attribute(struct device_monitor *dev, const char *attr, int value)
{
	struct udev_device *parent;
	int attr_fd;
	char attrpath[256];
	char status[64], *eptr;
	size_t len;
	int oldvalue;
	int rc = 0;

	parent = udev_device_get_parent(dev->device);
	if (!parent)
		return 0;
	sprintf(attrpath, "%s/%s", udev_device_get_syspath(parent), attr);
	attr_fd = open(attrpath, O_RDWR);
	if (attr_fd < 0) {
		info("%s: failed to open '%s' attribute for %s: %d",
		     dev->dev_name, attr, attrpath, errno);
		return 0;
	}

	memset(status, 0, sizeof(status));
	len = read(attr_fd, status, sizeof(status));
	if (len < 0) {
		warn("%s: cannot read '%s' attribute: %d",
		     dev->dev_name, attr, errno);
		rc = errno;
		goto remove;
	}
	if (len == 0) {
		warn("%s: EOF on reading '%s' attribute", dev->dev_name, attr);
		goto remove;
	}
	if (status[len - 1] == '\n') {
		status[len - 1] = '\0';
		len--;
	}
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
			warn("%s: cannot set '%s' attribute to '%s': %d",
			     dev->dev_name, attr, status, errno);
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

static void dasd_timeout_ioctl(struct device_monitor *dev, int set)
{
	int ioctl_arg = set ? BIODASDTIMEOUT : BIODASDRESYNC;

	if (dev->fd < 0) {
		const char *devnode = udev_device_get_devnode(dev->device);

		if (!devnode) {
			warn("%s: device removed", dev->dev_name);
		} else {
			dev->fd = open(devnode, O_RDONLY);
			if (dev->fd < 0) {
				warn("%s: cannot open %s: %d",
				     dev->dev_name, devnode, errno);
			}
		}
	}
	if (dev->fd >= 0 && ioctl(dev->fd, ioctl_arg) < 0) {
		info("%s: cannot %s DASD timeout flag, error %d",
		     dev->dev_name, set ? "set" : "unset", errno);
	} else {
		dbg("%s: %s DASD timeout flag", dev->dev_name,
		    set ? "set" : "unset");
	}
}

static int dasd_setup_aio(struct device_monitor *dev)
{
	const char *devnode = udev_device_get_devnode(dev->device);
	int rc, flags;

	rc = io_setup(1, &dev->ioctx);
	if (rc != 0) {
		warn("%s: io_setup failed with %d",
		     dev->dev_name, -rc);
		dev->ioctx = 0;
		return 1;
	}
	if (devnode && dev->fd < 0)
		dev->fd = open(devnode, O_RDONLY);
	if (dev->fd  < 0) {
		warn("%s: cannot open %s: %d",
		     dev->dev_name, devnode, errno);
		return 2;
	}
	flags = fcntl(dev->fd, F_GETFL);
	if (flags < 0) {
		warn("%s: fcntl GETFL failed: %d",
		     dev->dev_name, errno);
		return 3;
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

static enum dasd_io_status dasd_check_aio(struct device_monitor *dev,
					  int timeout)
{
	struct iocb *ios[1] = { &dev->io };
	unsigned long pgsize = getpagesize();
	unsigned char *ioptr;
	struct io_event event;
	struct timespec	tmo = { .tv_sec = timeout };
	int rc;
	enum dasd_io_status io_status = IO_UNKNOWN;
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
			warn("md_exec: failed to get time, error %d",
			     errno);
			dev->aio_start_time.tv_sec = 0;
		}
		if (io_submit(dev->ioctx, 1, ios) != 1) {
			warn("%s: io_submit failed: %d",
			     dev->dev_name, errno);
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
		} else {
			warn("%s: no events", dev->dev_name);
			io_status = IO_PENDING;
		}
	} else {
		struct timeval diff, end_time;
		int timeout_adj;

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
		if (adjust_timeout) {
			if (diff.tv_usec > monitor_round_limit)
				diff.tv_sec++;
			timeout_adj = monitor_timeout - diff.tv_sec;
			if (timeout_adj < 0) {
				warn("%s: I/O latency too high, not adjusting",
				     dev->dev_name);
			} else {
				info("%s: adjusting timeout to %d",
				     dev->dev_name, timeout_adj);
				dasd_set_attribute(dev, "timeout", timeout_adj);
			}
		}
	}
	return io_status;
}

void dasd_monitor_cleanup(void *data)
{
	struct device_monitor *dev = data;
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
		close(dev->fd);
		dev->fd = -1;
	}
	if (dev->buf) {
		free(dev->buf);
		dev->buf = NULL;
	}
	info("%s: shutdown dasd monitor thread", dev->dev_name);
	dev->running = 0;
	dev->thread = 0;
	udev_device_unref(dev->device);
}

void *dasd_monitor_thread (void *ctx)
{
	struct device_monitor *dev = ctx;
	unsigned long pgsize = getpagesize();
	enum dasd_io_status io_status;
	enum md_rdev_status md_status, new_status;
	struct timespec tmo;
	int rc, aio_timeout = 0, sig_timeout = checker_timeout;

	dev->buf = NULL;
	dev->fd = -1;
	dev->aio_active = 0;
	pthread_cleanup_push(dasd_monitor_cleanup, dev);
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

	while (dev->running) {
		dbg("%s: check aio state, timeout %d secs",
		    dev->dev_name, aio_timeout);
		io_status = dasd_check_aio(dev, aio_timeout);
		if (io_status == IO_UNKNOWN)
			continue;
		if (io_status == IO_ERROR) {
			warn("%s: error during aio submission, exit",
			     dev->dev_name);
			break;
		}
		/* Re-check; status might have been changed during aio */
		md_status = md_rdev_check_state(dev);
		/* Write status back */
		pthread_mutex_lock(&dev->lock);
		new_status = md_rdev_update_state(dev, md_status);
		if (io_status == IO_PENDING) {
			/*
			 * io_getevents or sigtimedwait
			 * got interrupted by a signal.
			 * Check whether we need to fail the mirror.
			 */
			pthread_mutex_unlock(&dev->lock);
			info("%s: path checker interrupted", dev->dev_name);
			if (new_status == FAULTY || new_status == TIMEOUT) {
				fail_mirror(dev, new_status);
			}
			aio_timeout = monitor_timeout;
			continue;
		}
		dev->io_status = io_status;
		pthread_mutex_unlock(&dev->lock);
		if (io_status == IO_TIMEOUT) {
			info("%s: I/O timeout", dev->dev_name);
			/* Wait for real I/O timeout error */
		} else if (io_status != IO_OK) {
			switch (new_status) {
			case RECOVERY:
				warn("%s: failing device in recovery",
				     dev->dev_name);
				fail_mirror(dev, new_status);
				break;
			case IN_SYNC:
				warn("%s: failing device in_sync",
				     dev->dev_name);
				new_status = FAULTY;
			case PENDING:
			case FAULTY:
			case TIMEOUT:
				fail_mirror(dev, new_status);
				break;
			default:
				warn("%s: Invalid array state", dev->dev_name);
				break;
			}
		} else {
			switch (new_status) {
			case IN_SYNC:
				if (stop_on_sync) {
					info("%s: path ok, stopping monitor",
					     dev->dev_name);
					sig_timeout = 0;
					dev->running = 0;
				}
				break;
			case RECOVERY:
			case BLOCKED:
			case FAULTY:
			case TIMEOUT:
				reset_mirror(dev);
				/* Fallthrough */
			default:
				break;
			}
		}
		info("%s: state %s / %s",
		     dev->dev_name, md_rdev_print_state(new_status),
		     dasd_io_print_state(io_status));
		if (!sig_timeout)
			break;
		tmo.tv_sec = sig_timeout;
		tmo.tv_nsec = 0;
		info("%s: waiting %ld seconds ...",
		     dev->dev_name, (long)tmo.tv_sec);
		rc = sigtimedwait(&thread_sigmask, NULL, &tmo);
		if (rc < 0) {
			if (errno == EINTR) {
				info("%s: ignore signal",
				     dev->dev_name);
			} else if (errno != EAGAIN) {
				info("%s: signal %d (%d)",
				     dev->dev_name, rc, errno);
				break;
			}
			aio_timeout = monitor_timeout;
		} else {
			info("%s: wait interrupted",
			     dev->dev_name);
			aio_timeout = 0;
		}
	}

	pthread_cleanup_pop(1);
	return ((void *)0);
}

static void monitor_dasd(struct device_monitor *dev)
{
	int rc;

	if (dev->running) {
		/* check if thread is still alive */
		if (dev->thread) {
			/* Yes, everything is okay */
			info("%s: notify monitor thread",
			     dev->dev_name);
			pthread_kill(dev->thread, SIGHUP);
			return;
		}
		info("%s: Re-start monitor", dev->dev_name);
		dev->running = 0;
	} else {
		/* Start new monitor thread */
		info("%s: Start new monitor", dev->dev_name);
	}
	udev_device_ref(dev->device);
	dev->running = 1;
	rc = pthread_create(&dev->thread, &monitor_attr,
			    dasd_monitor_thread, dev);
	if (rc) {
		dev->thread = 0;
		dev->running = 0;
		dev->io_status = IO_UNKNOWN;
		warn("%s: Failed to start monitor thread, error %d",
		     dev->dev_name, rc);
		udev_device_unref(dev->device);
	}
}

static void add_component(struct md_monitor *md, struct device_monitor *dev,
	const char *md_name)
{
	info("Add component %s", dev->dev_name);
	if (!dev->parent) {
		udev_device_ref(md->device);
		dev->parent = md->device;
	}
	strncpy(dev->md_name, md_name, strlen(md_name));
	dasd_set_attribute(dev, "failfast", 1);
	dasd_set_attribute(dev, "failfast_retries", failfast_retries);
	dasd_set_attribute(dev, "failfast_expires", failfast_timeout);
}

static void remove_component(struct device_monitor *dev)
{
	pthread_t thread = dev->thread;

	info("Remove component %s", dev->dev_name);

	list_del_init(&dev->siblings);

	if (thread) {
		pthread_cancel(thread);
	}
	udev_device_unref(dev->parent);
	dev->parent = NULL;
}

static int fail_component(struct md_monitor *md_dev,
			  struct device_monitor *dev,
			  enum md_rdev_status new_status)
{
	enum md_rdev_status md_status;
	int rc = 0;

	/* Always set the timeout flag */
	if (new_status == TIMEOUT) {
		dasd_timeout_ioctl(dev, 1);
	}

	/* Check state if we need to do anything here */
	md_status = md_rdev_check_state(dev);
	if (md_status == new_status) {
		info("%s: already in state '%s'",
		     dev->dev_name, md_rdev_print_state(md_status));
		return rc;
	}

	pthread_mutex_lock(&dev->lock);

	if (dev->running && dev->thread) {
		if (new_status == REMOVED)
			dev->running = 0;
		info("%s: notify monitor thread",
		     dev->dev_name);
		pthread_kill(dev->thread, SIGHUP);
		rc = EBUSY;
	}

	pthread_mutex_unlock(&dev->lock);
	return rc;
}

static void reset_component(struct device_monitor *dev)
{
	dasd_timeout_ioctl(dev, 0);

	pthread_mutex_lock(&dev->lock);
	switch (dev->md_status) {
	case FAULTY:
	case TIMEOUT:
	case REMOVED:
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
}

static void fail_mirror(struct device_monitor *dev, enum md_rdev_status status)
{
	struct md_monitor *md_dev;
	struct device_monitor *tmp;
	const char *md_name;
	int side;

	md_dev = lookup_md(dev->parent);
	if (!md_dev) {
		warn("%s: No md device found", dev->dev_name);
		return;
	}

	if (!fail_mirror_side || status == REMOVED) {
		fail_component(md_dev, dev, status);
		return;
	}

	pthread_mutex_lock(&md_dev->lock);
	md_name = udev_device_get_sysname(md_dev->device);
	if (dev->md_slot < 0) {
		int nr_devs[2];

		memset(nr_devs, 0, sizeof(int) * 2);
		/* Try to figure out which side to fail */
		list_for_each_entry(tmp, &md_dev->children, siblings) {
			if (tmp->md_slot < 0)
				continue;
			side = tmp->md_slot % (md_dev->layout & 0xFF);
			nr_devs[side]++;
		}
		if (nr_devs[0] == nr_devs[1]) {
			warn("%s: slot number unknown", dev->dev_name);
			pthread_mutex_unlock(&md_dev->lock);
			return;
		}
		side = nr_devs[0] > nr_devs[1] ? 1 : 0;
	} else {
		side = dev->md_slot % (md_dev->layout & 0xFF);
	}

	if (md_dev->degraded & (1 << side)) {
		/* Mirror side is already failed, nothing to be done here */
		info("%s: mirror side %d is already failed", md_name, side);
	} else if (md_dev->degraded) {
		/* Mirror is already degraded, do not notify md */
		info("%s: other mirror side for %d is already failed",
		     md_name, side);
		list_for_each_entry(tmp, &md_dev->children, siblings) {
			if ((tmp->md_slot % (md_dev->layout & 0xFF)) == side) {
				pthread_mutex_lock(&tmp->lock);
				tmp->md_status = BLOCKED;
				pthread_mutex_unlock(&tmp->lock);
			}
		}
		md_dev->degraded |= (1 << side);
	} else {
		info("%s: Failing all devices on side %d", md_name, side);
		if (list_empty(&md_dev->pending)) {
			pthread_mutex_lock(&pending_lock);
			md_dev->pending_status = status;
			md_dev->pending_side = (1 << side);
			list_add(&md_dev->pending, &pending_list);
			pthread_cond_signal(&pending_cond);
			pthread_mutex_unlock(&pending_lock);
		} else {
			info("%s: fail scheduled", md_name);
		}
	}

	pthread_mutex_unlock(&md_dev->lock);
}

static void reset_mirror(struct device_monitor *dev)
{
	struct md_monitor *md_dev;
	int ready_devices;
	int side;
	struct device_monitor *tmp;
	const char *md_name;

	md_dev = lookup_md(dev->parent);
	if (!md_dev) {
		warn("%s: No md device found", dev->dev_name);
		return;
	}
	side = dev->md_slot % (md_dev->layout & 0xFF);
	md_name = udev_device_get_sysname(md_dev->device);
	info("%s: reset mirror side %d", md_name, side);
	ready_devices = 0;
	pthread_mutex_lock(&md_dev->lock);
	list_for_each_entry(tmp, &md_dev->children, siblings) {
		int this_side = tmp->md_slot % (md_dev->layout & 0xFF);
		dbg("%s: dev %s side %d state %s / %s", md_name, tmp->dev_name,
		     this_side, md_rdev_print_state(tmp->md_status),
		     dasd_io_print_state(tmp->io_status));
		if (this_side != side)
			ready_devices++;
		else if (tmp->io_status == IO_OK)
			ready_devices++;
	}
	/* Not enough devices, don't reset mirror side */
	if (ready_devices != md_dev->raid_disks) {
		info("%s: not enough devices to reset (%d/%d)", md_name,
		     ready_devices, md_dev->raid_disks);
		pthread_mutex_unlock(&md_dev->lock);
		return;
	}
	info("%s: reset mirror, %d of %d devices ready", md_name,
	     ready_devices, md_dev->raid_disks);
	if (!list_empty(&md_dev->pending)) {
		info("%s: reset scheduled", md_name);
		pthread_mutex_unlock(&md_dev->lock);
		return;
	}
	pthread_mutex_lock(&pending_lock);
	md_dev->pending_status = IN_SYNC;
	md_dev->pending_side = (1 << side);
	list_add(&md_dev->pending, &pending_list);
	pthread_cond_signal(&pending_cond);
	pthread_mutex_unlock(&pending_lock);
	pthread_mutex_unlock(&md_dev->lock);
}

static void fail_md_component(struct md_monitor *md_dev,
			      struct device_monitor *dev)
{
	enum md_rdev_status md_status;

	/*
	 * Fail does not necessarily indicate the device has gone,
	 * so just invoke a state check.
	 */
	info("%s: notify for device state change", dev->dev_name);

	md_status = md_rdev_check_state(dev);
	if (md_status == REMOVED || md_status == SPARE) {
		warn("%s: device status %s", dev->dev_name,
		     md_rdev_print_state(md_status));
		return;
	}
	pthread_mutex_lock(&dev->lock);
	dev->md_status = md_status;
	if (md_status == TIMEOUT)
		dev->io_status = IO_TIMEOUT;
	else if (md_status == FAULTY)
		dev->io_status = IO_FAILED;
	else
		dev->io_status = IO_OK;
	pthread_mutex_unlock(&dev->lock);
	if (md_status != IN_SYNC)
		fail_mirror(dev, md_status);

	monitor_dasd(dev);
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
	monitor_dasd(dev);
}

static void remove_md_component(struct md_monitor *md_dev,
				struct device_monitor *dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);

	pthread_mutex_lock(&dev->lock);
	if (dev->md_status == PENDING) {
		warn("%s: mdadm call still pending", dev->dev_name);
	}
	info("%s: setting '%s' to REMOVED",
	     md_name, dev->dev_name);
	dev->md_status = REMOVED;
	pthread_mutex_unlock(&dev->lock);
	if (dev->running && dev->thread) {
		pthread_t thread = dev->thread;

		info("%s: shutdown monitor thread",
		     dev->dev_name);
		dev->running = 0;
		pthread_cancel(thread);
		pthread_join(thread, NULL);
	}
}

static void discover_md_components(struct md_monitor *md)
{
	const char *mdname = udev_device_get_sysname(md->device);
	int ioctl_fd, i, offset = 0;
	mdu_disk_info_t info;
	struct device_monitor *tmp, *found = NULL;
	char mdpath[256];
	dev_t raid_devt, mon_devt, tmp_devt;
	struct udev *udev;
	struct udev_device *raid_dev;
	struct list_head update_list;

	if (!mdname)
		return;

	sprintf(mdpath, "/dev/%s", mdname);
	ioctl_fd = open(mdpath, O_RDWR|O_NONBLOCK);
	if (ioctl_fd < 0) {
		warn("%s: Couldn't open %s: %d",
		     mdname, mdpath, errno);
		return;
	}
	/* Temporarily move children devices onto a separate list */
	INIT_LIST_HEAD(&update_list);
	pthread_mutex_lock(&md->lock);
	list_splice_init(&md->children, &update_list);
	for (i = 0; i < 4096; i++) {
		info.number = i;
		if (ioctl(ioctl_fd, GET_DISK_INFO, &info) < 0) {
			warn("%s: ioctl GET_DISK_INFO for disk %d failed, "
			     "error %d", mdname, i, errno);
			continue;
		}
		if (info.major == 0 && info.minor == 0)
			continue;

		found = NULL;
		/*
		 * Magic:
		 * We have to figure out the minor number of the
		 * corresponding block device.
		 * For DASD it's major 94 with pitch 4
		 * For SCSI it's major 8,
		 * 65, 66, 67, 68, 69, 70, 71,
		 * 128, 129, 130, 131, 132, 133, 134, 135
		 * with pitch 16
		 */
		raid_devt = makedev(info.major, info.minor);
		if (info.major == 94) {
			offset = info.minor % 4;
		} else if (info.major == 8 ||
			   (info.major > 64 && info.major < 72) ||
			   (info.major > 127 && info.major < 136)) {
			offset = info.minor % 16;
		}
		mon_devt = makedev(info.major, info.minor - offset);
		/* Restart monitoring on existing devices */
		list_for_each_entry(tmp, &update_list, siblings) {
			tmp_devt = udev_device_get_devnum(tmp->device);
			if (tmp_devt == mon_devt) {
				found = tmp;
				break;
			}
		}
		if (found) {
			if (!found->parent)
				found->parent = md->device;

			info("%s: Restart monitoring %s", mdname,
			     found->md_name);
			list_move(&found->siblings, &md->children);
			monitor_dasd(found);
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
			continue;
		}
		found->md_index = i;
		found->md_slot = info.raid_disk;
		found->md_devt = raid_devt;
		udev = udev_device_get_udev(md->device);
		raid_dev = udev_device_new_from_devnum(udev, 'b', raid_devt);
		info("%s: Start monitoring %s", mdname,
		     udev_device_get_sysname(raid_dev));
		add_component(md, found, udev_device_get_sysname(raid_dev));
		udev_device_unref(raid_dev);
		list_add(&found->siblings, &md->children);
		monitor_dasd(found);
	}
	pthread_mutex_unlock(&md->lock);
	/* Cleanup stale devices */
	list_for_each_entry_safe(found, tmp, &update_list, siblings) {
		info("%s: Remove stale device",
		     found->dev_name);
		remove_md_component(md, found);
		remove_component(found);
	}
	close(ioctl_fd);
}

static void fail_md(struct md_monitor *md_dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	char cmdline[256];
	int rc;
	struct device_monitor *dev;

	if (!md_dev->pending_side) {
		warn("%s: no pending side", md_name);
		return;
	}
	if (md_dev->degraded & (1 << md_dev->pending_side)) {
		info("%s: mirror side %d already failed", md_name,
		     (md_dev->pending_side >> 1));
		return;
	}
	sprintf(cmdline, "mdadm --manage /dev/%s --fail set-%c", md_name,
		(md_dev->pending_side >> 1) ? 'B' : 'A' );
	dbg("%s: call 'system' '%s'", md_name, cmdline);
	rc = system(cmdline);
	if (rc) {
		warn("%s: cannot fail mirror, error %d", md_name, rc);
	} else {
		list_for_each_entry(dev, &md_dev->children, siblings) {
			int this_side = dev->md_slot % (md_dev->layout & 0xFF);
			if (this_side == (md_dev->pending_side >> 1)) {
				fail_component(md_dev, dev,
					       md_dev->pending_status);
			}
		}
		md_dev->degraded |= md_dev->pending_side;
		md_dev->pending_side = 0;
		md_dev->pending_status = UNKNOWN;
	}
}

static void reset_md(struct md_monitor *md_dev)
{
	const char *md_name = udev_device_get_sysname(md_dev->device);
	char cmdline[256];
	int rc;
	struct device_monitor *dev;

	list_for_each_entry(dev, &md_dev->children, siblings) {
		reset_component(dev);
	}

	if (!md_name)
		return;

	sprintf(cmdline, "mdadm --manage /dev/%s --re-add faulty", md_name);
	dbg("%s: call 'system' '%s'", md_name, cmdline);
	rc = system(cmdline);
	if (rc) {
		warn("%s: cannot reset mirror, error %d",
		     md_name, rc);
	} else {
		md_dev->degraded = 0;
		md_dev->pending_side = 0;
		md_dev->pending_status = UNKNOWN;
	}
}

static void remove_md(struct md_monitor *md_dev)
{
	struct device_monitor *dev, *tmp;
	struct list_head remove_list;

	INIT_LIST_HEAD(&remove_list);
	pthread_mutex_lock(&md_dev->lock);
	list_splice_init(&md_dev->children, &remove_list);
	pthread_mutex_unlock(&md_dev->lock);

	list_for_each_entry_safe(dev, tmp, &remove_list, siblings) {
		info("%s: Remove MD component device",
		     dev->dev_name);
		remove_md_component(md_dev, dev);
		remove_component(dev);
	}

	list_del_init(&md_dev->entry);
	info("Stop monitoring %s",
	     udev_device_get_devpath(md_dev->device));
	udev_device_unref(md_dev->device);
	free(md_dev);
}

static void monitor_md(struct udev_device *md_dev)
{
	struct md_monitor *found = NULL;
	char devpath[256];
	const char *devname;
	mdu_array_info_t info;
	int ioctl_fd;

	devname = udev_device_get_sysname(md_dev);
	if (!devname || !strlen(devname))
		return;

	memset(&info, 0, sizeof(mdu_array_info_t));
	sprintf(devpath, "/dev/%s", devname);
	ioctl_fd = open(devpath, O_RDWR|O_NONBLOCK);
	if (ioctl_fd >= 0) {
		if (ioctl(ioctl_fd, GET_ARRAY_INFO, &info) < 0) {
			err("%s: ioctl GET_ARRAY_INFO failed, error %d",
			    devname, errno);
			info.raid_disks = 0;
		} else if (info.raid_disks == 0) {
			warn("%s: no RAID disks, ignoring", devname);
		}
		close(ioctl_fd);
	} else {
		err("%s: could not open %s: error %d", devname,
		    devpath, errno);
		info.raid_disks = 0;
	}
	if (info.raid_disks < 1)
		return;

	if (info.level != 10) {
		err("%s: not a RAID10 array", devname);
		return;
	}

	found = lookup_md(md_dev);
	if (found) {
		warn("%s: Already monitoring %s", devname,
		       udev_device_get_devpath(found->device));
	} else {
		warn("%s: Start monitoring %s", devname,
		       udev_device_get_devpath(md_dev));
		found = malloc(sizeof(struct md_monitor));
		memset(found, 0, sizeof(struct md_monitor));
		found->device = md_dev;
		found->raid_disks = info.raid_disks;
		found->layout = info.layout;
		udev_device_ref(md_dev);
		INIT_LIST_HEAD(&found->children);
		INIT_LIST_HEAD(&found->pending);
		pthread_mutex_init(&found->lock, NULL);
		list_add(&found->entry, &md_list);
		discover_md_components(found);
	}
}

static void unmonitor_md(struct udev_device *md_dev)
{
	struct md_monitor *found_md = NULL;

	found_md = lookup_md(md_dev);
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
			monitor_md(md_dev);
			udev_device_unref(md_dev);
		}
	}
	udev_enumerate_unref(md_enumerate);
}

static int display_md_status(struct md_monitor *md_dev, char *buf)
{
	struct device_monitor *dev;
	int slot;
	int len = 0, i, max_index;
	char *status;

	status = malloc(md_dev->raid_disks + 1);
	if (!status)
		return -ENOMEM;
	memset(status, '.', md_dev->raid_disks);
	pthread_mutex_lock(&md_dev->lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		slot = dev->md_slot;
		if (slot < 0)
			continue;
		status[slot] = md_rdev_print_state_short(dev->md_status);
	}
	pthread_mutex_unlock(&md_dev->lock);
	if (md_dev->raid_disks > CLI_BUFLEN) {
		warn("%s: CLI buffer too small, min %d",
		     udev_device_get_sysname(md_dev->device),
		     md_dev->raid_disks);
		max_index = CLI_BUFLEN;
	} else {
		max_index = md_dev->raid_disks;
	}
	for (i = 0; i < max_index; i++ )
		len += sprintf(buf + len, "%c", status[i]);
	info("%s: md status %s", udev_device_get_sysname(md_dev->device), buf);
	free(status);
	return len;
}

static int display_io_status(struct md_monitor *md_dev, char *buf)
{
	struct device_monitor *dev;
	int slot;
	int len = 0, i, max_index;
	char *status;

	status = malloc(md_dev->raid_disks + 1);
	if (!status)
		return -ENOMEM;
	memset(status, '.', md_dev->raid_disks);
	pthread_mutex_lock(&md_dev->lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		slot = dev->md_slot;
		if (slot < 0)
			continue;
		status[slot] = dasd_io_print_state_short(dev->io_status);
	}
	pthread_mutex_unlock(&md_dev->lock);
	if (md_dev->raid_disks > CLI_BUFLEN) {
		warn("%s: CLI buffer too small, min %d",
		     udev_device_get_sysname(md_dev->device),
		     md_dev->raid_disks);
		max_index = CLI_BUFLEN;
	} else {
		max_index = md_dev->raid_disks;
	}
	for (i = 0; i < max_index; i++ )
		len += sprintf(buf + len, "%c", status[i]);
	info("%s: io status %s", udev_device_get_sysname(md_dev->device), buf);
	free(status);
	return len;
}

static int display_md(struct md_monitor *md_dev, char *buf)
{
	struct device_monitor *dev;
	char status[4096];
	int bufsize = 0, len;

	pthread_mutex_lock(&md_dev->lock);
	list_for_each_entry(dev, &md_dev->children, siblings) {
		len = sprintf(status, "%s: dev %s slot %d/%d status %s %s\n",
			       udev_device_get_sysname(md_dev->device),
			       dev->dev_name, dev->md_slot, md_dev->raid_disks,
			       md_rdev_print_state(dev->md_status),
			       dasd_io_print_state(dev->io_status));
		if ((bufsize + len) > CLI_BUFLEN) {
			warn("%s: CLI buffer too small, min %d",
			     udev_device_get_sysname(md_dev->device),
			     bufsize + len);
			memset(buf, 0, CLI_BUFLEN);
			len = -ENOMEM;
			break;
		}
		strcat(buf + bufsize, status);
		bufsize += len;
	}
	pthread_mutex_unlock(&md_dev->lock);
	return len;
}

static void fail_devices(struct udev_device *dev)
{
	const char *devpath;
	struct device_monitor *tmp, *found = NULL;

	devpath = udev_device_get_property_value(dev, "DEVPATH_OLD");
	if (!devpath)
		return;
	lock_device_list();
	list_for_each_entry(tmp, &device_list, entry) {
		const char *tmppath;

		tmppath = udev_device_get_devpath(tmp->device);
		if (tmppath && !strncmp(tmppath, devpath, strlen(devpath))) {
			found = tmp;
			break;
		}
	}
	unlock_device_list();
	if (found) {
		fail_mirror(found, REMOVED);
	}
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
	if (!strcmp(action, "change")) {
		if (!strncmp(devname, "md", 2)) {
			monitor_md(device);
		}
		if (!strncmp(devname, "dasd", 4)) {
			attach_dasd(device);
		}
	} else if (!strcmp(action, "remove")) {
		if (!strncmp(devname, "md", 2)) {
			unmonitor_md(device);
		}
		if (!strncmp(devname, "dasd" , 4)) {
			detach_dasd(device);
		}
	} else if (!strcmp(action, "move")) {
		/* Move event, generated by z/VM ATTACH/DETACH */
		if (strstr(udev_device_get_devpath(device),"defunct")) {
			/* Device detached, set it to faulty */
			warn("%s: device detached", devname);
			fail_devices(device);
		} else {
			warn("%s: device attached", devname);
			reset_devices(device);
		}
	}
}

static void print_device(struct udev_device *device)
{
	struct timeval tv;
	struct timezone tz;
	int prop = 0;

	gettimeofday(&tv, &tz);
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
	int rc;
	struct list_head active_list;
	struct md_monitor *md_dev, *tmp;
	const char *md_name = NULL;

	while (thr->running) {
		INIT_LIST_HEAD(&active_list);
		pthread_mutex_lock(&pending_lock);
		if (list_empty(&pending_list)) {
			info("md_exec: no requests, waiting %ld seconds",
			     failfast_timeout);
			if (gettimeofday(&start_time, NULL)) {
				err("md_exec: failed to get time, error %d",
				    errno);
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
			pthread_mutex_lock(&md_dev->lock);
			md_name = udev_device_get_sysname(md_dev->device);

			if (gettimeofday(&start_time, NULL) < 0)
				start_time.tv_sec = 0;

			list_del_init(&md_dev->pending);
			if (md_dev->pending_status == UNKNOWN)
				dbg("%s: task already completed", md_name);
			else if (md_dev->pending_status != IN_SYNC)
				fail_md(md_dev);
			else
				reset_md(md_dev);

			if (start_time.tv_sec &&
			    gettimeofday(&end_time, NULL) == 0) {
				timersub(&end_time, &start_time, &diff);
				info("%s: all devices failed after "
				     "%lu.%06lu secs", md_name,
				     diff.tv_sec, diff.tv_usec);
			} else {
				info("%s: all devices failed", md_name);
			}
			pthread_mutex_unlock(&md_dev->lock);
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
		err("Failed to start mdadm exec thread: %d", errno);
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

	cli->running = 1;
	pthread_cleanup_push(cli_monitor_cleanup, cli);

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
		fdcount = select(cli->sock + 1, &readfds, NULL, NULL, NULL);
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
				err("error receiving cli message, errno %d",
				    errno);
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
		if (!md_dev) {
			info("%s: skipping event, array not monitored", mdstr);
			buf[0] = ENODEV;
			iov.iov_len = 1;
			goto send_msg;
		}
		if (!strcmp(event, "RebuildStarted")) {
			info("%s: Rebuild started",
			     udev_device_get_sysname(md_dev->device));
			md_dev->in_recovery = 1;
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "RebuildFinished")) {
			info("%s: Rebuild finished",
			     udev_device_get_sysname(md_dev->device));
			md_dev->in_recovery = 0;
			buf[0] = 0;
			iov.iov_len = 0;
			goto send_msg;
		}
		if (!strcmp(event, "DeviceDisappeared")) {
			/* Event handled via uevents */
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
			info("%s: display mirror status for %d devices",
			     mdstr, md_dev->raid_disks);
			buflen = display_md_status(md_dev, buf);
			if (buflen < 0) {
				iov.iov_len = 1;
				buf[0] = -buflen;
			} else {
				iov.iov_len = buflen;
			}
			goto send_msg;
		}
		if (!strcmp(event, "MonitorStatus")) {
			info("%s: display monitor status for %d devices",
			     mdstr, md_dev->raid_disks);
			buflen = display_io_status(md_dev, buf);
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
		if (dev) {
			if (!strcmp(event, "FailSpare") ||
			    !strcmp(event, "Fail")) {
				fail_md_component(md_dev, dev);
				buf[0] = 0;
				iov.iov_len = 0;
			} else if (!strcmp(event, "Remove")) {
				remove_md_component(md_dev, dev);
				pthread_mutex_lock(&md_dev->lock);
				remove_component(dev);
				pthread_mutex_unlock(&md_dev->lock);
				buf[0] = 0;
				iov.iov_len = 0;
			} else if (!strcmp(event, "SpareActive")) {
				sync_md_component(md_dev, dev);
				buf[0] = 0;
				iov.iov_len = 0;
			} else {
				info("%s: Unhandled event '%s'", mdstr, event);
				buf[0] = EINVAL;
				iov.iov_len = 1;
			}
		} else {
			if (!strcmp(event, "SpareActive")) {
				discover_md_components(md_dev);
				buf[0] = 0;
				iov.iov_len = 0;
			} else {
				info("%s: Unhandled event '%s'", mdstr, event);
				buf[0] = EINVAL;
				iov.iov_len = 1;
			}
		}
	send_msg:
		if (sendmsg(cli->sock, &smsg, 0) < 0)
			err("sendmsg failed, error %d", errno);
	}
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
		err("cannot open cli socket, error %d", errno);
		free(cli);
		return NULL;
	}
	if (bind(cli->sock, (struct sockaddr *) &sun, addrlen) < 0) {
		err("cannot bind cli socket, error %d", errno);
		close(cli->sock);
		free(cli);
		return NULL;
	}
	setsockopt(cli->sock, SOL_SOCKET, SO_PASSCRED,
		   &feature_on, sizeof(feature_on));

	rc = pthread_create(&cli->thread, &cli_attr, cli_monitor_thread, cli);
	if (rc) {
		cli->thread = 0;
		close(cli->sock);
		err("Failed to start cli monitor: %d", errno);
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
	int buflen = 0, i;
	char status;

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
		err("bind to local cli address failed, error %d", errno);
		return 4;
	}
	setsockopt(cli_sock, SOL_SOCKET, SO_PASSCRED,
		   &feature_on, sizeof(feature_on));
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
			err("sendmsg failed, error %d", errno);
		}
		return 5;
	}

	for (i = 0; i < POLL_TIMEOUT; i++) {
		memset(buf, 0x00, sizeof(buf));
		iov.iov_base = buf;
		iov.iov_len = CLI_BUFLEN;
		buflen = recvmsg(cli_sock, &smsg, MSG_DONTWAIT);
		if (buflen >= 0 || errno != EAGAIN)
			break;
		dbg("No data received, retrying");
		sleep(1);
	}
	if (buflen < 0) {
		if (errno == EAGAIN) {
			err("recvmsg failed, md_monitor does not respond");
		} else {
			err("recvmsg failed, error %d", errno);
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
		fprintf(stderr, "can't initialize thread attr: %s\n",
			strerror(errno));
		exit(1);
	}
	if (stacksize < PTHREAD_STACK_MIN)
		stacksize = PTHREAD_STACK_MIN;

	if (pthread_attr_setstacksize(attr, stacksize)) {
		fprintf(stderr, "can't set thread stack size to %lu: %s\n",
			(unsigned long)stacksize, strerror(errno));
		exit(1);
	}
	if (detached &&
	    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED)) {
		fprintf(stderr, "can't set thread to detached: %s\n",
			strerror(errno));
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
		err("cannot get file limit");
		exit (1);
	}
	if ((pid = fork()) < 0) {
		err("fork failed with %d", errno);
		exit(errno);
	} else if (pid != 0)
		exit(0);

	setsid();

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	if (sigaction(SIGHUP, &act, NULL) < 0) {
		err("sigaction(SIGHUP) failed with %d", errno);
		exit(errno);
	}

	if ((pid = fork()) < 0) {
		err("fork failed with %d", errno);
		exit(errno);
	} else if (pid != 0) {
		fprintf(stdout, "%d\n", pid);
		exit(0);
	}

	if (chdir("/") < 0) {
		err("Cannot chdir to '/': %d", errno);
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
	    "[--adjust-timeout|-a] [--expires=<num>|-e <num>] [--retries=<num>|-r <num>]"
	    "[--log-priority=<prio>|-p <prio>] [--syslog|-s] [--verbose|-v]  "
	    "[--command=<cmd>|-c <cmd>] [--fail-mirror|-m] [--fail-disk|-o] "
	    "[--check-in-sync|y] [--check-timeout=<secs>|-t <secs>] [--help|-h]\n"
	    "  --adjust-timeout               track I/O latency ot adjust timeout\n"
	    "  --daemonize                    start monitor in background\n"
	    "  --expires=<num>                set failfast_expires to <num>\n"
	    "  --logfile=<file>               use <file> for logging\n"
	    "  --log-priority=<prio>          set logging priority to <prio>\n"
	    "  --retries=<num>                set failfast_retries to <num>\n"
	    "  --command=<cmd>                send command <cmd> to daemon\n"
	    "  --fail-mirror                  fail entire mirror side\n"
	    "  --fail-disk                    fail affected disk only\n"
	    "  --syslog                       use syslog for logging\n"
	    "  --check-timeout=<secs>         run path checker every <secs> seconds\n"
	    "  --check-in-sync                run path checker for in_sync devices\n"
	    "  --verbose                      increase logging priority\n"
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
	unsigned long max_proc;
	struct rlimit cur;
	char *command_to_send = NULL;
	char *logfile = NULL;
	fd_set readfds;
	int monitor_fd;
	int rc = 0;

	static const struct option options[] = {
		{ "adjust-timeout", required_argument, NULL, 'a' },
		{ "command", required_argument, NULL, 'c' },
		{ "daemonize", no_argument, NULL, 'd' },
		{ "expires", required_argument, NULL, 'e' },
		{ "logfile", required_argument, NULL, 'f' },
		{ "process-limit", required_argument, NULL, 'l' },
		{ "fail-mirror", no_argument, NULL, 'm' },
		{ "fail-disk", no_argument, NULL, 'o' },
		{ "retries", required_argument, NULL, 'r' },
		{ "log-priority", required_argument, NULL, 'p' },
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

	setup_thread_attr(&monitor_attr, 64 * 1024, 1);
	setup_thread_attr(&cli_attr, 64 * 1024, 0);

	while (1) {
		option = getopt_long(argc, argv, "ac:de:f:l:mp:r:st:vyhV",
				     options, NULL);
		if (option == -1) {
			break;
		}

		switch (option) {
		case 'a':
			adjust_timeout = 1;
			break;
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
		case 'l':
			max_proc = strtoul(optarg, NULL, 10);
			if (max_proc < 1) {
				err("Invalid limit '%s' for max_processes",
				    optarg);
				exit(1);
			}
			if (getrlimit(RLIMIT_NPROC, &cur) < 0) {
				err("Cannot get current process limit, "
				    "error %d", errno);
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
				err("Cannot modify process limit, "
				    "error %d", errno);
			}
			break;
		case 'm':
			fail_mirror_side = 1;
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
			if (checker_timeout < 1) {
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
			fprintf(stderr, "Cannot open logfile '%s', error %d\n",
				logfile, errno);
			exit (1);
		}
		close(2);
		fd2 = dup(1);
		if (fd2 < 0) {
			err("Cannot reassign stderr");
			exit (1);
		}
	}

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
		err("cannot set sigmask: %d", errno);
		goto out;
	}

	info("startup");
	cli = monitor_cli();
	if (!cli)
		goto out;
	mdx = start_mdadm_exec();
	if (!mdx)
		goto out;

	/* Discover existing DASDs */
	discover_dasd(udev);

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
		struct timeval tmo;

		FD_ZERO(&readfds);
		FD_SET(monitor_fd, &readfds);
		tmo.tv_sec = 1;
		tmo.tv_usec = 0;
		fdcount = select(monitor_fd + 1, &readfds,
				 NULL, NULL, &tmo);
		if (fdcount < 0) {
			if (errno != EINTR)
				warn("error receiving uevent message: %m");
			continue;
		}
		/* Timeout, just continue */
		if (fdcount == 0)
			continue;

		if (FD_ISSET(monitor_fd, &readfds)) {
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
	list_for_each_entry_safe(found_md, tmp_md, &md_list, entry) {
		remove_md(found_md);
	}

	pthread_mutex_lock(&device_lock);
	list_for_each_entry_safe(found_dev, tmp_dev, &device_list, entry) {
		info("%s: detached '%s'", found_dev->dev_name,
		     udev_device_get_devpath(found_dev->device));
		list_del_init(&found_dev->entry);
		udev_device_unref(found_dev->device);
		free(found_dev);
	}
	pthread_mutex_unlock(&device_lock);

	if (cli && cli->thread) {
		pthread_cancel(cli->thread);
		pthread_join(cli->thread, NULL);
	}
	free(cli);

	if (mdx && mdx->thread) {
		pthread_cancel(mdx->thread);
		pthread_join(mdx->thread, NULL);
	}
	free(mdx);

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
