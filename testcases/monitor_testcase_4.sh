#!/bin/bash
#
# Testcase 4: Disk attach/detach
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase4"
MONITOR_TIMEOUT=60

logger "Monitor Testcase 4: Disk detach/attach"

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

echo "$(date) Run dt"
run_dt /mnt;

echo "$(date) Detach disk on first half ..."
for devno in ${DEVNOS_LEFT} ; do
    vmcp det ${devno##*.}
    break;
done

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
while [ $sleeptime -lt $MONITOR_TIMEOUT  ] ; do
    raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
    if [ "$raid_status" ] ; then
	raid_disks=${raid_status%/*}
	working_disks=${raid_status#*/}
	failed_disks=$(( raid_disks - working_disks))
	[ $working_disks -eq $failed_disks ] && break;
    fi
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    echo "$(date) ERROR: $working_disks / $raid_disks are still working"
fi

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail /dev/${MD_NUM}

echo "$(date) Re-attach disk on first half ..."
for devno in $DEVNOS_LEFT ; do
    vmcp link \* ${devno##*.} ${devno##*.}
    break
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

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop dt"
killall -KILL dt 2> /dev/null

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM}

mdadm --detail /dev/${MD_NUM}

if [ "$detach_other_half" ] ; then
    echo "Detach disk on second half ..."
    for devno in ${DEVNOS_RIGHT} ; do
	vmcp det ${devno##*.}
	break;
    done

    echo "Ok. Waiting for MD to pick up changes ..."
    sleeptime=0
    while [ $sleeptime -lt 60  ] ; do
	raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
	if [ "$raid_status" ] ; then
	    raid_disks=${raid_status%/*}
	    working_disks=${raid_status#*/}
	    failed_disks=$(( raid_disks - working_disks))
	    [ $working_disks -eq $failed_disks ] && break;
	fi
	sleep 1
	(( sleeptime ++ ))
    done
    if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
	echo "MD monitor picked up changes after $sleeptime seconds"
    else
	echo "ERROR: $working_disks / $raid_disks are still working"
    fi

    sleep 5
    mdadm --detail /dev/${MD_NUM}
    ls /mnt
    echo "Re-attach disk on second half ..."
    for devno in $DEVNOS_RIGHT ; do
	vmcp link \* ${devno##*.} ${devno##*.}
	break;
    done

    echo "Ok. Waiting for MD to pick up changes ..."
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
	echo "MD monitor picked up changes after $sleeptime seconds"
    else
	echo "ERROR: $num devices are still faulty"
    fi
    
    wait_for_sync ${MD_NUM}
    mdadm --detail /dev/${MD_NUM}
fi

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
