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
md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
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
md_monitor -c"ArrayStatus:/dev/${MD_NUM}"

echo "Remount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot re-mount MD array."
fi

ls -l /mnt

sleep 5

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
