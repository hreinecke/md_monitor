#!/bin/bash
#
# Testcase 9: Disk replacement
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase9"
MD_DEV="/dev/md/${MD_NAME}"

stop_md ${MD_DEV}

activate_devices

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME} 6

logger "${MD_NAME}: Disk replacement"

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
done
md_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
echo "Monitor status: $md_status"
wait_md ${MD_DEV}
echo "Add new device on left side"
for d in ${DEVICES_LEFT[3]} ; do
    if [ -n "$DEVNOS_LEFT" ] ; then
	if ! dasdfmt -p -y -b 4096 -f ${d%1} ; then
	    error_exit "Cannot format device ${d%1}"
	fi
	sleep 2
	if ! fdasd -a ${d%1} ; then
	    error_exit "Cannot partition device ${d%1}"
	fi
    else
	dd if=/dev/zero of=${d} bs=1M count=64
    fi
    sleep 2
    if ! mdadm --manage ${MD_DEV} --add --failfast $d ; then
	error_exit "Cannot add $d to MD array $MD_NAME"
    fi
done
mdadm --detail ${MD_DEV}
wait_md ${MD_DEV}
MD_TIMEOUT=15
wait_time=0
while [ $wait_time -lt $MD_TIMEOUT ] ; do
    new_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
    [ $new_status == $old_status ] && break
    sleep 1
    (( wait_time++ )) || true
done
if [ $wait_time -ge $MD_TIMEOUT ] ; then
    error_exit "Monitor status hasn't changed for $MD_TIMEOUT seconds"
fi
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
