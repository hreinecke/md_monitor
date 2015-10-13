#!/bin/bash
#
# Setup root: set up system with root on MD
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="root_on_md"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: setting up root on MD"

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi
sleep 1
echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Set up boot DASD ..."
# set up boot DASD - using DASD 0xA0C8
D=($(setup_one_dasd 0xa0c8))
devno=${D[0]}
dasd=${D[1]}
# re-partition the DASD
if [ ! -d /sys/block/${dasd}/${dasd}1 ] || [ -d /sys/block/dasd/${dasd}/${dasd}2 ] ; then
    if ! echo -e '[2,15779]\n[15780,last]' | fdasd -c /dev/stdin /dev/$dasd ; then
	error_exit "Failed to partition $dasd"
    fi
fi
SWAP_DEVICE=("/dev/${dasd}1")
BOOT_DEVICE=("/dev/${dasd}2")

echo "Set up logging DASD ..."
# set up logging DASD - using DASD 0xA1C8
D=($(setup_one_dasd 0xa1c8))
devno=${D[0]}
dasd=${D[1]}
LOG_DEVICE=("/dev/${dasd}1")

echo "Create swap (${SWAP_DEVICE}) and filesystem on boot device (${BOOT_DEVICE})..."
mkswap "${SWAP_DEVICE}"
if ! mkfs.ext3 "${BOOT_DEVICE}" ; then
    error_exit "Cannot create fs"
fi

echo "Create filesystem on logging device (${LOG_DEVICE})..."
if ! mkfs.ext3 "${LOG_DEVICE}" ; then
    error_exit "Cannot create fs"
fi

echo "Mount boot device filesystem ..."
mkdir /mnt/boot
if ! mount "${BOOT_DEVICE}" /mnt/boot ; then
    error_exit "Cannot mount MD array."
fi
push_recovery_fn "umount /mnt/boot"

echo "Mount logging device filesystem ..."
mkdir -p /mnt/var/log
if ! mount "${LOG_DEVICE}" /mnt//var/log ; then
    error_exit "Cannot mount MD array."
fi
push_recovery_fn "umount /mnt/var/log"

echo "Copy files ..."
# excluding old logs
find / -mount -a ! \( -wholename /var/log/\* -type f \) | cpio --pass-through --make-directories --preserve-modification-time /mnt

echo "Install zipl ..."
# change kernel boot parameters
KERN_PARAMS="$(cat /proc/cmdline | sed 's,ccw-0\.0\.0150-part,ccw-0.0.a0c8-part,g')"
zipl --noninteractive --verbose --target=/mnt/boot/zipl --image=/mnt/boot/image-$(uname -r) \
	--ramdisk=/mnt/boot/initrd-$(uname -r) --parameters="$KERN_PARAMS"

# change device paths
sed -i 's,ccw-0\.0\.0150-part1,ccw-0.0.a0c8-part1,g;s,disk/by-path/ccw-0.0.0150-part2,md1,g' /mnt/etc/fstab /mnt/etc/zipl.conf
echo "/dev/disk/by-path/ccw-0.0.a0c8-part2 /boot ext3 acl,user_xattr 1 1" >> /mnt/etc/fstab
echo "/dev/disk/by-path/ccw-0.0.a1c8-part1 /var/log ext3 acl,user_xattr 1 1" >> /mnt/etc/fstab

# run md_monitor on the system from boot
echo "/sbin/md_monitor -y -p 7 -d -s" >> /mnt/etc/init.d/boot.local

echo "Re-generate inirtd ..."
mount --bind /dev /mnt/dev
push_recovery_fn "umount /mnt/dev"
chroot /mnt mount /proc
push_recovery_fn "umount /mnt/proc"
chroot /mnt mount /sys
push_recovery_fn "umount /mnt/sys"
chroot /mnt mkinitrd

# see you after reboot
umount /mnt/proc
umount /mnt/sys
umount /mnt/dev

#############################

echo "Umount filesystem ..."
umount /mnt/boot
umount /mnt/var/log
umount /mnt

stop_md ${MD_NUM}
