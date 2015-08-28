#!/bin/bash
#
# Testcase 8: Accidental disk overwrite
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase8"
SLEEPTIME=30

stop_md $MD_NUM

activate_dasds

clear_metadata

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    echo "This testcase can only run under z/VM"
    trap - EXIT
    exit 0
fi
ulimit -c unlimited
start_md $MD_NUM

echo "Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

logger "${MD_NAME}: Accidental DASD overwrite"

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | grep Devices > ${MD_LOG1}

echo "Run I/O test"
run_iotest /mnt

echo "Invoke flashcopy"
DEVNO_DST=$(readlink /sys/block/${DASDS_LEFT[1]}/device)
DEVNO_SRC=$(readlink /sys/block/${DASDS_LEFT[0]}/device)
vmcp flashcopy ${DEVNO_DST##*.} 16 32 ${DEVNO_SRC##*.} 0 16

echo "Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $sleeptime -lt $SLEEPTIME  ] ; do
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\](F)\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- )) || true
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $num -gt 0 ] ; then
    stop_iotest
    error_exit "MD monitor did not pick up changes after $sleeptime seconds"
fi

echo "MD monitor picked up changes after $sleeptime seconds"

echo "Stop I/O test"
stop_iotest

# Wait for sync to complete
sleep 5

echo "MD status"
mdadm --detail /dev/${MD_NUM}

old_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
echo "Monitor status: $old_status"

echo "Reset Disk ${DEVICES_LEFT[0]}"
for d in ${DEVICES_LEFT[0]} ; do
    /sbin/md_monitor -c "Remove:/dev/${MD_NUM}@$d"

    if ! mdadm --manage /dev/${MD_NUM} --remove $d ; then
	error_exit "Cannot remove $d in MD array $MD_NUM"
    fi
    md_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
    echo "Monitor status: $md_status"
    wait_md ${MD_NUM}

    if ! dasdfmt -p -y -b 4096 -f ${d%1} ; then
	error_exit "Cannot format device ${d%1}"
    fi
    sleep 2
    if ! fdasd -a ${d%1} ; then
	error_exit "Cannot partition device ${d%1}"
    fi
    sleep 2
    if ! mdadm --manage /dev/${MD_NUM} --add --failfast $d ; then
	error_exit "Cannot add $d to MD array $MD_NUM"
    fi
done

MD_TIMEOUT=15
wait_time=0
while [ $wait_time -lt $MD_TIMEOUT ] ; do
    new_status=$(md_monitor -c "MonitorStatus:/dev/${MD_NUM}")
    [ $new_status != $old_status ] && break
    sleep 1
    (( wait_time++ )) || true
done
if [ $wait_time -ge $MD_TIMEOUT ] ; then
    error_exit "Monitor status hasn't changed for $MD_TIMEOUT seconds"
fi
echo "Monitor status: $new_status"

wait_md ${MD_NUM}
echo "MD status after mdadm --wait:"
cat /proc/mdstat

if ! wait_for_sync ${MD_NUM} ; then
    error_exit "Failed to synchronize array"
fi

MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail /dev/${MD_NUM} | grep Devices > ${MD_LOG2}
if ! diff -u "${MD_LOG1}" "${MD_LOG2}" ; then
    error_exit "Not all devices on ${MD_NUM} are working"
fi
# The array configuration is different from the original one,
# so we cannot compare the final and the initial state
unset START_LOG

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md $MD_NUM
