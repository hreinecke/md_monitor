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

wait_for_md_failed $MONITOR_TIMEOUT

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

wait_for_md_running $MONITOR_TIMEOUT

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

    wait_for_md_failed $MONITOR_TIMEOUT

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

    wait_for_md_running $MONITOR_TIMEOUT
    
    wait_for_sync ${MD_NUM} || \
	error_exit "Failed to synchronize array"
    mdadm --detail /dev/${MD_NUM}
fi

trap - EXIT

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
