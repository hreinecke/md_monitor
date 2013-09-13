#!/bin/bash
#
# Testcase 5: chpid on/off
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase5"
MONITOR_TIMEOUT=60

CHPID_LEFT="0.4b"
CHPID_RIGHT="0.4e"

detach_other_half=1

logger "Monitor Testcase 5: Chpid vary on/off"

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

echo "$(date) vary off on chpid $CHPID_LEFT for the left side"
logger "Vary off chpid $CHPID_LEFT"
chchp -v 0 $CHPID_LEFT
logger "Vary off path $CHPID_LEFT"
chchp -c 0 $CHPID_LEFT

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
    # bail out after the channel path re-activation
    TEST_FAILED=1
fi

if [ -z "$TEST_FAILED" ] ; then
    echo "$(date) Wait for 10 seconds"
    sleep 10
    mdadm --detail /dev/${MD_NUM}
fi

echo "$(date) vary on chpid $CHPID_LEFT for the left side"
logger "Vary on path $CHPID_LEFT"
chchp -c 1 $CHPID_LEFT
logger "Vary on chpid $CHPID_LEFT"
chchp -v 1 $CHPID_LEFT

# Caveat: 'vmcp vary off' will detach the DASDs
echo "$(date) re-attach DASDs"
for dev in /sys/devices/css0/defunct/0.0.* ; do
    devno=${dev##*/}
    vmcp att ${devno#0.0.} \*
done

[ -n "$TEST_FAILED" ] && error_exit "TEST FAILED DUE TO THE ABOVE TIMEOUT ERROR"

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

if [ -z "$detach_other_half" ] ; then
    echo "$(date) Stop I/O test"
    stop_iotest
fi

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM}

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi

if [ -n "$detach_other_half" ] ; then
    echo "$(date) vary off on chpid $CHPID_RIGHT for the right side"
    logger "Vary off chpid $CHPID_RIGHT"
    chchp -v 0 $CHPID_RIGHT
    logger "Vary off path $CHPID_RIGHT"
    chchp -c 0 $CHPID_RIGHT

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
	echo "$(date) MD monitor picked up changes after $sleeptime seconds"
    else
	echo "$(date) ERROR: $working_disks / $raid_disks are still working"
	TEST_FAILED=1
    fi

    sleep 5
    mdadm --detail /dev/${MD_NUM}
    ls /mnt
    echo "$(date) vary on chpid $CHPID_RIGHT for the right side"
    logger "Vary on path $CHPID_RIGHT"
    chchp -c 1 $CHPID_RIGHT
    logger "Vary on chpid $CHPID_RIGHT"
    chchp -v 1 $CHPID_RIGHT
    [ -n "$TEST_FAILED" ] && error_exit "TEST FAILED DUE TO THE ABOVE TIMEOUT ERROR"

    echo "$(date) Ok. Waiting for MD to pick up changes ..."
    # Wait for md_monitor to pick up changes
    sleeptime=0
    num=${#DASDS_RIGHT[@]}
    while [ $num -gt 0  ] ; do
	[ $sleeptime -ge $MONITOR_TIMEOUT ] && break
	for d in ${DASDS_RIGHT[@]} ; do
	    device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
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
	error_exit "$(date) ERROR: $num devices are still faulty"
    fi
    
    echo "$(date) Stop I/O test"
    stop_iotest

    wait_for_sync ${MD_NUM}
    mdadm --detail /dev/${MD_NUM}
fi

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
