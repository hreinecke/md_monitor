#!/bin/bash
#
# Testcase 4: Disk attach/detach
#             (Disk online/offline for zFCP)
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

function attach_scsi() {
    local devno=$1

    vmcp att ${devno##*.} \* || \
	error_exit "Cannot attach device $devno"
}

stop_md $MD_NUM

activate_devices

clear_metadata

if [ -n "$DEVNOS_LEFT" ] ; then
    userid=$(vmcp q userid | cut -f 1 -d ' ')
    if [ -z "$userid" ] ; then
	error_exit "This testcase can only run under z/VM"
    fi
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

if [ -n "$DEVNOS_LEFT" ] ; then
    echo "$(date) Detach disk on first half ..."
    for devno in ${DEVNOS_LEFT} ; do
	vmcp det ${devno##*.} || \
	    error_exit "Cannot detach device ${devno##*.}"
	push_recovery_fn "attach_dasd $userid ${devno##*.}"
	break;
    done
else
    echo "$(date) Detach HBA on first half ..."
    for shost in ${SHOSTS_LEFT[@]} ; do
	hostpath=$(cd -P /sys/class/scsi_host/$shost)
	ccwpath=${devpath%/host*}
	devno=${ccwpath##*/}
	vmcp det ${devno##*.} || \
	    error_exit "Cannot detach device ${devno##*.}"
	push_recovery_fn "attach_scsi ${devno##*.}"
    done
fi

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail /dev/${MD_NUM}

echo "$(date) Re-attach disk on first half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

wait_for_md_running_left $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

check_md_log step1

if [ "$detach_other_half" ] ; then
    if [ -n "$DEVNOS_RIGHT" ] ; then
	echo "Detach disk on second half ..."
	for devno in ${DEVNOS_RIGHT} ; do
	    vmcp det ${devno##*.}
	    push_recovery_fn "attach_dasd $userid ${devno##*.}"
	    break;
	done
    else
	echo "$(date) setting second half offline ..."
	for shost in ${SHOSTS_RIGHT[@]} ; do
	    hostpath=$(cd -P /sys/class/scsi_host/$shost)
	    ccwpath=${devpath%/host*}
	    devno=${ccwpath##*/}
	    vmcp det ${devno##*.} || \
		error_exit "Cannot detach device ${devno##*.}"
	    push_recovery_fn "attach_scsi ${devno##*.}"
	done
    fi

    wait_for_md_failed $MONITOR_TIMEOUT

    sleep 5
    mdadm --detail /dev/${MD_NUM}
    ls /mnt
    echo "Re-attach disk on second half ..."
    while true ; do
	if ! pop_recovery_fn ; then
	    break;
	fi
    done

    wait_for_md_running_right $MONITOR_TIMEOUT
    
    wait_for_sync ${MD_NUM} || \
	error_exit "Failed to synchronize array"

    check_md_log step2
fi

logger "${MD_NAME}: success"

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
