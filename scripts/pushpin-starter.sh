#!/bin/bash -e

USER=pushpin
GROUP=pushpin
TMPDIR=$(/bin/mktemp -d /tmp/pushpin-XXXXXXXX)

do_mount() {
	local mode="$1" src="$2" dst="${TMPDIR}${2}"
	if [ -d "$src" ]; then
		mkdir -p "$dst"
	else
		mkdir -p $(dirname "$dst")
		touch "$dst"
	fi
	mount --bind -o "$mode" "$src" "$dst"
}

MOUNT_RW=( "/var/run/pushpin" "/tmp/pushpin.sock" )
# mounted rw
for m in "${MOUNT_RW[@]}"
do
	do_mount "rw" "${m}"
done
MOUNT_RO=( "/opt/fst-pushpin" "/etc" "/dev" "/usr" "/lib" "/lib64" "/bin" "/sbin" "/proc" )
# mounted ro
for m in "${MOUNT_RO[@]}"
do
	do_mount "ro" "${m}"
done

cleanup () {
	for m in "${MOUNT_RW[@]}" "${MOUNT_RO[@]}"
	do
		umount "${m}"
	done
}

trap cleanup EXIT

exec chroot "$TMPDIR" /opt/fst-pushpin/bin/pushpin-inner.sh
