#!/bin/bash
#
# Testcase 6: reserve DASDs w/ I/O
#

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase6"
IO_TIMEOUT=10
MONITOR_TIMEOUT=60

logger "Monitor Testcase 6: Reserve DASDs w/ I/O"

stop_md $MD_NUM

activate_dasds

clear_metadata

ulimit -c unlimited
start_md $MD_NUM

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

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Reserve DASD $devno on left half ..."
    result=$(wget -nv --max-redirect=0 "http://s390vsl026.suse.de/dasd.php?ccw=${devno}&action=Reserve" 2>&1)
    if [ "$result" != "0 redirections exceeded." ] ; then
	echo "$(date) Reserve DASD $devno failed; wget returned:"
	echo "$result"
    fi
    break;
done

echo "$(date) Wait for reservation on left half ..."
num=0
while [ $num -eq 0 ] ; do
    for d in ${DASDS_LEFT[@]}; do
	state=$(tunedasd -Q /dev/$d)
	if [ "$state" = "other" ] ; then
	    (( num ++ ))
	    break;
	fi
    done
    sleep 1
done
logger "$num DASDs reserved"
echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    mdstat=$(cat /proc/mdstat)
    for d in ${DASDS_LEFT[@]} ; do
	device=$(echo $mdstat | sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]([F|T])\).*/\1/p")
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    [ $sleeptime -gt $MONITOR_TIMEOUT ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ ))
done
if [ $num -eq 0 ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$(date) MD monitor did not pick up changes after $sleeptime seconds"
fi

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Wait for $IO_TIMEOUT seconds"
sleep $IO_TIMEOUT

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Release DASD $devno on left half ..."
    result=$(wget -nv --max-redirect=0 "http://s390vsl026.suse.de/dasd.php?ccw=${devno}&action=Release" 2>&1 )
    if [ "$result" != "0 redirections exceeded." ] ; then
	echo "$(date) Reserve DASD $devno failed; wget returned:"
	echo "$result"
    fi
    break;
done
echo "$(date) Wait for reservations to be cleared ..."
num=${#DASDS_LEFT[@]}
while [ $num -gt 0 ] ; do
    num=0
    for d in ${DASDS_LEFT[@]}; do
	state=$(tunedasd -Q /dev/$d)
	if [ "$state" = "other" ] ; then
	    (( num ++ ))
	fi
    done
    [ $num -eq 0 ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
done
logger "All DASDs released"

echo "$(date) Ok. Waiting for MD to pick up changes ..."
# Wait for md_monitor to pick up changes
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0  ] ; do
    for d in ${DASDS_LEFT[@]} ; do
	device=$(sed -n "s/${MD_NUM}.* \(${d}1\[[0-9]*\]\).*/\1/p" /proc/mdstat)
	if [ "$device" ] ; then
	    (( num -- ))
	fi
    done
    [ $num -eq 0 ] && break
    [ $sleeptime -gt t $MONITOR_TIMEOUT ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ ))
done
if [ $num -eq 0 ] ; then
    echo "$(date) MD monitor picked up changes after $sleeptime seconds"
else
    error_exit "$(date) MD monitor did not pick up changes after $sleeptime seconds"
fi

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync ..."
wait_for_sync ${MD_NUM}

mdadm --detail /dev/${MD_NUM}

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
