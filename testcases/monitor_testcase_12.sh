#!/bin/bash
#
# Testcase 12: Successive Disk attach/detach
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase12"
MONITOR_TIMEOUT=60
SLEEPTIME=30

logger "Monitor Testcase 12: Successive Disk detach/attach"

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

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" > ${MD_LOG1}

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME

    echo "$(date) Detach left device $devno ..."
    vmcp det ${devno##*.}
    echo "$(date) Waiting for 15 seconds ..."
    sleep 15
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    echo "$(date) Attach left device $devno ..."
    vmcp link \* ${devno##*.} ${devno##*.}
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
done

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "mirror not synchronized"
fi
echo "$(date) mirror synchronized"
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" > ${MD_LOG2}
if ! diff -pu ${MD_LOG1} ${MD_LOG2} ; then
    stop_iotest
    error_exit "MD monitor reported array failure"
fi

for devno in ${DEVNOS_RIGHT} ; do
    echo "$(date) Waiting for $SLEEPTIME seconds ..."
    sleep $SLEEPTIME
    
    echo "$(date) Detach right device $devno ..."
    vmcp det ${devno##*.}
    echo "$(date) Waiting for 15 seconds ..."
    sleep 15
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    echo "$(date) Attach right device $devno ..."
    vmcp link \* ${devno##*.} ${devno##*.}
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
done

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    stop_iotest
    error_exit "mirror not synchronized"
fi
echo "$(date) sync finished"
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" > ${MD_LOG3}
if ! diff -pu ${MD_LOG1} ${MD_LOG3} ; then
    stop_iotest
    error_exit "MD monitor reported array failure"
fi

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
