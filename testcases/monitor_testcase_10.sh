#!/bin/bash
#
# Testcase 10: Disk reset
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase10"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM} 8

logger "${MD_NAME}: Disk reset"

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | grep Devices > ${MD_LOG1}

echo "Write test file 1 ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
ls -l /mnt | tee ${MD_LOG2}

old_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
echo "Monitor status: $old_status"

echo "Remove first drive on left"
for d in ${DEVICES_LEFT[0]} ; do
    /sbin/md_monitor -c "Remove:/dev/${MD_NUM}@$d"

    if ! mdadm --manage /dev/${MD_NUM} --fail $d ; then
	error_exit "Cannot fail $d in MD array $MD_NUM"
    fi
    if ! mdadm --manage /dev/${MD_NUM} --remove $d ; then
	error_exit "Cannot remove $d in MD array $MD_NUM"
    fi
    md_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
    echo "Monitor status: $md_status"
    wait_md ${MD_NUM}
    echo "Reset device $d"
    if ! mdadm --zero-superblock --force $d ; then
	error_exit "Cannot zero superblock on $d"
    fi
    if ! mdadm --manage /dev/${MD_NUM} --add --failfast $d ; then
	error_exit "Cannot add $d to MD array $MD_NUM"
    fi
done
mdadm --detail /dev/${MD_NUM}
wait_md ${MD_NUM}
MD_TIMEOUT=15
wait_time=0
while [ $wait_time -lt $MD_TIMEOUT ] ; do
    new_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
    [ $new_status == $old_status ] && break
    sleep 1
    (( wait_time++ )) || true
done
if [ $wait_time -ge $MD_TIMEOUT ] ; then
    error_exit "Monitor status hasn't changed for $MD_TIMEOUT seconds"
fi
echo "Monitor status: $new_status"
mdadm --detail /dev/${MD_NUM}
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
ls -l /mnt | tee ${MD_LOG3}

if ! diff ${MD_LOG2} ${MD_LOG3} ; then
    error_exit "Filesystem contents differ"
fi

MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
mdadm --detail /dev/${MD_NUM} | grep Devices > ${MD_LOG4}
if ! diff -u "${MD_LOG1}" "${MD_LOG4}" ; then
    error_exit "Not all devices on ${MD_NUM} are working"
fi
# The array configuration is different from the original one,
# so we cannot compare the final and the initial state
unset START_LOG

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
