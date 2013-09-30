#!/bin/bash
#
# Testcase 5: chpid on/off
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase5"
MONITOR_TIMEOUT=60

CHPID_LEFT="0.4b"
CHPID_RIGHT="0.4e"

detach_other_half=1

modprobe vmcp
userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
elif [ "$userid" != "LINUX021" ] ; then
    echo "This testcase can only run on z/VM guest LINUX021"
    trap - EXIT
    exit 0
fi

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: Chpid vary on/off"

echo "$(date) Create filesystem ..."
if ! mkfs.ext3 /dev/${MD_NUM} ; then
    error_exit "Cannot create fs"
fi

echo "$(date) Mount filesystem ..."
if ! mount /dev/${MD_NUM} /mnt ; then
    error_exit "Cannot mount MD array."
fi

echo "$(date) Run I/O test"
run_iotest /mnt;

# clean up for the channel path stuff
function chp_configure() {
    local chpid=$1

    chchp -c 1 $chpid || \
	error_exit "Cannot set chpid $chpid to 'configured'"
}

function chp_vary_on() {
    local chpid=$1

    chchp -v 1 $chpid || \
	error_exit "Cannot vary on chpid $chpid"
}

echo "$(date) vary off on chpid $CHPID_LEFT for the left side"
logger "Vary off chpid $CHPID_LEFT"
chchp -v 0 $CHPID_LEFT || \
    error_exit "Cannot vary off chpid $CHPID_LEFT"
push_recovery_fn "chp_vary_on $CHPID_LEFT"
logger "Set chpid $CHPID_LEFT to 'standby'"
if chchp -c 0 $CHPID_LEFT ; then
    push_recovery_fn "chp_configure ${CHPID_LEFT}"
else
    echo "Cannot set chpid $CHPID_LEFT to 'standby', ignoring"
fi

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
while [ $sleeptime -lt $MONITOR_TIMEOUT  ] ; do
    raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
    if [ "$raid_status" ] ; then
	raid_disks=${raid_status%/*}
	working_disks=${raid_status#*/}
	failed_disks=$(( raid_disks - working_disks))
	[ $working_disks -eq $failed_disks ] && break;
    fi
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "ERROR: $working_disks / $raid_disks are still working"
fi

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail /dev/${MD_NUM}

echo "$(date) vary on chpid $CHPID_LEFT for the left side"
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

# Caveat: 'vmcp vary off' will detach the DASDs
echo "$(date) re-attach DASDs"
for dev in /sys/devices/css0/defunct/0.0.* ; do
    [ -e $dev ] || continue
    devno=${dev##*/}
    vmcp att ${devno#0.0.} \* || \
	error_exit "Cannot attach device ${devno#0.0.}"
done

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    [ $sleeptime -ge $MONITOR_TIMEOUT ] && break
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$(date) ERROR: $num devices are still faulty"
fi

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Wait for sync"
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG1}
if ! diff -u "${START_LOG}" "${MD_LOG1}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi

echo "$(date) vary off on chpid $CHPID_RIGHT for the right side"
logger "Vary off chpid $CHPID_RIGHT"
chchp -v 0 $CHPID_RIGHT || \
    error_exit "Cannot vary off chpid $CHPID_RIGHT"
push_recovery_fn "chp_vary_on $CHPID_RIGHT"
logger "Set chpid $CHPID_RIGHT to 'standby'"
if chchp -c 0 $CHPID_RIGHT ; then
    push_recovery_fn "chp_configure ${CHPID_RIGHT}"
else
    echo "Cannot set chpid $CHPID_RIGHT to 'standby', ignoring"
fi

echo "Ok. Waiting for MD to pick up changes ..."
sleeptime=0
while [ $sleeptime -lt 60  ] ; do
    raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
    if [ "$raid_status" ] ; then
	raid_disks=${raid_status%/*}
	working_disks=${raid_status#*/}
	failed_disks=$(( raid_disks - working_disks))
	[ $working_disks -eq $failed_disks ] && break;
    fi
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "ERROR: $working_disks / $raid_disks are still working"
fi
sleep 5
mdadm --detail /dev/${MD_NUM}
ls /mnt
echo "$(date) vary on chpid $CHPID_RIGHT for the right side"
logger "Vary on path $CHPID_RIGHT"
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

echo "$(date) Ok. Waiting for MD to pick up changes ..."
    # Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_RIGHT[@]}
while [ $num -gt 0  ] ; do
    [ $sleeptime -ge $MONITOR_TIMEOUT ] && break
    for d in ${DASDS_RIGHT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_RIGHT[@]}
    sleep 1
    (( sleeptime ++ ))
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$(date) ERROR: $num devices are still faulty"
fi
    
echo "$(date) Stop I/O test"
stop_iotest

wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail /dev/${MD_NUM} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG2}
if ! diff -u "${START_LOG}" "${MD_LOG2}" ; then
    error_exit "current ${MD_NUM} state differs after test but should be identical to initial state"
fi

logger "${MD_NAME}: success"

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
