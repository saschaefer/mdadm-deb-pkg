#!/bin/sh -eu
#
# mdadm-name-generator.sh -- determines the md node name to be used by udev
#
# This script determines which naming convention is expected for a given md
# device name, passed in as udev kernel name (%k; e.g. md1). It will output
# the name to use relative to /dev/.
#
# It currently looks at /etc/fstab, /etrc/crypttab, and the output of
# pvdisplay (LVM). If it cannot determine the name, it falls back to the
# default: /dev/mdX.
#
# Intended use is in udev rules:
#
# KERNEL=="md[0-9]*", PROGRAM="/etc/udev/scripts/mdadm-name-generator.sh %k", \
#   NAME="%c", SYMLINK=""
# KERNEL=="md_d[0-9]*", PROGRAM="/etc/udev/scripts/mdadm-name-generator.sh %k", \
#   NAME="%c", SYMLINK=""
#
# Copyright © martin f. krafft <madduck@madduck.net>
# distributed under the terms of the Artistic Licence 2.0
#
# $Id: mkconf 89 2006-08-08 09:33:05Z madduck $
#

case "$1" in
  md*) :;;
  '')
    echo "E: missing device name argument." >&2
    exit 1
    ;;
  *)
    echo "E: invalid device name: $1" >&2
    exit 2
    ;;
esac

KNAME="$1"

PVS=$(pvdisplay 2>/dev/null || :)

add_file()
{
  [ -f "$1" ] && EXTRAFILES="${EXTRAFILES:-$EXTRAFILES }$1" || :
}

EXTRAFILES=""
add_file /etc/fstab
add_file /etc/crypttab

DEVFS_KNAME="${KNAME#md}"; DEVFS_KNAME="${DEVFS_KNAME#_}"
if echo $PVS | eval grep -q "/dev/md/$DEVFS_KNAME" - "$EXTRAFILES"; then
  # devfs-style naming expected
  echo "md/${DEVFS_KNAME}"
  exit 0
fi

echo "$KNAME"
exit 0
