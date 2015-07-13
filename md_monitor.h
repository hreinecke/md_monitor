/*
 * md_monitor.h
 *
 * Copyright (C) 2015 Hannes Reinecke <hare@suse.de>
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

#ifndef _MD_MONITOR_H
#define _MD_MONITOR_H

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
	STOPPED,	/* md should be stopped */
	RESERVED,	/* end marker */
};

enum device_io_status {
	IO_UNKNOWN,
	IO_ERROR,
	IO_OK,
	IO_FAILED,
	IO_PENDING,
	IO_TIMEOUT,
	IO_RETRY,
	IO_RESERVED
};

struct mdadm_exec {
	int running;
	pthread_t thread;
};

#define CLI_BUFLEN 4096
#define MD_NAMELEN 256

struct cli_monitor {
	int running;
	int sock;
	pthread_t thread;
};

struct md_monitor {
	char dev_name[MD_NAMELEN];
	struct list_head entry;
	struct list_head children;
	pthread_mutex_t device_lock;
	struct udev_device *device;
	pthread_mutex_t status_lock;
	struct list_head pending;
	enum md_rdev_status pending_status;
	int pending_side;
	int raid_disks;
	int layout;
	int in_recovery;
	int degraded;
	int in_discovery;
};

struct device_monitor {
	struct list_head entry;
	struct list_head siblings;
	struct udev_device *device;
	struct udev_device *parent;
	char dev_name[MD_NAMELEN];
	char md_name[MD_NAMELEN];
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t io_cond;
	enum md_rdev_status md_status;
	enum device_io_status io_status;
	int ref;
	int md_index;
	int md_slot;
	int fd;
	int running;
	int aio_active;
	struct timeval aio_start_time;
	struct iocb io;
	io_context_t ioctx;
	int blksize;
	unsigned char *buf;
};

extern sigset_t thread_sigmask;
extern void sig_handler(int signum);
extern struct device_monitor *lookup_device_mdname(const char *devname);
extern enum md_rdev_status md_rdev_check_state(struct device_monitor *dev);
extern enum md_rdev_status md_rdev_update_state(struct device_monitor *dev,
						enum md_rdev_status md_status);
extern enum md_rdev_status
device_monitor_update(struct device_monitor *dev,
		      enum device_io_status io_status,
		      enum md_rdev_status new_status);
extern char *md_rdev_print_state(enum md_rdev_status state);
extern char *device_io_print_state(enum device_io_status state);

#endif /* _MD_MONITOR_H */
