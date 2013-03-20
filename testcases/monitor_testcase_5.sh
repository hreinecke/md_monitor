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
MONITOR_TIMEOUT=60
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

echo "$(date) Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "$(date) Run dt"
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

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    [ $sleeptime -ge $MONITOR_TIMEOUT ] && break
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]\]\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    echo "$(date) ERROR: $num devices are still faulty"
fi

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    echo "$(date) mirror not synchronized"
    cat /proc/mdstat
else
    echo "$(date) mirror synchronized"
    cat /proc/mdstat
    for devno in ${DEVNOS_RIGHT} ; do
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

    echo "$(date) Ok. Waiting for MD to pick up changes ..."
    # Wait for md_monitor to pick up changes
    sleeptime=0
    num=${#DASDS_RIGHT[@]}
    while [ $num -gt 0  ] ; do
	[ $sleeptime -ge $MONITOR_TIMEOUT ] && break
	for d in ${DASDS_RIGHT[@]} ; do
	    device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]\]\).*/\1/p" /proc/mdstat)
	    if [ "$device" ] ; then
		(( num -- ))
	    fi
	done
	[ $num -eq 0 ] && break
	num=${#DASDS_RIGHT[@]}
	sleep 1
	(( sleeptime ++ ))
    done
    if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
	echo "$(date) MD monitor picked up changes after $sleeptime seconds"
    else
	echo "$(date) ERROR: $num devices are still faulty"
    fi
    echo "$(date) Wait for sync"
    wait_for_sync ${MD_NUM}
    echo "$(date) sync finished"
    cat /proc/mdstat
fi

killall -KILL dt

wait_for_sync ${MD_NUM}

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
