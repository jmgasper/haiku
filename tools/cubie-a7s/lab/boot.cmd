# air/OS lab boot selector for the Radxa Cubie A7S. U-Boot's distro boot
# runs this script from the ESP (partition 2) before it looks for EFI
# binaries there.
#
#   airos/haiku-once     boot Haiku once (the file is deleted before booting)
#   airos/haiku-default  boot Haiku whenever there is no other request
#   otherwise            boot the Debian recovery system on partition 3
#
# A power cycle after a failed Haiku boot therefore lands in Debian unless
# haiku-default exists. On the serial console, stop autoboot and type
# "run airos_linux" or "run airos_haiku" after "setenv airos_dev mmc 0".

setenv airos_dev ${devtype} ${devnum}
setenv airos_esp ${devtype} ${devnum}:${distro_bootpart}
setenv airos_linux 'sysboot ${airos_dev}:3 any ${scriptaddr} /boot/extlinux/extlinux.conf'
setenv airos_haiku 'if load ${airos_dev}:2 ${kernel_addr_r} EFI/airos/haiku_loader.efi; then if load ${airos_dev}:2 ${fdt_addr_r} airos/cubie-a7s.dtb; then bootefi ${kernel_addr_r} ${fdt_addr_r}; else bootefi ${kernel_addr_r} ${fdtcontroladdr}; fi; fi'

setenv airos_target linux
if test -e ${airos_esp} airos/haiku-default; then
	setenv airos_target haiku
fi
if test -e ${airos_esp} airos/haiku-once; then
	fatrm ${airos_esp} airos/haiku-once
	if test -e ${airos_esp} airos/haiku-once; then
		echo "air/OS lab: could not delete airos/haiku-once"
	else
		setenv airos_target haiku
	fi
fi

echo "air/OS lab: booting ${airos_target}"
if test "${airos_target}" = haiku; then
	run airos_haiku
	echo "air/OS lab: Haiku did not start, falling back to Debian"
fi
run airos_linux
