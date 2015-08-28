#!/bin/bash
#
# Testcase 4: Disk attach/detach
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase4"
MONITOR_TIMEOUT=60

function attach_dasd() {
    local userid=$1
    local devno=$2
    
    if [ "$userid" = "LINUX025" ] ; then
	vmcp link \* ${devno##*.} ${devno##*.} || \
	    error_exit "Cannot link device $devno"
    else
	vmcp att ${devno##*.} \* || \
	    error_exit "Cannot attach device $devno"
    fi
}

stop_md $MD_NUM

activate_dasds

clear_metadata

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "This testcase can only run under z/VM"
fi

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: Disk detach/attach"

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
    vmcp det ${devno##*.} || \
	error_exit "Cannot detach device ${devno##*.}"
    push_recovery_fn "attach_dasd $userid ${devno##*.}"
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
    error_exit "$working_disks / $raid_disks are still working"
fi

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail /dev/${MD_NUM}

echo "$(date) Re-attach disk on first half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    [ $sleeptime -ge $MONITOR_TIMEOUT ] && break
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
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
    error_exit "$(date) ERROR: $num devices are still faulty"
fi

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi

if [ "$detach_other_half" ] ; then
    echo "Detach disk on second half ..."
    for devno in ${DEVNOS_RIGHT} ; do
	vmcp det ${devno##*.}
	push_recovery_fn "attach_dasd $userid ${devno##*.}"
	break;
    done

    echo "$(date) Ok. Waiting for MD to pick up changes ..."
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
	echo "$(date) MD monitor picked up changes after $sleeptime seconds"
    else
	error_exit "$working_disks / $raid_disks are still working"
    fi

    sleep 5
    mdadm --detail /dev/${MD_NUM}
    ls /mnt
    echo "Re-attach disk on second half ..."
    while true ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done

    echo "$(date) Ok. Waiting for MD to pick up changes ..."
    # Wait for md_monitor to pick up changes
    sleeptime=0
    num=${#DASDS_LEFT[@]}
    while [ $num -gt 0  ] ; do
	[ $sleeptime -ge $MONITOR_TIMEOUT ] && break
	for d in ${DASDS_LEFT[@]} ; do
	    device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
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
	error_exit "$(date) ERROR: $num devices are still faulty"
    fi
    
    wait_for_sync ${MD_NUM} || \
	error_exit "Failed to synchronize array"

    mdadm --detail /dev/${MD_NUM}
fi

logger "${MD_NAME}: success"

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
