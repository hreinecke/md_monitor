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

#include "list.h"
#include "md_debug.h"
#include "md_monitor.h"

#define DEFAULT_SOCKET "/org/kernel/linux/storage/multipathd"

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
	while (ptr && ptr < reply + len && *ptr == ' ') ptr++;
	if (!strcmp(ptr, "off"))
		io_status = IO_FAILED;
	else if (!*ptr == '-')
		io_status = IO_PENDING;

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
		if (len && reply[len - 2] == '\n') {
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
