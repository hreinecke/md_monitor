#!/bin/bash
#
# Testcase 13: Pick up failed array
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase13"
MD_DEV="/dev/md/${MD_NAME}"

MONITOR_TIMEOUT=60

logger "Monitor Testcase 13: Pick up failed array"

stop_md ${MD_DEV}

activate_devices

clear_metadata

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
fi

ulimit -c unlimited
start_md ${MD_NAME}

echo "$(date) Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "$(date) Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "$(date) Run I/O test"
run_iotest /mnt;

echo "$(date) Detach disk on first half ..."
if [ -n "$DEVNOS_LEFT" ] ; then
    for devno in ${DEVNOS_LEFT} ; do
	vmcp det ${devno##*.}
	break;
    done
else
    for wwid in ${SCSIID_LEFT} ; do
	paths=$(multipathd -k"show map $wwid topology" | \
	        sed -n 's/.*[0-9]*:[0-9]*:[0-9]*:[0-9]* \(sd[a-z]*\) .*/\1/p')
	for path in ${paths} ; do
	    echo 1 > /sys/block/$path/device/delete || \
		error_exit "Cannot remove device ${path} from ${wwid}"
	done
	break;
    done
fi

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) Stop md_monitor"
if ! md_monitor -c"Shutdown" ; then
    error_exit "Failed to stop md_monitor"
fi

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail ${MD_DEV}

echo "$(date) Re-attach disk on first half ..."
if [ -n "$DEVNOS_LEFT" ] ; then
    for devno in $DEVNOS_LEFT ; do
	if [ "$userid" = "LINUX025" ] ; then
	    vmcp link \* ${devno##*.} ${devno##*.}
	else
	    vmcp att ${devno##*.} \*
	fi
	break
    done
else
    for shost in ${SHOSTS_LEFT[@]} ; do
	echo '- - -' > /sys/class/scsi_host/$shost/scan || \
	    error_error "Failed to rescan host $shost"
    done
fi

echo "$(date) Start md_monitor"
MONITOR_PID=$(/sbin/md_monitor -y -p 7 -d -s)

wait_for_md_running_left $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail ${MD_DEV}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync"
wait_for_sync ${MD_DEV} || \
    error_exit "Failed to synchronize array"

mdadm --detail ${MD_DEV}

if [ "$detach_other_half" ] ; then
    if [ -n "$DEVNOS_RIGHT" ] ; then
	echo "Detach disk on second half ..."
	for devno in ${DEVNOS_RIGHT} ; do
	    vmcp det ${devno##*.}
	    break;
	done
    else
	for wwid in ${SCSIID_RIGHT} ; do
	    paths=$(multipathd -k"show map $wwid topology" | \
	        sed -n 's/.*[0-9]*:[0-9]*:[0-9]*:[0-9]* \(sd[a-z]*\) .*/\1/p')
	    for path in ${paths} ; do
		echo 1 > /sys/block/$path/device/delete || \
		    error_exit "Cannot remove device ${path} from ${wwid}"
	    done
	    break;
	done
    fi

    wait_for_md_failed $MONITOR_TIMEOUT

    sleep 5
    mdadm --detail ${MD_DEV}
    ls /mnt
    echo "Re-attach disk on second half ..."
    if [ -n "$DEVNOS_RIGHT" ] ; then
	for devno in $DEVNOS_RIGHT ; do
	    if [ "$userid" = "LINUX025" ] ; then
		vmcp link \* ${devno##*.} ${devno##*.}
	    else
		vmcp att ${devno##*.} \*
	    fi
	    break;
	done
    else
	for shost in ${SHOSTS_RIGHT[@]} ; do
	    echo '- - -' > /sys/class/scsi_host/$shost/scan || \
		error_error "Failed to rescan host $shost"
	done
    fi
    wait_for_md_running_right $MONITOR_TIMEOUT
    
    wait_for_sync ${MD_DEV} || \
	error_exit "Failed to synchronize array"
    mdadm --detail ${MD_DEV}
fi

trap - EXIT

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_DEV}
