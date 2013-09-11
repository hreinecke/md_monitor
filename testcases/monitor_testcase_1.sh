#!/bin/bash
#
# Testcase 1: array start & shutdown
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase1"

logger "Monitor Testcase 1: Array startup/shutdown"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM}

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi
sleep 1
echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Write test file ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024
MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | tee ${MD_LOG1}
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
ls -l /mnt | tee ${MD_LOG2}
sleep 5
echo "Umount filesystem ..."
umount /mnt
echo "Stop MD array ..."
mdadm --stop /dev/${MD_NUM}
if md_monitor -c"ArrayStatus:/dev/${MD_NUM}" >/dev/null ; then
    error_exit "md_monitor detected live array!"
else
    echo "md_monitor detected stopped array"
fi
echo "Reassemble MD array ..."
mdadm --assemble /dev/${MD_NUM}
mdadm --wait /dev/${MD_NUM}
# md_monitor needs some time to pick up array data
sleep 1
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | tee ${MD_LOG3}
if ! diff ${MD_LOG1} ${MD_LOG3} ;then
    error_exit "Monitor status mismatch"
fi

echo "Remount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot re-mount MD array."
fi

MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
ls -l /mnt | tee ${MD_LOG4}

if ! diff ${MD_LOG2} ${MD_LOG4} ; then
    error_exit "Filesystem contents differ"
fi
sleep 5

echo "Umount filesystem ..."
umount /mnt

rm -f /tmp/monitor_${MD_NAME}_step*.log
stop_md ${MD_NUM}
