#!/bin/bash
#
# Testcase 5: Successive Disk attach/detach
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase5"
DEVNOS_LEFT="0.0.0200 0.0.0201 0.0.0202 0.0.0203"
DEVNOS_RIGHT="0.0.0210 0.0.0211 0.0.0212 0.0.0213"
SLEEPTIME=30

logger "Monitor Testcase 5: Successive Disk detach/attach"

stop_md $MD_NUM

activate_dasds

clear_metadata

modprobe vmcp

ulimit -c unlimited
start_md ${MD_NUM} ${MD_NAME}

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi
sleep 1

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

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
    vmcp attach ${devno##*.} \*
    cat /proc/mdstat
done
wait_for_sync ${MD_NUM}
for dev in ${DEVNOS_RIGHT} ; do
    echo "Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME

    echo "Detach right device $devno ..."
    vmcp det ${devno##*.}
    echo "Waiting for 15 seconds ..."
    sleep 15
    cat /proc/mdstat
    echo "Attach right device $devno ..."
    vmcp attach ${devno##*.} \*
    cat /proc/mdstat
done

killall -KILL dt

wait_for_sync ${MD_NUM}

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
