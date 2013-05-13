#!/bin/bash
#
# Testcase 11: multiple array start & shutdown
#

. ./monitor_testcase_functions.sh

MD_NUM="md1"
MD_NAME="testcase11"

logger "Monitor Testcase 11: Multiple Array startup/shutdown"

stop_md $MD_NUM

activate_dasds 16

clear_metadata

ulimit -c unlimited
start_md ${MD_NUM} 8

md_monitor -c"ArrayStatus:/dev/${MD_NUM}"

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
    --failfast ${devlist}
if [ $? != 0 ] ; then
    error_exit "Cannot create MD array $MD2_NAME."
fi

mdadm --wait /dev/${MD2_NAME}
mdadm --detail /dev/${MD2_NAME}
mdadm --brief --detail /dev/${MD2_NAME} >> /etc/mdadm.conf
sleep 1
md_monitor -c"ArrayStatus:/dev/${MD2_NAME}"

n=7
devlist=
while [ $n -lt 9 ] ; do
    devlist="$devlist ${DEVICES_LEFT[$n]} ${DEVICES_RIGHT[$n]}"
    n=$(expr $n + 1)
done

MD3_NAME="md3"
echo "Create MD array $MD3_NAME ..."
mdadm --create /dev/${MD3_NAME} --name=${MD3_NAME} \
    --raid-devices=4 ${MD_ARGS} --level=raid10 \
    --failfast ${devlist}
if [ $? != 0 ] ; then
    error_exit "Cannot create MD array $MD3_NAME."
fi

mdadm --wait /dev/${MD3_NAME}
mdadm --detail /dev/${MD3_NAME}
mdadm --brief --detail /dev/${MD3_NAME} >> /etc/mdadm.conf
sleep 1
md_monitor -c"ArrayStatus:/dev/${MD3_NAME}"

for MD in md1 md2 md3 md2 md1 md2 md3 md1 md3 ; do
    echo "Stop MD array $MD ..."
    mdadm --stop /dev/${MD}
    sleep 1
    md_monitor -c"MirrorStatus:/dev/${MD}"
    md_monitor -c"MonitorStatus:/dev/${MD}"
    sleep $(expr $RANDOM / 1024)
    echo "Reassemble MD array $MD ..."
    mdadm --assemble /dev/${MD}
    mdadm --wait /dev/${MD}
    sleep 1
    md_monitor -c"MirrorStatus:/dev/${MD}"
    md_monitor -c"MonitorStatus:/dev/${MD}"
    sleep $(expr $RANDOM / 1024)
done

mdadm --stop /dev/${MD2_NAME}
mdadm --stop /dev/${MD3_NAME}

stop_md ${MD_NUM}
