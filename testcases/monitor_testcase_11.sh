#!/bin/bash
#
# Testcase 11: multiple array start & shutdown
#

set -o errexit

. $(dirname "$0")/monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase11"
NUM_STEPS=8

stop_md $MD_NUM

activate_dasds 16

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM} 8

logger "${MD_NAME}: Multiple Array startup/shutdown"

sleep 1
echo -n "MonitorStatus on /dev/${MD_NUM}: "
MD_LOG1="/tmp/monitor_${MD_NAME}_step1.log"
md_monitor -c"MonitorStatus:/dev/${MD_NUM}" | tee ${MD_LOG1}

n=4
devlist=
while [ $n -lt 7 ] ; do
    devlist="$devlist ${DEVICES_LEFT[$n]} ${DEVICES_RIGHT[$n]}"
    n=$(expr $n + 1)
done

MD2_NAME="md2"
MD_ARGS="--bitmap=internal --chunk=1024 --bitmap-chunk=512K --assume-clean --force"
echo "Create MD array $MD2_NAME ..."
mdadm --create /dev/${MD2_NAME} --name=${MD2_NAME} \
    --raid-devices=6 ${MD_ARGS} --level=raid10 \
    --failfast ${devlist} \
    || error_exit "Cannot create MD array $MD2_NAME."

# stop the extra md in case of failure
function stop_extra_mds_1() {
    mdadm --stop /dev/${MD2_NAME}
}
push_recovery_fn stop_extra_mds_1
wait_md ${MD2_NAME}
MD_LOG2="/tmp/monitor_${MD_NAME}_step2.log"
mdadm --detail /dev/${MD2_NAME} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG2}
mdadm --brief --detail /dev/${MD2_NAME} >> /etc/mdadm.conf
sleep 1
echo -n "MonitorStatus on /dev/${MD2_NAME}: "
MD_LOG3="/tmp/monitor_${MD_NAME}_step3.log"
md_monitor -c"MonitorStatus:/dev/${MD2_NAME}" | tee ${MD_LOG3}
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
    MD3_NAME="md3"
    function stop_extra_mds_2() {
	mdadm --stop /dev/${MD3_NAME}
    }
    echo "Create MD array $MD3_NAME ..."
    mdadm --create /dev/${MD3_NAME} --name=${MD3_NAME} \
	--raid-devices=4 ${MD_ARGS} --level=raid10 \
	--failfast ${devlist} \
	|| error_exit "Cannot create MD array $MD3_NAME."
    (( MD_MAX++ )) || true
    push_recovery_fn stop_extra_mds_2
    wait_md ${MD3_NAME}
    MD_LOG4="/tmp/monitor_${MD_NAME}_step4.log"
    mdadm --detail /dev/${MD3_NAME} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG4}
    mdadm --brief --detail /dev/${MD3_NAME} >> /etc/mdadm.conf
    sleep 1
    echo -n "MonitorStatus on /dev/${MD3_NAME}: "
    MD_LOG5="/tmp/monitor_${MD_NAME}_step5.log"
    md_monitor -c"MonitorStatus:/dev/${MD3_NAME}" | tee ${MD_LOG5}
fi

step=0
while [ $step -lt $NUM_STEPS ] ; do
    MD=$(expr $RANDOM % $MD_MAX) || true
    (( MD++ )) || true
    old_status=$(md_monitor -c"MonitorStatus:/dev/md${MD}")
    echo "Stop MD array md$MD ..."
    mdadm --stop /dev/md${MD}
    sleep 1
    if md_monitor -c"ArrayStatus:/dev/md${MD}" > /dev/null ; then
	error_exit "MD array md${MD} still working"
    fi
    sleeptime=$(expr $RANDOM / 1024 || true)
    echo "Waiting for $sleeptime seconds ..."
    sleep $sleeptime
    echo "Reassemble MD array md$MD ..."
    mdadm --assemble /dev/md${MD} \
	|| error_exit "Cannot assemble MD array md${MD}"
    wait_md md${MD}
    MD_LOG6="/tmp/monitor_${MD_NAME}_step6.log"
    mdadm --detail /dev/md${MD} | sed '/Update Time/D;/Events/D' | tee ${MD_LOG6}
    if [ ${MD} -eq 1 ] ; then
	MD_LOGN=${START_LOG}
    elif [ ${MD} -eq 2 ] ; then
	MD_LOGN=${MD_LOG2}
    elif [ ${MD} -eq 3 ] ; then
	MD_LOGN=${MD_LOG4}
    fi
    if ! diff -u "${MD_LOGN}" "${MD_LOG6}" ; then
	error_exit "Not all devices on md${MD} are working"
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
	new_status=$(md_monitor -c"MonitorStatus:/dev/md${MD}" || true)
	echo "MonitorStatus on /dev/md${MD}: $new_status"
	if [ "$new_status" = "$old_status" ] ; then
	    break;
	fi
	(( wait_time++ )) || true
	sleep 1
    done
    if [ "$new_status" != "$old_status" ] ; then
	error_exit "Monitor information on md${MD} is inconsistent"
    fi
    sleeptime=$(expr $RANDOM / 1024 || true)
    echo "Waiting for $sleeptime seconds ..."
    sleep $sleeptime
    (( step++ )) || true
done

logger "${MD_NAME}: success"

mdadm --stop /dev/${MD2_NAME}
mdadm --stop /dev/${MD3_NAME}

stop_md ${MD_NUM}
