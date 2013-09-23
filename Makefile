#
# Makefile for compiling md_monitor
#

MANDIR = /usr/share/man
MAN1DIR = $(MANDIR)/man1
MAN8DIR = $(MANDIR)/man8

CFLAGS = -g -Wall $(OPTFLAGS)

all: md_monitor

clean:
	rm -f *.o
	rm -f md_monitor

install: all
	[ -d $(DESTDIR)/sbin ] || mkdir $(DESTDIR)/sbin
	install md_monitor $(DESTDIR)/sbin/md_monitor
	[ -d $(DESTDIR)/usr/share/misc ] || mkdir -p $(DESTDIR)/usr/share/misc
	install -D md_notify_device.sh $(DESTDIR)/usr/share/misc/md_notify_device.sh
	[ -d $(DESTDIR)$(MAN8DIR) ] || mkdir -p $(DESTDIR)$(MAN8DIR)
	install -D -m 644 md_monitor.man $(DESTDIR)$(MAN8DIR)/md_monitor.8

md_monitor: md_monitor.o dasd_ioctl.o
	$(CC) $(CFLAGS) -o $@ $^ -ludev -lpthread -laio

md_monitor.o: md_monitor.c
	$(CC) $(CFLAGS) -c -o $@ $^

dasd_ioctl.o: dasd_ioctl.c
	$(CC) $(CFLAGS) -c -o $@ $^

md_monitor.c: md_debug.h dasd_ioctl.h list.h

dasd_ioctl.c: md_debug.h dasd_ioctl.h
