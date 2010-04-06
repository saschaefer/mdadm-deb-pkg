/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 *
 *    Added by Dale Stephenson
 *    steph@snapserver.com
 */

#include	"mdadm.h"
#include	"md_u.h"
#include	"md_p.h"

int Kill(char *dev, struct supertype *st, int force, int quiet, int noexcl)
{
	/*
	 * Nothing fancy about Kill.  It just zeroes out a superblock
	 * Definitely not safe.
	 * Returns:
	 *  0 - a zero superblock was successfully written out
	 *  1 - failed to write the zero superblock
	 *  2 - failed to open the device or find a superblock.
	 */

	int fd, rv = 0;

	if (force)
		noexcl = 1;
	fd = open(dev, O_RDWR|(noexcl ? 0 : O_EXCL));
	if (fd < 0) {
		if (!quiet)
			fprintf(stderr, Name ": Couldn't open %s for write - not zeroing\n",
				dev);
		return 2;
	}
	if (st == NULL)
		st = guess_super(fd);
	if (st == NULL) {
		if (!quiet)
			fprintf(stderr, Name ": Unrecognised md component device - %s\n", dev);
		close(fd);
		return 2;
	}
	rv = st->ss->load_super(st, fd, dev);
	if (force && rv >= 2)
		rv = 0; /* ignore bad data in superblock */
	if (rv== 0 || (force && rv >= 2)) {
		st->ss->free_super(st);
		st->ss->init_super(st, NULL, 0, "", NULL, NULL);
		if (st->ss->store_super(st, fd)) {
			if (!quiet)
				fprintf(stderr, Name ": Could not zero superblock on %s\n",
					dev);
			rv = 1;
		} else if (rv) {
			if (!quiet)
				fprintf(stderr, Name ": superblock zeroed anyway\n");
			rv = 0;
		}
	}
	close(fd);
	return rv;
}
