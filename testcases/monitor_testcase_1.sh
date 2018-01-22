#!/bin/bash
#
# Testcase 1: array start & shutdown
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase1"
MD_DEV="/dev/md/$MD_NAME"

stop_md $MD_DEV

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME}
MD_NUM=$(resolve_md ${MD_DEV})

logger "${MD_NAME}: Array startup/shutdown"

echo "Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi
sleep 1
echo "Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Write test file ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024
MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"ArrayStatus:${MD_DEV}" | tee ${MD_LOG1}
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
ls -l /mnt | tee ${MD_LOG2}
sleep 5
echo "Umount filesystem ..."
umount /mnt
echo "Stop MD array ..."
mdadm --stop ${MD_DEV}
if md_monitor -c"ArrayStatus:${MD_DEV}" >/dev/null ; then
    error_exit "md_monitor detected live array!"
else
    echo "md_monitor detected stopped array"
fi
echo "Reassemble MD array ..."
mdadm --assemble ${MD_DEV}
wait_md ${MD_DEV}
# md_monitor needs some time to pick up array data
sleep 1
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"ArrayStatus:${MD_DEV}" | tee ${MD_LOG3}
if ! diff ${MD_LOG1} ${MD_LOG3} ;then
    error_exit "Monitor status mismatch"
fi

echo "Remount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot re-mount MD array."
fi

MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
ls -l /mnt | tee ${MD_LOG4}

if ! diff ${MD_LOG2} ${MD_LOG4} ; then
    error_exit "Filesystem contents differ"
fi

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_DEV}
