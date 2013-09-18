#!/bin/bash
#
# Common functions for testcases
#

function devno_from_dasd() {
    local dasd=${1##*/}
    local devpath

    if [ ! -e /sys/block/$dasd ] ; then
	dasd=${dasd%1}
    fi
    if [ ! -L /sys/block/$dasd ] ; then
	echo "No sysfs entry for $1"
	exit 1
    fi
    devpath=$(cd -P /sys/block/${dasd}/device; echo $PWD)
    echo ${devpath##*/}
}

function error_exit() {
    local errstr=$1

    echo "$(date) $errstr"
    exit 1
}

function start_md() {
    local MD_NUM=$1
    local MD_DEVICES=$2
    local MD_MONITOR=/sbin/md_monitor
    local MD_SCRIPT=/usr/share/misc/md_notify_device.sh
    local LOGFILE="/tmp/monitor_${MD_NAME}.log"
    local MD_ARGS="--bitmap=internal --chunk=1024 --assume-clean --force"
    local n=0
    local devlist

    case "$MD_NUM" in
	md*)
	    MD_DEVNAME=/dev/$MD_NUM
	    ;;
	*)
	    MD_NAME=$1
	    MD_NUM=$2
	    MD_DEVICES=$3
	    MD_DEVNAME=/dev/md/$MD_NAME
	    ;;
    esac
    if [ -z "$MD_DEVICES" ] ; then
	MD_DEVICES=$MD_DEVNUM
    fi
    while [ $n -lt $(expr $MD_DEVICES / 2) ] ; do
	devlist="$devlist ${DEVICES_LEFT[$n]} ${DEVICES_RIGHT[$n]}"
	n=$(expr $n + 1)
    done

    echo "Create MD array ..."
    mdadm --create ${MD_DEVNAME} --name=${MD_NAME} \
	--raid-devices=${MD_DEVICES} ${MD_ARGS} --level=raid10 \
	--failfast ${devlist} \
	|| error_exit "Cannot create MD array."

    mdadm --wait ${MD_DEVNAME}
    START_LOG="/tmp/monitor_${MD_NAME}_mdstat_start.log"
    mdadm --detail ${MD_DEVNAME} | sed '/Update Time/D;/Events/D' | tee ${START_LOG}
    echo "POLICY action=re-add" > /etc/mdadm.conf
    echo "AUTO -all" >> /etc/mdadm.conf
    mdadm --brief --detail ${MD_DEVNAME} >> /etc/mdadm.conf
    echo "PROGRAM ${MD_SCRIPT}" >> /etc/mdadm.conf

    rm /var/log/messages
    rcsyslog restart
    MONITOR_PID=$(/sbin/md_monitor -y -p 7 -d -s)
    trapcmd="stop_iotest"
    if [ -n "$MONITOR_PID" ] ; then
	trapcmd="$trapcmd ; ${MD_MONITOR} -c'Shutdown:/dev/console'"
    fi
    MDADM_PID=$(mdadm --monitor --scan --daemonise)
    if [ -n "$MDADM_PID" ] ; then
	trapcmd="$trapcmd ; kill -TERM $MDADM_PID"
    fi
    if [ -n "$trapcmd" ] ; then
	trap "$trapcmd" EXIT
	MDADM_PID=
    fi
    iostat -kt 1 > /tmp/monitor_${MD_NAME}_iostat.log 2>&1 &
}

function stop_md() {
    local md
    local md_detail
    local cur_md=$1

    if ! grep -q ${cur_md} /proc/mdstat ; then
	return
    fi
    STOP_LOG="/tmp/monitor_${MD_NAME}_mdstat_stop.log"
    if [ -n "${START_LOG}" ] ; then
	mdadm --detail /dev/${cur_md} | sed '/Update Time/D;/Events/D' | tee ${STOP_LOG}
	if ! diff -u ${START_LOG} ${STOP_LOG} ; then
	    echo "MD array configuration inconsistent"
	    exit 1
	fi
    fi
    rm -f ${START_LOG} ${STOP_LOG}
    for md in $(sed -n 's/^\(md[0-9]*\) .*/\1/p' /proc/mdstat) ; do
	if [ "$md" = "$cur_md" ] ; then
	    if grep -q /dev/$md /proc/mounts ; then
		echo "Unmounting filesystems ..."
		if ! umount /dev/$md ; then
		    echo "Cannot unmount /dev/$md"
		    exit 1
		fi
	    fi
	    echo "Stopping MD array ..."
	    mdadm --stop /dev/$md
	fi
    done
    cp /var/log/messages /tmp/monitor_${MD_NAME}.log
    rm -f /etc/mdadm.conf
}

function activate_dasds() {
    local devno_max=$1
    local devno;
    local dasd;
    local DEVNO_LEFT_START="0xa000"
    local DEVNO_LEFT_END="0xa0c8"
    local DEVNO_RIGHT_START="0xa100"
    local DEVNO_RIGHT_END="0xa1c8"
    local i=0

    if ! grep -q vmcp /proc/modules ; then
	modprobe vmcp
    fi
    userid=$(vmcp q userid 2> /dev/null | cut -f 1 -d ' ')
    if [ "$userid" = "LINUX025" ] ; then
        # linux025 layout
	DEVNO_LEFT_START="0x0210"
	DEVNO_LEFT_END="0x0217"
	DEVNO_RIGHT_START="0x0220"
	DEVNO_RIGHT_END="0x0227"
    elif [ "$userid" != "LINUX021" ] ; then
	error_exit "Cannot determine DASD layout for $userid"
    fi

    # Use 8 DASDs per side per default
    if [ -z "$devno_max" ] ; then
	devno_max=8
    fi
    devno_start=$((DEVNO_LEFT_START))
    devno_end=$(( devno_start + $devno_max ))
    if [ $devno_end -gt $((DEVNO_LEFT_END)) ] ; then
	devno_end=$((DEVNO_LEFT_END))
    fi
    while [ $devno_start -lt $devno_end ] ; do
	devno=$(printf "0.0.%04x" $devno_start)
	read online < /sys/bus/ccw/devices/$devno/online
	if [ "$online" -ne 1 ] ; then
	    if ! echo 1 > /sys/bus/ccw/devices/$devno/online ; then
		error_exit "Cannot set device $devno online"
	    fi
	    udevadm settle
	fi
	dasd=
	for d in /sys/bus/ccw/devices/$devno/block/* ; do
	    if [ -d "$d" ] ; then
		dasd=${d##*/}
	    fi
	done
	if [ -z "$dasd" ] ; then
	    error_exit "Cannot activate device $devno"
	fi
	read status < /sys/bus/ccw/devices/$devno/status
	if [ "$status" = "unformatted" ] ; then
	    if ! dasdfmt -p -y -b 4096 -f /dev/$dasd ; then
		error_exit "Failed to format $dasd"
	    fi
	    read status < /sys/bus/ccw/devices/$devno/status
	fi
	if [ "$status" != "online" ] ; then
	    error_exit "Failed to activate $dasd"
	fi
	DEVNOS_LEFT="$DEVNOS_LEFT $devno"
	DASDS_LEFT+=("$dasd")
	if [ ! -d /sys/block/${dasd}/${dasd}1 ] || [ -d /sys/block/dasd/${dasd}/${dasd}2 ] ; then
	    if ! fdasd -a /dev/$dasd ; then
		error_exit "Failed to partition $dasd"
	    fi
	fi
	DEVICES_LEFT+=("/dev/${dasd}1")
	(( devno_start++))
    done

    devno_start=$((DEVNO_RIGHT_START))
    devno_end=$(( devno_start + $devno_max ))
    if [ $devno_end -gt $((DEVNO_RIGHT_END)) ] ; then
	devno_end=$((DEVNO_RIGHT_END))
    fi
    while [ $devno_start -lt $devno_end ] ; do
	devno=$(printf "0.0.%04x" $devno_start)
	read online < /sys/bus/ccw/devices/$devno/online
	if [ "$online" -ne 1 ] ; then
	    if ! echo 1 > /sys/bus/ccw/devices/$devno/online ; then
		error_exit "Cannot set device $devno online"
	    fi
	fi
	dasd=
	for d in /sys/bus/ccw/devices/$devno/block/* ; do
	    if [ -d "$d" ] ; then
		dasd=${d##*/}
	    fi
	done
	if [ -z "$dasd" ] ; then
	    error_exit "Cannot activate device $devno"
	fi
	read status < /sys/bus/ccw/devices/$devno/status
	if [ "$status" = "unformatted" ] ; then
	    if ! dasdfmt -p -y -b 4096 -f /dev/$dasd ; then
		error_exit "Failed to format $dasd"
	    fi
	    read status < /sys/bus/ccw/devices/$devno/status
	fi
	if [ "$status" != "online" ] ; then
	    error_exit "Failed to activate $dasd"
	fi
	DEVNOS_RIGHT="$DEVNOS_RIGHT $devno"
	DASDS_RIGHT+=("$dasd")
	if [ ! -d /sys/block/${dasd}/${dasd}1 ] || [ -d /sys/block/dasd/${dasd}/${dasd}2 ] ; then
	    if ! fdasd -a /dev/$dasd ; then
		error_exit "Failed to partition $dasd"
	    fi
	fi
	DEVICES_RIGHT+=("/dev/${dasd}1")
	(( devno_start++))
    done
}

function clear_metadata() {
    echo -n "Clear DASD Metadata ..."
    MD_DEVNUM=0
    for dev in ${DEVICES_LEFT[@]} ${DEVICES_RIGHT[@]} ; do
	[ -b $dev ] || continue
	dd if=/dev/zero of=${dev} bs=4096 count=4096 >/dev/null 2>&1
	echo -n " $dev ..."
	MD_DEVNUM=$(( $MD_DEVNUM + 1 ))
    done
}

function run_dd() {
    local PRG=$1
    local MNT=$2
    local BLKS=$3
    local SIZE
    local CPUS

    if [ "$PRG" = "dt" ] ; then
	CPUS=$(sed -n 's/^# processors *: \([0-9]*\)/\1/p' /proc/cpuinfo)
	(( CPUS * 2 ))
	SIZE=$(( $BLKS * 4096 ))
	${DT_PROG} of=${MNT}/dt.scratch bs=4k incr=var min=4k max=256k errors=1 procs=$CPUS oncerr=abort disable=pstats disable=fsync oflags=trunc errors=1 dispose=keep pattern=iot iotype=random runtime=24h limit=${SIZE} log=/tmp/dt.log > /dev/null 2>&1
    else
	while true ; do
	    dd if=/dev/random of=${MNT}/dd.scratch bs=4k count=${BLKS}
	    dd if=${MNT}/dd.scratch of=/dev/null bs=4k count=${BLKS}
	done
    fi
}

function run_iotest() {
    local MNT=$1
    local DT_PROG
    local CPUS
    local BLKS

    DT_PROG=$(which dt 2> /dev/null)

    BLKS=$(df | sed -n "s/[a-z/]*[0-9]* *[0-9]* *[0-9]* *\([0-9]*\) *[0-9]*% *.*${MNT##*/}/\1/p")
    if [ -z "$BLKS" ] ; then
	echo "Device $MNT not found"
	exit 1
    fi
    BLKS=$(( BLKS >> 3 ))
    if [ -z "$DT_PROG" ] ; then
	run_dd "dd" $MNT $BLKS > /tmp/dt.log 2>&1 &
    else
	run_dd "dt" $MNT $BLKS > /tmp/dt.log 2>&1 &
    fi
    DT_PID=$!
}

function stop_iotest() {
    DT_PROG=$(which dt 2> /dev/null);

    if [ -n "${DT_PID}" ] && kill -TERM ${DT_PID} 2> /dev/null ; then
	echo -n "waiting for ${DT_PROG:-dd} to finish (PID ${DT_PID}) ... "
	wait ${DT_PID} 2> /dev/null
	echo done
    fi
}

function wait_for_sync () {
  echo "waiting for sync...";
  local START_TIME=`date +%s`;
  local ELAPSED_TIME;
  local status;
  local resync_time;
  local MD=$1
  local wait_for_bitmap=$2
  local RAIDLVL
  local raid_status
  local raid_disks=0;
  local working_disks=0;
  local MONITORTIMEOUT=30
  local RESYNCSPEED=4000

  if [ ! -L /sys/block/$MD ] ; then
      md_link=$(readlink /dev/$MD)
      MD=${md_link##*/}
  fi
      
  local RAIDLVL=$(sed -n "s/${MD}.*\(raid[0-9]*\) .*/\1/p" /proc/mdstat)
  if [ -z "$RAIDLVL" ] ; then
      echo "ERROR: array not started"
      return 1
  fi
  cat /proc/mdstat
  mdadm --detail /dev/$MD

  # Check overall status
  raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
  if [ "$raid_status" ] ; then
      raid_disks=${raid_status%/*}
      working_disks=${raid_status#*/}
  fi
  if [ $raid_disks -eq 0 ] ; then
      echo "ERROR: No raid disks on mirror ${MD}"
      return 1
  fi

  # This is tricky
  # Recovery is done in several stages
  # 1. The failed devices are removed
  # 2. The removed devices are re-added
  # 3. Recovery will start
  # 
  # To complicate things any of theses steps
  # might already be done by the time we get
  # around checking for it.
  #
  # So first check if all devices are working
  #
  resync_time=$(sed -n 's/.* finish=\(.*\)min speed.*/\1/p' /proc/mdstat)
  if [ $raid_disks -eq $working_disks ] && [ -z "$resync_time" ] ; then
      # All devices in sync, ok
      echo "All devices in sync"
      return 0
  fi
  action=$(sed -n 's/.* \([a-z]*\) =.*/\1/p' /proc/mdstat)
  if [ "$action" != "reshape" ] ; then
      # Bump resync speed
      echo $RESYNCSPEED > /sys/block/${MD}/md/sync_speed_min
  fi
  # Wait for resync process to be started
  wait_time=0
  while [ $wait_time -lt $MONITORTIMEOUT ] ; do
      resync_time=$(sed -n 's/.* finish=\(.*\)min speed.*/\1/p' /proc/mdstat)
      # Recovery will start as soon as the devices have been re-added
      [ -z "$resync_time" ] || break
      raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
      working_disks=${raid_status#*/}
      # Stop loop if all devices are working
      [ $working_disks -eq $raid_disks ] && break
      sleep 1
      (( wait_time++ ))
  done
  if [ $wait_time -ge $MONITORTIMEOUT ] ; then
      echo "ERROR: recovery didn't start after $MONITORTIMEOUT seconds"
      return 1
  fi
  mdadm --wait /dev/$MD

  if [ "$action" != "reshape" ] ; then
      # Reset sync speed
      echo "system" > /sys/block/${MD}/md/sync_speed_min
  fi
  raid_status=$(sed -n 's/.*\[\([0-9]*\/[0-9]*\)\].*/\1/p' /proc/mdstat)
  if [ -z "$raid_status" ] ; then
      echo "ERROR: no raid disks on mirror $MD"
      return 1;
  fi
  raid_disks=${raid_status%/*}
  working_disks=${raid_status#*/}
  if [ $raid_disks -ne $working_disks ] ; then
      echo "ERROR: mirror $MD degraded after recovery"
      mdadm --detail /dev/$MD
      return 1;
  fi
  if [ "$wait_for_bitmap" ] ; then
      # Waiting for bitmap to clear
      num_pages=1
      wait_time=0
      while [ $wait_time -lt $MONITORTIMEOUT ] ; do
	  num_pages=$(sed -n 's/ *bitmap: \([0-9]*\)\/[0-9]* .*/\1/p' /proc/mdstat)
	  [ $num_pages -eq 0 ] && break
	  sleep 1
	  (( wait_time++ ))
      done
      if [ $wait_time -ge $MONITORTIMEOUT ] ; then
	  echo "bitmap didn't clear after $MONITORTIMEOUT seconds:"
	  cat /proc/mdstat
      fi
  fi

  let ELAPSED_TIME=`date +%s`-$START_TIME;
  echo "sync finished after $ELAPSED_TIME secs";
}
