#
# cron.d/mdadm -- schedules periodic parity checks of RAID devices
#
# Copyright © martin f. krafft <madduck@madduck.net>
# distributed under the terms of the Artistic Licence 2.0
#
# Revision: $Id$
#

# By default, run at 01:06 on the every Sunday, but do nothing unless the day
# of the month is less than or equal to 7. Thus, only run on the first Sunday
# of each month. crontab(5) sucks, unfortunately, so therefore this hack
# (see #380425).
6 1 * * 0 root [ -x /usr/share/mdadm/checkarray ] && [ $(date +\%d) -le 7 ] && /usr/share/mdadm/checkarray --cron --all --quiet
