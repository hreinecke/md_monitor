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

wait_for_md_failed $MONITOR_TIMEOUT

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

wait_for_md_running $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

check_md_log step1

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
