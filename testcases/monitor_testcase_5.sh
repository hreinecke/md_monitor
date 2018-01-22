#!/bin/bash
#
# Testcase 5: chpid on/off
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase5"
MD_DEV="/dev/md/${MD_NAME}"

MONITOR_TIMEOUT=60

CHPID_LEFT="0.13"
CHPID_RIGHT="0.1b"

detach_other_half=1

userid=$(vmcp q userid | cut -f 1 -d ' ')
if [ -z "$userid" ] ; then
    error_exit "No z/VM userid"
elif [ "$userid" != "LINUX021" -a "$userid" != "LINUX042" ] ; then
    echo "%userid is not configured for this testcase"
    trap - EXIT
    exit 0
fi

stop_md ${MD_DEV}

activate_devices

clear_metadata

ulimit -c unlimited
start_md ${MD_NAME}

logger "${MD_NAME}: Chpid vary on/off"

echo "$(date) Create filesystem ..."
if ! mkfs.ext3 ${MD_DEV} ; then
    error_exit "Cannot create fs"
fi

echo "$(date) Mount filesystem ..."
if ! mount ${MD_DEV} /mnt ; then
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

echo "$(date) vary off chipds for the left side ..."
if [ -n "$DEVNOS_LEFT" ] ; then
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
else
    for shost in ${SHOSTS_LEFT[@]} ; do
	hostpath=$(cd -P /sys/class/scsi_host/$shost; echo $PWD)
	ccwpath=${hostpath%%/host*}
	for chpid in $(cat ${ccwpath}/../chpids) ; do
	    [ "$chpid" = "00" ] && continue
	    logger "Vary off chpid $chpid"
	    chchp -v 0 $chpid || \
		error_exit "Cannot vary off chpid $chpid"
	    push_recovery_fn "chp_vary_on $chpid"
	done
    done
fi

wait_for_md_failed $MONITOR_TIMEOUT

echo "$(date) Wait for 10 seconds"
sleep 10
mdadm --detail ${MD_DEV}

echo "$(date) vary on chpids for the left side"
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

if [ -n "$DEVNOS_LEFT" ] ; then
    # Caveat: 'vmcp vary off' will detach the DASDs
    echo "$(date) re-attach DASDs"
    for dev in /sys/devices/css0/defunct/0.0.* ; do
	[ -e $dev ] || continue
	devno=${dev##*/}
	vmcp att ${devno#0.0.} \* || \
	    error_exit "Cannot attach device ${devno#0.0.}"
    done
fi

wait_for_md_running_left $MONITOR_TIMEOUT

echo "$(date) MD status"
mdadm --detail ${MD_DEV}

echo "$(date) Wait for sync"
wait_for_sync ${MD_DEV} || \
    error_exit "Failed to synchronize array"

check_md_log step1

echo "$(date) vary off on chpids for the right side"
if [ -n "$DEVNOS_LEFT" ] ; then
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
else
    for shost in ${SHOSTS_RIGHT[@]} ; do
	hostpath=$(cd -P /sys/class/scsi_host/$shost; echo $PWD)
	ccwpath=${hostpath%%/host*}
	for chpid in $(cat ${ccwpath}/../chpids) ; do
	    [ "$chpid" = "00" ] && continue
	    logger "Vary off chpid $chpid"
	    chchp -v 0 $chpid || \
		error_exit "Cannot vary off chpid $chpid"
	    push_recovery_fn "chp_vary_on $chpid"
	done
    done
fi

wait_for_md_failed $MONITOR_TIMEOUT

sleep 5
mdadm --detail ${MD_DEV}
ls /mnt
echo "$(date) vary on chpids for the right side"
while true ; do
    if ! pop_recovery_fn ; then
	break;
    fi
done

wait_for_md_running_right $MONITOR_TIMEOUT
    
echo "$(date) Stop I/O test"
stop_iotest

wait_for_sync ${MD_DEV} || \
    error_exit "Failed to synchronize array"

check_md_log step2

logger "${MD_NAME}: success"

echo "$(date) Umount filesystem ..."
umount /mnt

stop_md ${MD_DEV}
