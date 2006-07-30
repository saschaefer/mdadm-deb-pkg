#
# cron.d/mdadm -- schedules periodic parity checks of RAID devices
#
# Copyright © 2006 martin f. krafft <madduck@madduck.net>
# distributed under the terms of the Artistic Licence.
#

# by default, run at 01:06 on the 5th of every month. I would like to be able
# to limit this down to weekends, but crontab(5) is not able to do so
# (see #380425)
6 1 5 * * root [ -x /usr/share/mdadm/checkarray ] && /usr/share/mdadm/checkarray --cron --all --quiet
