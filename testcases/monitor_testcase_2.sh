#!/bin/bash
#
# Testcase 2: fail mirror sides w/o I/O
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase2"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: Fail both mirror sides w/o I/O"

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
wait_md ${MD_NUM}

if ! wait_for_sync ${MD_NUM} ; then
    error_exit "First half still faulty"
fi

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi
echo "Fail second half ..."
mdadm --manage /dev/${MD_NUM} --fail ${DEVICES_RIGHT[@]}
wait_md ${MD_NUM}
if ! wait_for_sync ${MD_NUM} ; then
    error_exit "Second half still faulty"
fi
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG2}
if ! diff -u "${START_LOG}" "${MD_LOG2}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi
rm -f ${MD_LOG1} ${MD_LOG2}

echo "Umount filesystem ..."
umount /mnt

logger "${MD_NAME}: success"

stop_md ${MD_NUM}
