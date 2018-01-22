#!/bin/bash
#
# Testcase 11: multiple array start & shutdown
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NAME="testcase11"

NUM_STEPS=8

stop_md

activate_devices 16

clear_metadata

ulimit -c unlimited

MD1_NAME="${MD_NAME}_1"
start_md ${MD1_NAME} 8
MD1_DEV="/dev/md/${MD1_NAME}"

logger "${MD_NAME}: Multiple Array startup/shutdown"

sleep 1
echo -n "MonitorStatus on ${MD1_DEV}: "
MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"MonitorStatus:${MD1_DEV}" | tee ${MD_LOG1}

n=4
devlist=
while [ $n -lt 7 ] ; do
    devlist="$devlist ${DEVICES_LEFT[$n]} ${DEVICES_RIGHT[$n]}"
    n=$(expr $n + 1)
done

MD2_NAME="${MD_NAME}_2"
MD_ARGS="--bitmap=internal --chunk=1024 --bitmap-chunk=512K --assume-clean --force"
MD2_DEV="/dev/md/${MD2_NAME}"
echo "Create MD array $MD2_NAME ..."
mdadm --create ${MD2_DEV} --name=${MD2_NAME} \
    --raid-devices=6 ${MD_ARGS} --level=raid10 \
    --failfast ${devlist} \
    || error_exit "Cannot create MD array $MD2_NAME."

# stop the extra md in case of failure
function stop_extra_mds_1() {
    mdadm --stop ${MD2_DEV}
}
push_recovery_fn stop_extra_mds_1
wait_md ${MD2_NAME}
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail ${MD2_DEV} | sed -n '/Devices/p' | tee ${MD_LOG2}
mdadm --brief --detail ${MD2_DEV} >> /etc/mdadm.conf
sleep 1
echo -n "MonitorStatus on ${MD2_DEV}: "
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"MonitorStatus:${MD2_DEV}" | tee ${MD_LOG3}
MD_MAX=2
n=7
devlist=
while [ $n -lt 9 ] ; do
    if [ "${DEVICES_LEFT[$n]}" ] ; then
	devlist="$devlist ${DEVICES_LEFT[$n]}"
    fi
    if [ "${DEVICES_RIGHT[$n]}" ] ; then
	devlist="$devlist ${DEVICES_RIGHT[$n]}"
    fi
    n=$(expr $n + 1)
done
if [ -n "$devlist" ] ; then
    MD3_NAME="${MD_NAME}_3"
    MD3_DEV="/dev/md/${MD3_NAME}"
    function stop_extra_mds_2() {
	mdadm --stop ${MD3_DEV}
    }
    echo "Create MD array $MD3_NAME ..."
    mdadm --create ${MD3_DEV} --name=${MD3_NAME} \
	--raid-devices=4 ${MD_ARGS} --level=raid10 \
	--failfast ${devlist} \
	|| error_exit "Cannot create MD array $MD3_NAME."
    (( MD_MAX++ )) || true
    push_recovery_fn stop_extra_mds_2
    wait_md ${MD3_DEV}
    MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
    mdadm --detail ${MD3_DEV} | sed -n '/Devices/p' | tee ${MD_LOG4}
    mdadm --brief --detail ${MD3_DEV} >> /etc/mdadm.conf
    sleep 1
    echo -n "MonitorStatus on ${MD3_NAME}: "
    MD_LOG5="/tmp/monitor_${MD_NAME}_step5.log"
    md_monitor -c"MonitorStatus:${MD3_DEV}" | tee ${MD_LOG5}
fi

step=0
while [ $step -lt $NUM_STEPS ] ; do
    MD=$(expr $RANDOM % $MD_MAX) || true
    (( MD++ )) || true
    MD_TMP_DEV="/dev/md/${MD_NAME}_${MD}"
    old_status=$(md_monitor -c"MonitorStatus:${MD_TMP_DEV}")
    echo "Stop MD array ${MD_TMP_DEV} ..."
    mdadm --stop ${MD_TMP_DEV}
    sleep 1
    if md_monitor -c"ArrayStatus:${MD_TMP_DEV}" > /dev/null ; then
	error_exit "MD array ${MD_TMP_DEV} still working"
    fi
    sleeptime=$(expr $RANDOM / 1024 || true)
    echo "Waiting for $sleeptime seconds ..."
    sleep $sleeptime
    echo "Reassemble MD array ${MD_TMP_DEV} ..."
    mdadm --assemble ${MD_TMP_DEV} \
	|| error_exit "Cannot assemble MD array ${MD_TMP_DEV}"
    wait_md ${MD_TMP_DEV}
    MD_STEP=$(expr $step + 6)
    MD_STEP_LOG="/tmp/monitor_${MD_NAME}_${MD_STEP}.log"
    mdadm --detail ${MD_TMP_DEV} | sed -n '/Devices/p' | tee ${MD_STEP_LOG}
    if [ ${MD} -eq 1 ] ; then
	MD_LOGN=${START_LOG}
    elif [ ${MD} -eq 2 ] ; then
	MD_LOGN=${MD_LOG2}
    elif [ ${MD} -eq 3 ] ; then
	MD_LOGN=${MD_LOG4}
    fi
    if ! diff -u "${MD_LOGN}" "${MD_STEP_LOG}" ; then
	error_exit "Not all devices on ${MD_TMP_DEV} are working"
    fi
    if [ ${MD} -eq 1 ] ; then
	MD_LOGN=${MD_LOG1}
    elif [ ${MD} -eq 2 ] ; then
	MD_LOGN=${MD_LOG3}
    elif [ ${MD} -eq 3 ] ; then
	MD_LOGN=${MD_LOG5}
    fi
    wait_time=0
    MD_TIMEOUT=15
    while [ $wait_time -lt $MD_TIMEOUT ] ; do
	new_status=$(md_monitor -c"MonitorStatus:${MD_TMP_DEV}" || true)
	echo "MonitorStatus on ${MD_TMP_DEV}: $new_status"
	if [ "$new_status" = "$old_status" ] ; then
	    break;
	fi
	(( wait_time++ )) || true
	sleep 1
    done
    if [ "$new_status" != "$old_status" ] ; then
	error_exit "Monitor information on ${MD_TMP_DEV} is inconsistent"
    fi
    sleeptime=$(expr $RANDOM / 1024 || true)
    echo "Waiting for $sleeptime seconds ..."
    sleep $sleeptime
    (( step++ )) || true
done

logger "${MD_NAME}: success"

mdadm --stop ${MD2_DEV}
if [ -n "${MD3_DEV}" ] ; then
    mdadm --stop ${MD3_DEV}
fi

stop_md ${MD1_DEV}
