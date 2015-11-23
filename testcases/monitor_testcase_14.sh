#!/bin/bash
#
# Testcase 14: Disk quiesce/resume
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase14"
MONITOR_TIMEOUT=60

function resume_dasd() {
    local dasd=$1

    setdasd -q 0 -d /dev/${dasd} || \
	error_exit "Cannot resume /dev/${dasd}"
}

function online_scsi() {
    local sdev=$1

    if ! echo running > /sys/block/$sdev/device/state ; then
	error_exit "Cannot set device $sdev online"
    fi
}

logger "Monitor Testcase 14: Disk quiesce/resume"

stop_md $MD_NUM

activate_devices

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

if [ -n "$DEVNOS_LEFT" ] ; then
    echo "$(date) Quiesce disks on first half ..."
    for d in ${DEVICES_LEFT[@]} ; do
	setdasd -q 1 -d /dev/${d} || \
	    error_exit "Cannot quiesce /dev/${d}"
	push_recovery_fn "resume_dasd ${d}"
    done
else
    for sdev in ${SDEVS_LEFT[@]} ; do
	echo offline > /sys/block/$sdev/device/state || \
	    error_exit "Cannot offline device $sdev"
	push_recovery_fn "online_scsi $sdev"
    done
fi

wait_for_md_failed $MONITOR_TIMEOUT

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "$(date) Write test file 2 ..."
dd if=/dev/zero of=/mnt/testfile2 bs=4096 count=1024
echo "$(date) Wait for 10 seconds"
sleep 10

echo "$(date) Quiesce disks on second half ..."

if [ -n "$DEVNOS_RIGHT" ] ; then
    for d in ${DEVICES_RIGHT[@]} ; do
	setdasd -q 1 -d /dev/${d} || \
	    error_exit "Cannot quiesce /dev/${d}"
	push_recovery_fn "resume_dasd ${d}"
    done
else
    for sdev in ${SDEVS_RIGHT[@]} ; do
	echo offline > /sys/block/$sdev/device/state || \
	    error_exit "Cannot offline device $sdev"
	push_recovery_fn "online_scsi $sdev"
    done
fi


# Needs to be started in the background, as it'll hang otherwise
( echo "Write test file 3 ..."; \
    dd if=/dev/zero of=/mnt/testfile3 bs=4096 count=1024 oflag=direct; \
    echo "Done" ) &

echo "$(date) Wait for 10 seconds"
sleep 10

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "$(date) Resume disks on second half ..."

if [ -n "$DEVNOS_RIGHT" ] ; then
    for d in ${DEVICES_RIGHT[@]} ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done
else
    for sdev in ${SDEVS_RIGHT[@]} ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done
fi

md_monitor -c "MonitorStatus:/dev/${MD_NUM}"

echo "Write test file 4 ..."
dd if=/dev/zero of=/mnt/testfile4 bs=4096 count=1024

echo "$(date) Resume disks on first half ..."
if [ -n "$DEVNOS_LEFT" ] ; then
    for d in ${DEVICES_LEFT[@]} ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done
else
    for sdev in ${SDEVS_LEFT[@]} ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done
fi

wait_for_md_running_left $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

check_md_log step1

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
