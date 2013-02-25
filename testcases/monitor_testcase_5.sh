#!/bin/bash
#
# Testcase 5: Successive Disk attach/detach
#
# This testcase does not work.
# I have to check with Neil Brown if it even
# is a valid testcase.

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase5"
DEVNOS_LEFT="0.0.0210 0.0.0211 0.0.0212 0.0.0213"
DEVNOS_RIGHT="0.0.0220 0.0.0221 0.0.0222 0.0.0223"
SLEEPTIME=30

logger "Monitor Testcase 5: Successive Disk detach/attach"

stop_md $MD_NUM

activate_dasds

clear_metadata

modprobe vmcp

ulimit -c unlimited
start_md ${MD_NUM}

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi
sleep 1

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

wait_for_sync ${MD_NUM}

run_dt /mnt;

for devno in ${DEVNOS_LEFT} ; do
    echo "Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME

    echo "Detach left device $devno ..."
    vmcp det ${devno##*.}
    echo "Waiting for 15 seconds ..."
    sleep 15
    cat /proc/mdstat
    echo "Attach left device $devno ..."
    vmcp link \* ${devno##*.} ${devno##*.}
    cat /proc/mdstat
done
if wait_for_sync ${MD_NUM} ; then
    for dev in ${DEVNOS_RIGHT} ; do
	echo "Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME

	echo "Detach right device $devno ..."
	vmcp det ${devno##*.}
	echo "Waiting for 15 seconds ..."
	sleep 15
	cat /proc/mdstat
	echo "Attach right device $devno ..."
	vmcp link \* ${devno##*.} ${devno##*.}
	cat /proc/mdstat
    done
fi

killall -KILL dt

wait_for_sync ${MD_NUM}

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
