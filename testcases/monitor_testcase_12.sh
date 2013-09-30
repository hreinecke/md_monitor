#!/bin/bash
#
# Testcase 12: Successive Disk attach/detach
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase12"
MONITOR_TIMEOUT=60
SLEEPTIME=30

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

modprobe vmcp
userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
fi

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: Successive Disk detach/attach"

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

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | tee ${MD_LOG1}

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME

    echo "$(date) Detach left device $devno ..."
    dasd=${devno##*.}
    if ! vmcp det ${dasd} ; then
	error_exit "Cannot detach DASD ${dasd}"
    fi
    push_recovery_fn "attach_dasd $userid ${dasd}"
    echo "$(date) Waiting for 15 seconds ..."
    sleep 15
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    echo "$(date) Attach left device $devno ..."
    pop_recovery_fn || \
	error_exit "Cannot attach DASD ${dasd}"

    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
done

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "Failed to synchronize array"
fi
echo "$(date) mirror synchronized"
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | tee ${MD_LOG2}
if ! diff -pu ${MD_LOG1} ${MD_LOG2} ; then
    stop_iotest
    error_exit "inconsistent md_monitor status after detaching left half"
fi

for devno in ${DEVNOS_RIGHT} ; do
    echo "$(date) Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME
    
    echo "$(date) Detach right device $devno ..."
    dasd=${devno##*.}
    if ! vmcp det ${dasd} ; then
	error_exit "Cannot detach DASD ${dasd}"
    fi
    push_recovery_fn "attach_dasd $userid ${dasd}"
    echo "$(date) Waiting for 15 seconds ..."
    sleep 15
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    echo "$(date) Attach right device $devno ..."
    if ! pop_recovery_fn ; then
	error_exit "Cannot attach DASD ${dasd}"
    fi
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
done

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "Failed to synchronize array"
fi
echo "$(date) sync finished"
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | tee ${MD_LOG3}
if ! diff -pu ${MD_LOG1} ${MD_LOG3} ; then
    stop_iotest
    error_exit "inconsistent md monitor status after detaching right half"
fi

logger "${MD_NAME}: success"

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
