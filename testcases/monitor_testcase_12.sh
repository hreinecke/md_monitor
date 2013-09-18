#!/bin/bash
#
# Testcase 12: Successive Disk attach/detach
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase12"
MONITOR_TIMEOUT=60
SLEEPTIME=30

logger "Monitor Testcase 12: Successive Disk detach/attach"

stop_md $MD_NUM

activate_dasds

clear_metadata

modprobe vmcp

ulimit -c unlimited
start_md ${MD_NUM}

echo "$(date) Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "$(date) Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "$(date) Run I/O test"
run_iotest /mnt;

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME

    echo "$(date) Detach left device $devno ..."
    vmcp det ${devno##*.}
    echo "$(date) Waiting for 15 seconds ..."
    sleep 15
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    echo "$(date) Attach left device $devno ..."
    vmcp link \* ${devno##*.} ${devno##*.}
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
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
    echo "$(date) MD array still faulty"
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "$num devices are still faulty"
fi

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "mirror not synchronized"
else
    echo "$(date) mirror synchronized"
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    for devno in ${DEVNOS_RIGHT} ; do
	echo "$(date) Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME

	echo "$(date) Detach right device $devno ..."
	vmcp det ${devno##*.}
	echo "$(date) Waiting for 15 seconds ..."
	sleep 15
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
	echo "$(date) Attach right device $devno ..."
	vmcp link \* ${devno##*.} ${devno##*.}
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
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
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
fi

echo "$(date) Stop I/O test"
stop_iotest

wait_for_sync ${MD_NUM}

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
