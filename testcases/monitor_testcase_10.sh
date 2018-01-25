#!/bin/bash
#
# Testcase 10: Disk reset
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase10"
MD_DEV="/dev/md/${MD_NAME}"

stop_md ${MD_DEV}

activate_devices

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME} 8

logger "${MD_NAME}: Disk reset"

echo "Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail ${MD_DEV} | sed -n '/Devices/p' > ${MD_LOG1}

echo "Write test file 1 ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
ls -l /mnt | tee ${MD_LOG2}

old_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
echo "Monitor status: $old_status"

echo "Remove first drive on left"
for d in ${DEVICES_LEFT[0]} ; do
    /sbin/md_monitor -c "Remove:${MD_DEV}@$d"

    if ! mdadm --manage ${MD_DEV} --fail $d ; then
	error_exit "Cannot fail $d in MD array $MD_NAME"
    fi
    if ! mdadm --manage ${MD_DEV} --remove $d ; then
	error_exit "Cannot remove $d in MD array $MD_NAME"
    fi
    md_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
    echo "Monitor status: $md_status"
    wait_md ${MD_DEV}
    echo "Reset device $d"
    if ! mdadm --zero-superblock --force $d ; then
	error_exit "Cannot zero superblock on $d"
    fi
    if ! mdadm --manage ${MD_DEV} --add --failfast $d ; then
	error_exit "Cannot add $d to MD array $MD_NAME"
    fi
done
mdadm --detail ${MD_DEV}
echo "Wait for mdadm to complete operation"
starttime=$(date +%s)
wait_md ${MD_DEV}
runtime=$(date +%s)
elapsed=$(( $runtime - $starttime ))
echo "mdadm completed after $elapsed seconds"
MD_TIMEOUT=15
if ! wait_for_monitor $MD_DEV $old_status $MD_TIMEOUT ; then
    error_exit "Monitor status hasn't changed for $MD_TIMEOUT seconds"
fi
newstatus=$(md_monitor -c"MonitorStatus:${MD_DEV}")
echo "Monitor status: $new_status"
mdadm --detail ${MD_DEV}
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
ls -l /mnt | tee ${MD_LOG3}

if ! diff ${MD_LOG2} ${MD_LOG3} ; then
    error_exit "Filesystem contents differ"
fi

MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
mdadm --detail ${MD_DEV} | sed -n '/Devices/p' > ${MD_LOG4}
if ! diff -u "${MD_LOG1}" "${MD_LOG4}" ; then
    error_exit "Not all devices on ${MD_NAME} are working"
fi
# The array configuration is different from the original one,
# so we cannot compare the final and the initial state
unset START_LOG

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_DEV}
