/*
 * mpath_util.c
 *
 * Copied from uxsock.c
 *
 * Original author : tridge@samba.org, January 2002
 *
 * Copyright (c) 2005 Christophe Varoqui
 * Copyright (c) 2005 Alasdair Kergon, Redhat
 * Copyright (c) 2015 Hannes Reinecke, SUSE Linux GmbH
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <syslog.h>
#include <signal.h>
#include <errno.h>
#include <libaio.h>
#include <limits.h>
#include <pthread.h>

#include "list.h"
#include "md_debug.h"
#include "md_monitor.h"

#define DEFAULT_SOCKET "/org/kernel/linux/storage/multipathd"
pthread_t mpath_thread;
static unsigned long mpath_timeout;

/*
 * connect to a unix domain socket
 */
int socket_connect(const char *name)
{
	int fd, len;
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	addr.sun_path[0] = '\0';
	len = strlen(name) + 1 + sizeof(sa_family_t);
	strncpy(&addr.sun_path[1], name, len);

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd == -1) {
		warn("Couldn't create ux_socket, error %d", errno);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, len) == -1) {
		warn("Couldn't connect to ux_socket, error %d", errno);
		close(fd);
		return -1;
	}

	return fd;
}

/*
 * keep writing until it's all sent
 */
size_t write_all(int fd, const void *buf, size_t len)
{
	size_t total = 0;

	while (len) {
		ssize_t n = write(fd, buf, len);
		if (n < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			return total;
		}
		if (!n)
			return total;
		buf = n + (char *)buf;
		len -= n;
		total += n;
	}
	return total;
}

/*
 * keep reading until its all read
 */
ssize_t read_all(int fd, void *buf, size_t len, unsigned int timeout)
{
	size_t total = 0;
	ssize_t n;
	int ret;
	struct pollfd pfd;

	while (len) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, timeout);
		if (!ret) {
			return -ETIMEDOUT;
		} else if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		} else if (!pfd.revents & POLLIN)
			continue;
		n = read(fd, buf, len);
		if (n < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			return -errno;
		}
		if (!n)
			return total;
		buf = n + (char *)buf;
		len -= n;
		total += n;
	}
	return total;
}

/*
 * send a packet in length prefix format
 */
int send_packet(int fd, const char *buf, size_t len)
{
	int ret = 0;
	sigset_t set, old;

	/* Block SIGPIPE */
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, &old);

	if (write_all(fd, &len, sizeof(len)) != sizeof(len))
		ret = -1;
	if (!ret && write_all(fd, buf, len) != len)
		ret = -1;

	/* And unblock it again */
	pthread_sigmask(SIG_SETMASK, &old, NULL);

	return ret;
}

/*
 * receive a packet in length prefix format
 */
int recv_packet(int fd, char **buf, size_t *len, unsigned int timeout)
{
	ssize_t ret;

	ret = read_all(fd, len, sizeof(*len), timeout);
	if (ret < 0) {
		(*buf) = NULL;
		*len = 0;
		return ret;
	}
	if (ret < sizeof(*len)) {
		(*buf) = NULL;
		*len = 0;
		return -EIO;
	}
	if (len == 0) {
		(*buf) = NULL;
		return 0;
	}
	(*buf) = malloc(*len);
	if (!*buf)
		return -ENOMEM;
	ret = read_all(fd, *buf, *len, timeout);
	if (ret != *len) {
		free(*buf);
		(*buf) = NULL;
		*len = 0;
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

enum device_io_status mpath_check_status(struct device_monitor *dev,
					 int timeout)
{
	int fd;
	char inbuf[64];
	char *reply, *ptr, *eptr;
	size_t len;
	unsigned long num_paths;
	int ret;
	enum device_io_status io_status = IO_UNKNOWN;

	fd = socket_connect(DEFAULT_SOCKET);
	if (fd < 0) {
		warn("%s: failed to connect to multipathd: %m", dev->dev_name);
		return io_status;
	}
	sprintf(inbuf, "show map %s format \"%%N %%Q\"", dev->md_name);
	ret = send_packet(fd, inbuf, strlen(inbuf) + 1);
	if (ret != 0) {
		warn("%s: cannot send command to multipathd: %s",
		     dev->dev_name, strerror(-ret));
		close(fd);
		return IO_ERROR;
	}
	ret = recv_packet(fd, &reply, &len, timeout);
	close(fd);
	if (ret < 0) {
		warn("%s: error receiving packet from multipathd: %s",
		     dev->dev_name, strerror(-ret));
		if (ret == -ETIMEDOUT)
			io_status = IO_TIMEOUT;
		else
			io_status = IO_ERROR;
		goto out;
	}
	ptr = reply;
	num_paths = strtoul(ptr, &eptr, 10);
	if (ptr == eptr || num_paths == ULONG_MAX) {
		warn("%s: parse error in multipathd reply '%s'",
		     dev->dev_name, reply);
		io_status = IO_ERROR;
		goto out;
	}
	if (num_paths > 0) {
		io_status = IO_OK;
		goto out;
	}
	if (!eptr) {
		io_status = IO_ERROR;
		goto out;
	}
	ptr = eptr;
	while (ptr && ptr < reply + len && *ptr == ' ') ptr++;
	if (!strncmp(ptr, "off", 3))
		io_status = IO_FAILED;
	else if (!*ptr == '-')
		io_status = IO_PENDING;
	else
		io_status = IO_RETRY;
out:
	free(reply);
	return io_status;
}

int mpath_modify_queueing(struct device_monitor *dev, int enable, int timeout)
{
	int fd;
	char inbuf[64];
	char *reply;
	size_t len;
	int ret;

	fd = socket_connect(DEFAULT_SOCKET);
	if (fd < 0) {
		warn("%s: failed to connect to multipathd: %m", dev->dev_name);
		return -errno;
	}
	if (enable)
		sprintf(inbuf, "restorequeueing map %s", dev->md_name);
	else
		sprintf(inbuf, "disablequeueing map %s", dev->md_name);

	dbg("%s: %s multipath queueing", dev->dev_name,
	    enable ? "restore" : "disable");

	ret = send_packet(fd, inbuf, strlen(inbuf) + 1);
	if (ret != 0) {
		warn("%s: cannot send command to multipathd: %s",
		     dev->dev_name, strerror(-ret));
		close(fd);
		return IO_ERROR;
	}
	ret = recv_packet(fd, &reply, &len, timeout);
	close(fd);
	if (ret < 0) {
		warn("%s: error receiving packet from multipathd: %s",
		     dev->dev_name, strerror(-ret));
	} else {
		/* multipathd calculates the length including the NULL byte */
		if (len > 1 && reply[len - 2] == '\n') {
			len--;
			reply[len - 1] = 0;
		}
		dbg("%s: received reply '%s'", dev->dev_name, reply);
		if (!strncmp(reply, "ok", 2))
			ret = 0;
		else if (!strncmp(reply, "timeout", 7))
			ret = -ETIMEDOUT;
		else
			ret = -EIO;
	}

	if (ret)
		warn("%s: cannot modify multipath queueing", dev->dev_name);

	free(reply);
	return ret;
}

ssize_t mpath_status(char **reply, int timeout)
{
	int fd;
	char inbuf[64];
	size_t len;
	int ret;

	fd = socket_connect(DEFAULT_SOCKET);
	if (fd < 0) {
		warn("mpath: failed to connect to multipathd: %m");
		return -errno;
	}
	sprintf(inbuf, "show maps format \"%%d %%N %%Q\"");
	ret = send_packet(fd, inbuf, strlen(inbuf) + 1);
	if (ret != 0) {
		warn("mpath: cannot send command to multipathd: %s",
		     strerror(-ret));
		close(fd);
		return ret;
	}
	ret = recv_packet(fd, reply, &len, timeout);
	close(fd);
	if (ret < 0) {
		warn("mpath: error receiving packet from multipathd: %s",
		     strerror(-ret));
		if (ret == -ETIMEDOUT)
			ret = 0;
		return ret;
	}
	return len;
}

void *mpath_status_thread (void *ctx)
{
	struct device_monitor *dev;
	enum device_io_status io_status;
	enum md_rdev_status md_status, new_status;
	struct timespec tmo;
	ssize_t len;
	char *reply = NULL, *ptr, *eptr;
	char *devname = NULL;
	unsigned long num_paths;
	int rc;

	while (1) {
		len = mpath_status(&reply, mpath_timeout);
		if (len < 0) {
			warn("error while reading multipath status, exit");
			break;
		}
		if (len == 0) {
			warn("timeout while reading multipath status, retry");
			free(reply);
			continue;
		}
		ptr = reply;
		ptr = strchr(reply, '\n');
		if (!ptr) {
			warn("Parse error in multipath header '%s'", reply);
			break;
		}
		ptr++;
		while (ptr && ptr < reply + len && strlen(ptr)) {
			devname = ptr;
			ptr = strchr(devname, ' ');
			if (!ptr) {
				warn("Parse error in multipath response '%s'",
				     devname);
				break;
			}
			*ptr = '\0';
			ptr++;
			while (*ptr == ' ' && ptr < reply + len)
				ptr++;
			if (ptr == reply + len) {
				ptr = '\0';
				len --;
			}
			num_paths = strtoul(ptr, &eptr, 10);
			if (ptr == eptr || num_paths == ULONG_MAX) {
				warn("%s: invalid number of paths, reply '%s'",
				     devname, ptr);
				ptr = strchr(ptr, '\n');
				if (ptr)
					ptr++;
				continue;
			}
			if (num_paths > 0) {
				io_status = IO_OK;
			} else if (!eptr) {
				io_status = IO_ERROR;
			} else {
				ptr = eptr;
				while (ptr && ptr < reply + len &&
				       *ptr == ' ') ptr++;
				if (!strncmp(ptr, "off", 3))
					io_status = IO_FAILED;
				else if (!*ptr == '-')
					io_status = IO_PENDING;
				else
					io_status = IO_RETRY;
			}
			ptr = strchr(ptr, '\n');
			if (ptr)
				ptr++;
#if 0
			info("mpath: parser dev %s paths %d status %s next %s",
			     devname, num_paths,
			     device_io_print_state(io_status), ptr);
#endif
			dev = lookup_device_devname(devname);
			if (!dev) {
				continue;
			}
			warn("%s: update status", devname);
			md_status = md_rdev_check_state(dev);
			if (md_status == UNKNOWN) {
				/* array has been stopped */
				continue;
			}
			/* Write status back */
			pthread_mutex_lock(&dev->lock);
			new_status = md_rdev_update_state(dev, md_status);
			dev->io_status = io_status;
			pthread_cond_signal(&dev->io_cond);
			pthread_mutex_unlock(&dev->lock);
			new_status = device_monitor_update(dev, io_status,
							   new_status);
			info("%s: state %s / %s",
			     dev->dev_name, md_rdev_print_state(new_status),
			     device_io_print_state(io_status));
		}
		free(reply);
		tmo.tv_sec = mpath_timeout;
		tmo.tv_nsec = 0;
		info("mpath: waiting %ld seconds ...", (long)tmo.tv_sec);
		rc = sigtimedwait(&thread_sigmask, NULL, &tmo);
		if (rc < 0) {
			if (errno == EINTR) {
				info("mpath: ignore signal interrupt");
			} else if (errno != EAGAIN) {
				info("mpath: wait failed: %s", strerror(errno));
				break;
			}
		} else {
			info("mpath: wait interrupted");
		}
	}
	return ((void *)0);
}

int start_mpath_check(unsigned long timeout)
{
	int rc;

	info("Start mpath status thread");
	mpath_timeout = timeout;

	rc = pthread_create(&mpath_thread, NULL, mpath_status_thread, NULL);
	if (rc) {
		err("Failed to start mpath update thread: %m");
		mpath_thread = 0;
	}
	return rc;
}

void stop_mpath_check(void)
{
	if (mpath_thread) {
		info("Stop mpath status thread");
		pthread_cancel(mpath_thread);
		pthread_join(mpath_thread, NULL);
	}
}
