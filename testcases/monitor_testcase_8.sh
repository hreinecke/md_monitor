#!/bin/bash
#
# Testcase 8: Accidental disk overwrite
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase8"
MD_DEV="/dev/md/${MD_NAME}"
SLEEPTIME=30

function resume_mpath() {
    local mpath=$1

    dmsetup resume $mpath || \
	error_exit "Cannot resume $mpath"
}

stop_md ${MD_DEV}

activate_devices

clear_metadata

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    echo "This testcase can only run under z/VM"
    trap - EXIT
    exit 0
fi
ulimit -c unlimited
start_md ${MD_NAME}

echo "Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

logger "${MD_NAME}: Accidental DASD overwrite"

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail ${MD_DEV} | sed -n '/Devices/p' > ${MD_LOG1}

echo "Run I/O test"
run_iotest /mnt

if [ -n "$DEVNOS_LEFT" ] ; then
    echo "Invoke flashcopy"
    DEVNO_SRC=$(readlink /sys/block/${DASDS_LEFT[1]}/device)
    DEVNO_DST=$(readlink /sys/block/${DASDS_LEFT[0]}/device)
    vmcp flashcopy ${DEVNO_SRC##*.} 16 32 ${DEVNO_DST##*.} 0 16
else
    echo "Invoke sg_dd"
    for mpath in ${DEVICES_LEFT[@]} ; do
	dmsetup suspend $mpath || \
	    error_exit "Cannot suspend device $mpath"
	push_recovery_fn "resume_mpath $mpath"
	break;
    done
    SDEV_SRC=${SDEVS_LEFT[2]}
    SDEV_DST=${SDEVS_LEFT[0]}
    sg_dd iflag=sgio,dsync,direct if=/dev/$SDEV_SRC oflag=sgio,dsync,direct of=/dev/$SDEV_DST bs=4096 count=16 skip=16
    pop_recovery_fn
fi

wait_for_md_failed $SLEEPTIME

echo "Stop I/O test"
stop_iotest

# Wait for sync to complete
sleep 5

echo "MD status"
mdadm --detail ${MD_DEV}

old_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
echo "Monitor status: $old_status"

echo "Reset Disk ${DEVICES_LEFT[0]}"
for d in ${DEVICES_LEFT[0]} ; do
    /sbin/md_monitor -c "Remove:${MD_DEV}@$d"

    if ! mdadm --manage ${MD_DEV} --remove $d ; then
	error_exit "Cannot remove $d in MD array $MD_NAME"
    fi
    md_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
    echo "Monitor status: $md_status"
    wait_md ${MD_DEV}
    if [ "$DEVNOS_LEFT" ] ; then
	sleep 1
	if ! dasdfmt -p -y -b 4096 -f ${d%1} ; then
	    error_exit "Cannot format device ${d%1}"
	fi
	sleep 2
	if ! fdasd -a ${d%1} ; then
	    error_exit "Cannot partition device ${d%1}"
	fi
    fi
    sleep 2
    if ! mdadm --manage ${MD_DEV} --add --failfast $d ; then
	error_exit "Cannot add $d to MD array $MD_NAME"
    fi
done

MD_TIMEOUT=15
wait_time=0
while [ $wait_time -lt $MD_TIMEOUT ] ; do
    new_status=$(md_monitor -c "MonitorStatus:${MD_DEV}")
    [ $new_status != $old_status ] && break
    sleep 1
    (( wait_time++ )) || true
done
if [ $wait_time -ge $MD_TIMEOUT ] ; then
    error_exit "Monitor status hasn't changed for $MD_TIMEOUT seconds"
fi
echo "Monitor status: $new_status"

wait_md ${MD_DEV}
echo "MD status after mdadm --wait:"
cat /proc/mdstat

if ! wait_for_sync ${MD_DEV} ; then
    error_exit "Failed to synchronize array"
fi

MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail ${MD_DEV} | sed -n '/Devices/p' > ${MD_LOG2}
if ! diff -u "${MD_LOG1}" "${MD_LOG2}" ; then
    error_exit "Not all devices on ${MD_NAME} are working"
fi
# The array configuration is different from the original one,
# so we cannot compare the final and the initial state
unset START_LOG

logger "${MD_NAME}: success"

echo "Umount filesystem ..."
umount /mnt

stop_md ${MD_DEV}
