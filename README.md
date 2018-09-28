
# Automatic device failover detection with mdadm and md_monitor

Currently, mdadm detects any I/O failure on a device and will be
setting the affected device(s) to 'faulty'. The MD array is then set
to 'degraded', but continues to work, provided that enough disks for
the given RAID scenarios are present.

The MD array then requires manual interaction to resolve this situation.
1) If the device had a temporary failure (eg connection loss with
   the storage array) it can be re-integrated with the degraded
   MD array.
2) If the device had a permanent failure it would need to be
   replaced with a spare device.

## 1) Automatic device integration after temporary failure

The md_monitor program has been developed to handle case 1), ie
automatic reintegration of a device after a temporary failure.

To facilitate this the md_monitor program monitors each MD array of
the system. For each device which is part of an MD array a checker
thread is started. This checker thread issues a READ request to
sector 0 of the device in regular intervals. Depending on the
current status of the device and the result of the I/O it will then
take corrective action:

a) I/O returned ok:
   - if the MD array has registered the device as 'ready', no
     action is triggered.
   - if the MD array has registered the device as 'faulty', a
     manual array recovery is triggered by executing the command:

  mdadm --manage /dev/mdX --remove /dev/dasdY --re-add /dev/dasdY

b) I/O returned with error:
   - if the MD array has registered the device as 'faulty', no
     action is triggered.
   - if the MD array has registered the device as 'ready', the
     MD array is instructed to fail the device by executing the
     command:

 mdadm --manage /dev/mdX --fail /dev/dasdY

c) I/O does not return after a given timeout:
   - if the number of retries has not been exceeded, re-check
     after waiting one second
   - if the number of retries has been exceeded, treat the
     device as if an I/O error had happened and continue
     as in 1.b)

## 2) Device replacement after a permanent failure

The md_monitor program continues to issue I/O to a device even if
the device is failed. On S/390, the DASD subsystem does not allow
for a device to be deactivated if there is any I/O pending.
So to replace a device after a permanent failure the md_monitor
program has first to be instructed to stop monitoring the device
before it can be replaced.

To do this, the administrator has to issue the command:

/sbin/md_monitor -c "Remove:/dev/mdX@/dev/dasdY"

to instruct md_monitor to stop monitoring device '/dev/dasdY' on MD
array '/dev/mdX'. Then the device can be set to faulty with

mdadm --manage /dev/mdX --fail /dev/dasdY

and removed from the MD array with:

mdadm --manage /dev/mdX --remove /dev/dasdY

The new disk can be added with

mdadm --zero-superblock /dev/dasdZ
mdadm --manage /dev/mdX --add /dev/dasdZ

md_monitor will pick up the changes automatically and start
monitoring the new device.


## 3) Set-up md_monitor: simple setup with systemd

 1. Make sure the number of system asynchronous IO slots is high enough for
`md_monitor` (only necessary on SLE12, with kernel below 4.4.155-94.50.1):

        echo "fs.aio-max-nr=$((1<<20))" >/etc/sysctl.d/99-aio.conf

 2. Set `MDADM_PROGRAM` in `/etc/sysconfig/mdadm`:

        MDADM_PROGRAM="/usr/share/misc/md_notify_device.sh"

 3. Customize the command line options for `md_monitor` in
`/etc/sysconfig/md_monitor` to suit your system's needs.

 4. Enable the `md_monitor` service:

        systemctl enable md_monitor

 5. Reboot to make sure all settings take effect.

## 4) Set-up md_monitor: detailed instructions

Make sure the number of system aio slots is high enough for `md_monitor` (see above).

md_monitor is informed about state changes from MD array either from
uevents or from mdadm in 'monitor' operation.
mdadm needs to be started with

mdadm --monitor --scan --program <MONITOR_SCRIPT>

where <MONITOR_SCRIPT> is a bash script containing the following:

    #!/bin/bash
    # MD monitor script
    #
    
    EVENT=$1
    MD=$2
    DEV=$3
    
    /sbin/md_monitor -c "${EVENT}:${MD}@${DEV}"

Assuming the md_monitor program has been installed under /sbin.
The default monitor script is installed under

/usr/share/misc/md_notify_device.sh


## 5) md_monitor Documentation

md_monitor has the following command-line options:

-d
--daemonize   Start md_monitor in background

-f <file>
--logfile=<file>	Write logging information into <file>
			instead of stdout
-s
--syslog		Write logging information to syslog.

-e <num>
--expires=<num>		Set failfast_expires to <num>

-r <num>
--retries=<num>		Set failfast_retries to <num>

-p <prio>
--log-priority=<prio>	Set logging priority to <num>

-v
--verbose		Increase logging priority

-c <cmd>
--command=<cmd>		Send command <cmd> to daemon

-h
--help			Display usage information

The --command option instructs the program to connect to a already
running md_monitor program and send a pre-defined command. The command
has the following syntax:

<cmd>:<md>(@<dev>)

The following values for <cmd> are recognised. If not specified
otherwise, <md> needs to be the device node of an existing MD array.

Shutdown      	     Shutdown md_monitor;
		     <md> argument should be /dev/console

RebuildStarted	     Rebuild has started on array <md>.

RebuildFinished	     Rebuild has finished on array <md>.

DeviceDisappeared    MD array has been stopped; md_monitor will stop
		     monitoring of the component devices for that
		     array.

Fail		     MD detected a failure on the component
		     device <dev>. md_monitor will re-check the device
		     every 'failfast_timeout' seconds.

Remove		     The component device <dev> has been removed
		     from the MD array <md>. md_monitor will stop
		     monitoring this device.

SpareActive	     MD has integrated the device <dev> into array
		     <md>. md_monitor will re-start monitoring of
		     this device every 'failfast_timeout' seconds.
		     The check interval will be increased for each
		     successful check up to a maximum of
		     'failfast_timeout' * 'failfast_retries' seconds.

	

		
