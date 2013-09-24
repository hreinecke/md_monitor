#!/bin/bash
#
# Testcase 14: Disk quiesce/resume
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase14"
MONITOR_TIMEOUT=60

logger "Monitor Testcase 14: Disk quiesce/resume"

stop_md $MD_NUM

activate_dasds

clear_metadata

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

echo "$(date) Write test file 1 ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024

echo "$(date) Quiesce disks on first half ..."
for d in ${DEVICES_LEFT[@]} ; do
    setdasd -q 1 -d /dev/${d} || \
	error_exit "Cannot quiesce /dev/${d}"
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

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "$(date) Write test file 2 ..."
dd if=/dev/zero of=/mnt/testfile2 bs=4096 count=1024
echo "$(date) Wait for 10 seconds"
sleep 10

echo "$(date) Quiesce disks on second half ..."
for d in ${DEVICES_RIGHT[@]} ; do
    setdasd -q 1 -d /dev/${d} || \
	error_exit "Cannot quiesce /dev/${d}"
done

# Needs to be started in the background, as it'll hang otherwise
( echo "Write test file 3 ..."; \
    dd if=/dev/zero of=/mnt/testfile3 bs=4096 count=1024 oflag=direct; \
    echo "Done" ) &

echo "$(date) Wait for 10 seconds"
sleep 10

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "$(date) Resume disks on second half ..."
for d in ${DEVICES_RIGHT[@]} ; do
    setdasd -q 0 -d /dev/${d} || \
	error_exit "Cannot resume /dev/${d}"
done

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "Write test file 4 ..."
dd if=/dev/zero of=/mnt/testfile4 bs=4096 count=1024

echo "$(date) Resume disks on first half ..."
for d in ${DEVICES_LEFT[@]} ; do
    setdasd -q 0 -d ${d} || \
	error_exit "Cannot resume /dev/${d}"
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

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
