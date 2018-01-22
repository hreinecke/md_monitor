#!/bin/bash
#
# Testcase 15: reserve DASDs w/ root on MD
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

IO_TIMEOUT=10
MONITOR_TIMEOUT=15

logger "Monitor Testcase 15: Reserve DASDs w/ I/O on root on MD"

ulimit -c unlimited

run_wget() {
    local devno=$1
    local action=$2

    result=$(wget -nv --max-redirect=0 "http://s390vsl026.suse.de/dasd.php?ccw=${devno}&action=${action}" 2>&1)

    if [ "$result" != "0 redirections exceeded." ] ; then
	echo $result
	return 1
    fi
    return 0
}

MD_DEV=$(mount | grep ' / ' | cut -d ' ' -f 1)
MD_NAME=${MD_DEV#/dev/}
if [ "${MD_NAME##md}" = "${MD_NAME}" ] ; then
    echo "Testcase can only be run with root on MD"
    exit 0
fi
if [ "${MD_NAME##md/}" != "${MD_NAME}" ] ; then
    MD_NAME=${MD_NAME##md/}
fi

for dasd in $(mdadm --detail ${MD_DEV} | sed -n 's/.*set-A failfast *\/dev\/\(.*\)/\1/p') ; do
    dasd=${dasd%1}
    DASDS_LEFT+=("$dasd")
done
num=0
for dasd in ${DASDS_LEFT[@]}  ; do
    devno=$(lsdasd | sed -n "/[[:space:]]$dasd[[:space:]]/p" | cut -f 1 -d ' ')
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
    (( num++ )) || true
    break;
done

if [ $num -eq 0 ] ; then
    error_exit "No DASDs have been reserved"
fi

echo "$(date) Wait for reservation on left half ..."
sleeptime=0
num=0
while [ $num -eq 0 ] ; do
    for d in ${DASDS_LEFT[@]}; do
	state=$(tunedasd -Q /dev/$d)
	if [ "$state" = "other" ] ; then
	    (( num ++ )) || true
	    logger "reserved DASD /dev/$d"
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

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail ${MD_DEV}

echo "$(date) Wait for $IO_TIMEOUT seconds"
sleep $IO_TIMEOUT

for dasd in ${DASDS_LEFT[@]}  ; do
    devno=$(lsdasd | sed -n "/[[:space:]]$dasd[[:space:]]/p" | cut -f 1 -d ' ')
    echo "$(date) Release DASD $devno on left half ..."
    result=$(run_wget ${devno} Release)
    if [ "$result" ] ; then
	echo "$(date) Release DASD $devno failed; wget returned:"
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

wait_for_md_running_left $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail ${MD_DEV}

echo "$(date) Wait for sync ..."
wait_for_sync ${MD_DEV} || \
    error_exit "Failed to synchronize array"

mdadm --detail ${MD_DEV}

true
