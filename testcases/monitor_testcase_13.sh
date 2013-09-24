#!/bin/bash
#
# Testcase 13: Pick up failed array
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase13"
MONITOR_TIMEOUT=60

logger "Monitor Testcase 13: Pick up failed array"

stop_md $MD_NUM

activate_dasds

clear_metadata

modprobe vmcp
userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
fi

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
	failed_disks=$(( raid_disks - working_disks)) || true
	[ $working_disks -eq $failed_disks ] && break;
    fi
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    echo "$(date) ERROR: $working_disks / $raid_disks are still working"
fi

echo "$(date) Stop md_monitor"
if ! md_monitor -c"Shutdown" ; then
    error_exit "Failed to stop md_monitor"
fi

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail /dev/${MD_NUM}

echo "$(date) Re-attach disk on first half ..."
for devno in $DEVNOS_LEFT ; do
    if [ "$userid" = "LINUX025" ] ; then
	vmcp link \* ${devno##*.} ${devno##*.}
    else
	vmcp att ${devno##*.} \*
    fi
    break
done

echo "$(date) Start md_monitor"
MONITOR_PID=$(/sbin/md_monitor -y -p 7 -d -s)

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    [ $sleeptime -ge $MONITOR_TIMEOUT ] && break
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- )) || true
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    echo "$(date) ERROR: $num devices are still faulty"
fi

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

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
	    failed_disks=$(( raid_disks - working_disks)) || true
	    [ $working_disks -eq $failed_disks ] && break;
	fi
	sleep 1
	(( sleeptime ++ )) || true
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
	if [ "$userid" = "LINUX025" ] ; then
	    vmcp link \* ${devno##*.} ${devno##*.}
	else
	    vmcp att ${devno##*.} \*
	fi
	break;
    done

    echo "Ok. Waiting for MD to pick up changes ..."
    # Wait for md_monitor to pick up changes
    sleeptime=0
    num=${#DASDS_LEFT[@]}
    while [ $num -gt 0  ] ; do
	[ $sleeptime -ge $MONITOR_TIMEOUT ] && break
	for d in ${DASDS_LEFT[@]} ; do
	    device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
	    if [ "$device" ] ; then
		(( num -- )) || true
	    fi
	done
	[ $num -eq 0 ] && break
	num=${#DASDS_LEFT[@]}
	sleep 1
	(( sleeptime ++ )) || true
    done
    if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
	echo "MD monitor picked up changes after $sleeptime seconds"
    else
	echo "ERROR: $num devices are still faulty"
    fi
    
    wait_for_sync ${MD_NUM} || \
	error_exit "Failed to synchronize array"
    mdadm --detail /dev/${MD_NUM}
fi

trap - EXIT

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
