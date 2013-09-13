#!/bin/bash
#
# Testcase 2: fail mirror sides w/o I/O
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase2"

logger "Monitor Testcase 2: Fail both mirror sides w/o I/O"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM}

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Write test file ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024

echo "Fail first half ..."
mdadm --manage /dev/${MD_NUM} --fail ${DEVICES_LEFT[@]}
mdadm --detail /dev/${MD_NUM}
sleep 10
MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi
echo "Fail second half ..."
mdadm --manage /dev/${MD_NUM} --fail ${DEVICES_RIGHT[@]}
mdadm --detail /dev/${MD_NUM}
sleep 10
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG2}
if ! diff -u "${START_LOG}" "${MD_LOG2}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi
rm -f ${MD_LOG1} ${MD_LOG2}

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
