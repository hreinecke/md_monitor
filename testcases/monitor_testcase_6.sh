#!/bin/bash
#
# Testcase 6: reserve DASDs w/ I/O
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase6"
IO_TIMEOUT=10
MONITOR_TIMEOUT=15

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

logger "${MD_NAME}: Reserve DASDs w/ I/O"

echo "$(date) Run I/O test"
run_iotest /mnt;

run_wget() {
    local devno=$1
    local action=$2

    result=$(curl -f "http://s390vsl026.suse.de/dasd.php?ccw=${devno}&action=${action}" 2>&1)

    if [ $? -ne 0 ] ; then
	echo $result
	return 1
    fi
    return 0
}

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Reserve DASD $devno on left half ..."
    result=$(run_wget ${devno} online)
    if [ "$result" ] ; then
	echo "$(date) Activate DASD $devno failed; wget returned:"
	echo "$result"
	exit 1
    fi
    result=$(run_wget ${devno} Reserve)
    if [ "$result" ] ; then
	echo "$(date) Reserve DASD $devno failed; wget returned:"
	echo "$result"
	exit 1
    fi
    break;
done

echo "$(date) Wait for reservation on left half ..."
sleeptime=0
num=0
while [ $num -eq 0 ] ; do
    for d in ${DASDS_LEFT[@]}; do
	state=$(tunedasd -Q /dev/$d)
	if [ "$state" = "other" ] ; then
	    (( num ++ )) || true
	    break;
	fi
    done
    [ $sleeptime -gt $MONITOR_TIMEOUT ] && break
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $num -eq 0 ] ; then
    error_exit "$(date) no DASDs have been reserved after $sleeptime seconds"
fi
logger "$num DASDs reserved"

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Wait for $IO_TIMEOUT seconds"
sleep $IO_TIMEOUT

for devno in ${DEVNOS_LEFT} ; do
    echo "$(date) Release DASD $devno on left half ..."
    result=$(run_wget ${devno} Release)
    if [ "$result" ] ; then
	echo "$(date) Reserve DASD $devno failed; wget returned:"
	echo "$result"
	exit 1
    fi
    break;
done
echo "$(date) Wait for reservations to be cleared ..."
sleeptime=0
num=${#DASDS_LEFT[@]}
while [ $num -gt 0 ] ; do
    num=0
    for d in ${DASDS_LEFT[@]}; do
	state=$(tunedasd -Q /dev/$d)
	if [ "$state" = "other" ] ; then
	    (( num ++ )) || true
	fi
    done
    [ $num -eq 0 ] && break
    [ $sleeptime -gt $MONITOR_TIMEOUT ] && break
    num=${#DASDS_LEFT[@]}
    sleep 1
    (( sleeptime ++ )) || true
done
if [ $num -gt 0 ] ; then
    error_exit "$(date) $num DASDs still reserved after $sleeptime seconds"
fi
logger "All DASDs released"

wait_for_md_running $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail /dev/${MD_NUM}

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Wait for sync ..."
wait_for_sync ${MD_NUM} || \
    error_exit "Failed to synchronize array"

mdadm --detail /dev/${MD_NUM}

logger "${MD_NAME}: success"

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
