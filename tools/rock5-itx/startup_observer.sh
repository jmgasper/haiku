set -e
exec > /dev/dprintf 2>&1
echo ROCK5_BOOT_STATE_OBSERVER_BEGIN
echo ROCK5_BOOT_STATE_SNAPSHOT_0_BEGIN
system_time
ps -as
fdinfo 'launch_daemon'
fdinfo 'app_server'
fdinfo 'net_server'
fdinfo '/bin/sh'
echo ROCK5_BOOT_STATE_SNAPSHOT_0_END
sleep 10
echo ROCK5_BOOT_STATE_SNAPSHOT_1_BEGIN
system_time
ps -as
fdinfo 'launch_daemon'
fdinfo 'app_server'
fdinfo 'net_server'
fdinfo '/bin/sh'
echo ROCK5_BOOT_STATE_SNAPSHOT_1_END
sleep 20
echo ROCK5_BOOT_STATE_SNAPSHOT_2_BEGIN
system_time
ps -as
fdinfo 'launch_daemon'
fdinfo 'app_server'
fdinfo 'net_server'
fdinfo '/bin/sh'
echo ROCK5_BOOT_STATE_SNAPSHOT_2_END
sleep 30
echo ROCK5_BOOT_STATE_SNAPSHOT_3_BEGIN
system_time
ps -as
fdinfo 'launch_daemon'
fdinfo 'app_server'
fdinfo 'net_server'
fdinfo '/bin/sh'
echo ROCK5_BOOT_STATE_SNAPSHOT_3_END
sleep 40
echo ROCK5_BOOT_STATE_SNAPSHOT_4_BEGIN
system_time
ps -as
fdinfo 'launch_daemon'
fdinfo 'app_server'
fdinfo 'net_server'
fdinfo '/bin/sh'
echo ROCK5_BOOT_STATE_SNAPSHOT_4_END
echo ROCK5_BOOT_STATE_OBSERVER_END
