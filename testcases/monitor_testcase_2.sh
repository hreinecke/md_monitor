#!/bin/bash
#
# Testcase 2: fail mirror sides w/o I/O
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase2"
MD_DEV="/dev/md/${MD_NAME}"

stop_md ${MD_DEV}

activate_devices

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME}

logger "${MD_NAME}: Fail both mirror sides w/o I/O"

echo "Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "Write test file ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024

echo "Fail first half ..."
mdadm --manage ${MD_DEV} --fail ${DEVICES_LEFT[@]}
wait_md ${MD_DEV}

if ! wait_for_sync ${MD_DEV} ; then
    error_exit "First half still faulty"
fi

check_md_log step1

echo "Fail second half ..."
mdadm --manage ${MD_DEV} --fail ${DEVICES_RIGHT[@]}
wait_md ${MD_DEV}
if ! wait_for_sync ${MD_DEV} ; then
    error_exit "Second half still faulty"
fi

check_md_log step2

echo "Umount filesystem ..."
umount /mnt

logger "${MD_NAME}: success"

stop_md ${MD_DEV}
