#!/bin/bash
#
# Testcase 12: Successive Disk attach/detach
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase12"
MONITOR_TIMEOUT=60
SLEEPTIME=30

function attach_dasd() {
    local userid=$1
    local devno=$2
    
    if [ "$userid" = "LINUX025" ] ; then
	vmcp link \* ${devno##*.} ${devno##*.} || \
	    error_exit "Cannot link device $devno"
    else
	vmcp att ${devno##*.} \* || \
	    error_exit "Cannot attach device $devno"
    fi
}

function rescan_shost() {
    local shost=$1

    echo '- - -' > /sys/class/scsi_host/${shost}/scan || \
	error_exit "Failed to rescan host ${shost}"
}

stop_md $MD_NUM

activate_devices

clear_metadata

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
fi

ulimit -c unlimited
start_md ${MD_NUM}

logger "${MD_NAME}: Successive Disk detach/attach"

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

MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | sed -n 's/\(.*\) slot [0-9]*\/[0-9]* \(.*\)/\1 \2/p' | tee ${MD_LOG1}

if [ -n "$DEVNOS_LEFT" ] ; then
    for devno in ${DEVNOS_LEFT} ; do
	echo "$(date) Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME

	echo "$(date) Detach left device $devno ..."
	dasd=${devno##*.}
	if ! vmcp det ${dasd} ; then
	    error_exit "Cannot detach DASD ${dasd}"
	fi
	push_recovery_fn "attach_dasd $userid ${dasd}"

	echo "$(date) Waiting for 15 seconds ..."
	sleep 15
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
	echo "$(date) Attach left device $devno ..."
	pop_recovery_fn || \
	    error_exit "Cannot attach DASD ${dasd}"

	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    done
else
    for wwid in ${SCSIID_LEFT} ; do
	echo "$(date) Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME

	echo "$(date) Remove left device $wwid ..."
	paths=$(multipathd -k"show map $wwid topology" | \
	        sed -n 's/.*[0-9]*:[0-9]*:[0-9]*:[0-9]* \(sd[a-z]*\) .*/\1/p')
	for path in ${paths} ; do
	    echo 1 > /sys/block/$path/device/delete || \
		error_exit "Cannot remove device ${path} from ${wwid}"
	done
	recovery_fn=0
	for shost in ${SHOSTS_LEFT[@]} ; do
	    push_recovery_fn "rescan_shost $shost"
	    (( recovery_fn++ )) || true
	done

	echo "$(date) Waiting for 15 seconds ..."
	sleep 15
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
	echo "$(date) Attach left device $wwid ..."
	while [ $recovery_fn -gt 0 ] ; do
	    pop_recovery_fn || \
		error_exit "Cannot rescan devices for ${wwid}"
	    (( recovery_fn-- )) || true
	done

	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    done
fi
echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "Failed to synchronize array"
fi
echo "$(date) mirror synchronized"
check_md_log
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | sed -n 's/\(.*\) slot [0-9]*\/[0-9]* \(.*\)/\1 \2/p' | tee ${MD_LOG2}
if ! diff -pu ${MD_LOG1} ${MD_LOG2} ; then
    stop_iotest
    error_exit "inconsistent md_monitor status after detaching left half"
fi

if [ -n "$DEVNOS_RIGHT" ] ; then
    for devno in ${DEVNOS_RIGHT} ; do
	echo "$(date) Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME
    
	echo "$(date) Detach right device $devno ..."
	dasd=${devno##*.}
	if ! vmcp det ${dasd} ; then
	    error_exit "Cannot detach DASD ${dasd}"
	fi
	push_recovery_fn "attach_dasd $userid ${dasd}"
	echo "$(date) Waiting for 15 seconds ..."
	sleep 15
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
	echo "$(date) Attach right device $devno ..."
	if ! pop_recovery_fn ; then
	    error_exit "Cannot attach DASD ${dasd}"
	fi
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    done
else
    for wwid in ${SCSIID_RIGHT} ; do
	echo "$(date) Waiting for $SLEEPTIME seconds ..."
	sleep $SLEEPTIME

	echo "$(date) Remove right device $wwid ..."
	paths=$(multipathd -k"show map $wwid topology" | \
	        sed -n 's/.*[0-9]*:[0-9]*:[0-9]*:[0-9]* \(sd[a-z]*\) .*/\1/p')
	for path in ${paths} ; do
	    echo 1 > /sys/block/$path/device/delete || \
		error_exit "Cannot remove device ${path} from ${wwid}"
	done
	recovery_fn=0
	for shost in ${SHOSTS_RIGHT[@]} ; do
	    push_recovery_fn "rescan_shost $shost"
	    (( recovery_fn++ )) || true
	done

	echo "$(date) Waiting for 15 seconds ..."
	sleep 15
	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
	echo "$(date) Attach right device $wwid ..."
	while [ $recovery_fn -gt 0 ] ; do
	    pop_recovery_fn || \
		error_exit "Cannot rescan devices for ${wwid}"
	    (( recovery_fn-- )) || true
	done

	md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    done
fi

echo "$(date) Wait for sync"
if ! wait_for_sync ${MD_NUM} ; then
    md_monitor -c"ArrayStatus:/dev/${MD_NUM}"
    stop_iotest
    error_exit "Failed to synchronize array"
fi
echo "$(date) sync finished"
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"ArrayStatus:/dev/${MD_NUM}" | sed -n 's/\(.*\) slot [0-9]*\/[0-9]* \(.*\)/\1 \2/p' | tee ${MD_LOG3}
if ! diff -pu ${MD_LOG1} ${MD_LOG3} ; then
    stop_iotest
    error_exit "inconsistent md monitor status after detaching right half"
fi

logger "${MD_NAME}: success"

echo "$(date) Stop I/O test"
stop_iotest

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_NUM}
