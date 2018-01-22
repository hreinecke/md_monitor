#!/bin/bash
#
# Testcase 3: Disk online/offline
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase3"
MD_DEV="/dev/md/${MD_NAME}"
MONITOR_TIMEOUT=30

function online_dasd() {
    local devno=$1

    if ! echo 1 > /sys/bus/ccw/devices/$devno/online ; then
	error_exit "Cannot set device $devno online"
    fi
}

stop_md $MD_DEV

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME}

logger "${MD_NAME}: Disk offline/online"

echo "Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
    error_exit "Cannot mount MD array."
fi

MD_LOG1=$(md_monitor -c"MonitorStatus:${MD_DEV}")

echo "Write test file 1 ..."
dd if=/dev/zero of=/mnt/testfile1 bs=4096 count=1024

echo "Shutting down first half ..."
for d in ${DEVICES_LEFT[@]} ; do
    md_monitor -c "Remove:${MD_DEV}@$d"
done
if ! mdadm --manage ${MD_DEV} --fail ${DEVICES_LEFT[@]} ; then
    error_exit "Cannot fail first half in MD array $MD_NAME"
fi
wait_md ${MD_DEV}
if ! mdadm --manage ${MD_DEV} --remove ${DEVICES_LEFT[@]} ; then
    error_exit "Cannot fail $d in MD array $MD_NAME"
fi
wait_md ${MD_DEV}
for devno in $DEVNOS_LEFT ; do
    if ! echo 0 > /sys/bus/ccw/devices/$devno/online ; then
	error_exit "Cannot set device $devno offline"
    fi
    push_recovery_fn "online_dasd $devno"
done
echo "Write test file 2 ..."
dd if=/dev/zero of=/mnt/testfile2 bs=4096 count=1024
sleep 6
mdadm --detail ${MD_DEV}
ls -l /mnt
echo "Restart first half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

#
# wait_for_sync will fail here, as the devices
# are being activated from udev on a one-by-one
# basis. So for every device md will start recovery
# and wait_for_sync will exit after the currently
# running recovery has finished, even though there
# might be other device events pending.
# So wait for md_monitor to pick up the changes
# first, and then check for sync.
#
echo "Wait for md_monitor to pick up changes"
sleeptime=0
while [ $sleeptime -lt $MONITOR_TIMEOUT ] ; do
    MD_LOG2=$(md_monitor -c"MonitorStatus:${MD_DEV}")
    if [ "${MD_LOG1}" = "${MD_LOG2}" ] ; then
	break;
    fi
    (( sleeptime ++ )) || true
    sleep 1
done
if [ $sleeptime -ge $MONITOR_TIMEOUT ] ; then
    echo "Monitor status does not match: is ${MD_LOG2} was ${MD_LOG1}"
    error_exit "md_monitor did not pick up changes after $sleeptime seconds"
fi
if ! wait_for_sync ${MD_DEV} ; then
    md_monitor -c"ArrayStatus:${MD_DEV}"
    error_exit "Failed to activate first half"
fi

mdadm --detail ${MD_DEV}
md_monitor -c"ArrayStatus:${MD_DEV}"

echo "Shutting down second half ..."
for d in ${DEVICES_RIGHT[@]} ; do
    md_monitor -c "Remove:${MD_DEV}@$d"
done
if ! mdadm --manage ${MD_DEV} --fail ${DEVICES_RIGHT[@]} ; then
    error_exit "Cannot fail $d in MD array $MD_NAME"
fi
wait_md ${MD_DEV}
if ! mdadm --manage ${MD_DEV} --remove ${DEVICES_RIGHT[@]} ; then
    error_exit "Cannot remove $d in MD array $MD_NAME"
fi
wait_md ${MD_DEV}
for devno in $DEVNOS_RIGHT ; do
    if ! echo 0 > /sys/bus/ccw/devices/$devno/online ; then
	error_exit "Cannot set device $devno offline"
    fi
    push_recovery_fn "online_dasd $devno"
done
echo "Write test file 3 ..."
dd if=/dev/zero of=/mnt/testfile3 bs=4096 count=1024
sleep 5
mdadm --detail ${MD_DEV}
ls -l /mnt
echo "Restart second half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

echo "Wait for md_monitor to pick up changes"
sleeptime=0
while [ $sleeptime -lt $MONITOR_TIMEOUT ] ; do
    MD_LOG3=$(md_monitor -c"MonitorStatus:${MD_DEV}")
    if [ "${MD_LOG1}" = "${MD_LOG3}" ] ; then
	break;
    fi
    (( sleeptime ++ )) || true
    sleep 1
done
if [ $sleeptime -ge $MONITOR_TIMEOUT ] ; then
    echo "Monitor status does not match: is ${MD_LOG3} was ${MD_LOG1}"
    error_exit "md_monitor did not pick up changes after $sleeptime seconds"
fi
if ! wait_for_sync ${MD_DEV} ; then
    md_monitor -c"ArrayStatus:${MD_DEV}"
    error_exit "Failed to activate second half"
fi
mdadm --detail ${MD_DEV}
ls -l /mnt

diff -u /dev/stdin <(stat --printf='%n %s\n' /mnt/testfile*) <<EOE
/mnt/testfile1 4194304
/mnt/testfile2 4194304
/mnt/testfile3 4194304
EOE

echo "Umount filesystem ..."
umount /mnt

logger "${MD_NAME}: success"

stop_md ${MD_DEV}
