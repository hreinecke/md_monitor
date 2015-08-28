#!/bin/bash
#
# Testcase 16: Reboot during recovery  w/ root on MD
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MONITOR_TIMEOUT=30
logger "Monitor Testcase 16: Reboot during recovery w/ I/O on root on MD"

ulimit -c unlimited

function resume_dasd() {
    local dasd=$1

    echo "Resume $dasd"
    setdasd -q 0 -d ${dasd} || \
	error_exit "Cannot resume /dev/${dasd}"
}

MD_DEV=$(mount | grep ' / ' | cut -d ' ' -f 1)
MD_NUM=${MD_DEV##*/}
if [ "${MD_NUM##md}" = "${MD_NUM}" ] ; then
    echo "Testcase can only be run with root on MD"
    exit 0
fi

for dasd in $(mdadm --detail ${MD_DEV} | sed -n 's/.*set-A failfast *\/dev\/\(.*\)/\1/p') ; do
    dasd=${dasd%1}
    DASDS_LEFT+=("$dasd")
done
num=0
echo "$(date) Quiesce disks on first half ..."
for d in ${DASDS_LEFT[@]} ; do
    setdasd -q 1 -d /dev/${d} || \
	error_exit "Cannot quiesce /dev/${d}"
    push_recovery_fn "resume_dasd /dev/${d}"
done

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
    (( sleeptime ++ )) || true
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$working_disks / $raid_disks are still working"
fi

echo "$(date) Resume disks on first half ..."
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
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
	    (( num -- )) || true
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $sleeptime -lt $MONITOR_TIMEOUT ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$(date) ERROR: $num devices are still faulty"
fi

sleep 2
# Stop z/VM guest
vmcp cp cpu all stop
