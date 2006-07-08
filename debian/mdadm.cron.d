#
# cron.d/mdadm -- schedules periodic parity checks of RAID devices
#
# Copyright © 2006 martin f. krafft <madduck@madduck.net>
# distributed under the terms of the Artistic Licence.
#

# by default, run at 01:06 on the first Sunday of each month.
6 1 1-7 * 7 root [ -x /usr/share/mdadm/checkarray ] && /usr/share/mdadm/checkarray --cron --all --quiet
