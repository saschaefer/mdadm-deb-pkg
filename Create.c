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
 */

#include "mdadm.h"
#include	"md_u.h"
#include	"md_p.h"
#include	<ctype.h>

static int default_layout(struct supertype *st, int level, int verbose)
{
	int layout = UnSet;

	if (st && st->ss->default_layout)
		layout = st->ss->default_layout(level);

	if (layout == UnSet)
		switch(level) {
		default: /* no layout */
			layout = 0;
			break;
		case 10:
			layout = 0x102; /* near=2, far=1 */
			if (verbose > 0)
				fprintf(stderr,
					Name ": layout defaults to n1\n");
			break;
		case 5:
		case 6:
			layout = map_name(r5layout, "default");
			if (verbose > 0)
				fprintf(stderr,
					Name ": layout defaults to %s\n", map_num(r5layout, layout));
			break;
		case LEVEL_FAULTY:
			layout = map_name(faultylayout, "default");

			if (verbose > 0)
				fprintf(stderr,
					Name ": layout defaults to %s\n", map_num(faultylayout, layout));
			break;
		}

	return layout;
}


int Create(struct supertype *st, char *mddev,
	   int chunk, int level, int layout, unsigned long long size, int raiddisks, int sparedisks,
	   char *name, char *homehost, int *uuid,
	   int subdevs, mddev_dev_t devlist,
	   int runstop, int verbose, int force, int assume_clean,
	   char *bitmap_file, int bitmap_chunk, int write_behind, int delay, int autof)
{
	/*
	 * Create a new raid array.
	 *
	 * First check that necessary details are available
	 * (i.e. level, raid-disks)
	 *
	 * Then check each disk to see what might be on it
	 * and report anything interesting.
	 *
	 * If anything looks odd, and runstop not set,
	 * abort.
	 *
	 * SET_ARRAY_INFO and ADD_NEW_DISK, and
	 * if runstop==run, or raiddisks disks were used,
	 * RUN_ARRAY
	 */
	int mdfd;
	unsigned long long minsize=0, maxsize=0;
	char *mindisc = NULL;
	char *maxdisc = NULL;
	int dnum;
	mddev_dev_t dv;
	int fail=0, warn=0;
	struct stat stb;
	int first_missing = subdevs * 2;
	int second_missing = subdevs * 2;
	int missing_disks = 0;
	int insert_point = subdevs * 2; /* where to insert a missing drive */
	int total_slots;
	int pass;
	int vers;
	int rv;
	int bitmap_fd;
	int have_container = 0;
	int container_fd = -1;
	int need_mdmon = 0;
	unsigned long long bitmapsize;
	struct mdinfo info, *infos;
	int did_default = 0;
	int do_default_layout = 0;
	unsigned long safe_mode_delay = 0;
	char chosen_name[1024];
	struct map_ent *map = NULL;
	unsigned long long newsize;

	int major_num = BITMAP_MAJOR_HI;

	memset(&info, 0, sizeof(info));

	if (level == UnSet) {
		/* "ddf" and "imsm" metadata only supports one level - should possibly
		 * push this into metadata handler??
		 */
		if (st && (st->ss == &super_ddf || st->ss == &super_imsm))
			level = LEVEL_CONTAINER;
	}

	if (level == UnSet) {
		fprintf(stderr,
			Name ": a RAID level is needed to create an array.\n");
		return 1;
	}
	if (raiddisks < 4 && level == 6) {
		fprintf(stderr,
			Name ": at least 4 raid-devices needed for level 6\n");
		return 1;
	}
	if (raiddisks > 256 && level == 6) {
		fprintf(stderr,
			Name ": no more than 256 raid-devices supported for level 6\n");
		return 1;
	}
	if (raiddisks < 2 && level >= 4) {
		fprintf(stderr,
			Name ": at least 2 raid-devices needed for level 4 or 5\n");
		return 1;
	}
	if (level <= 0 && sparedisks) {
		fprintf(stderr,
			Name ": This level does not support spare devices\n");
		return 1;
	}

	if (subdevs == 1 && strcmp(devlist->devname, "missing") != 0) {
		/* If given a single device, it might be a container, and we can
		 * extract a device list from there
		 */
		mdu_array_info_t inf;
		int fd;

		memset(&inf, 0, sizeof(inf));
		fd = open(devlist->devname, O_RDONLY);
		if (fd >= 0 &&
		    ioctl(fd, GET_ARRAY_INFO, &inf) == 0 &&
		    inf.raid_disks == 0) {
			/* yep, looks like a container */
			if (st) {
				rv = st->ss->load_super(st, fd,
							devlist->devname);
				if (rv == 0)
					have_container = 1;
			} else {
				st = guess_super(fd);
				if (st && !(rv = st->ss->
					    load_super(st, fd,
						       devlist->devname)))
					have_container = 1;
				else
					st = NULL;
			}
			if (have_container) {
				subdevs = raiddisks;
				first_missing = subdevs * 2;
				second_missing = subdevs * 2;
				insert_point = subdevs * 2;
			}
		}
		if (fd >= 0)
			close(fd);
	}
	if (st && st->ss->external && sparedisks) {
		fprintf(stderr,
			Name ": This metadata type does not support "
			"spare disks are create time\n");
		return 1;
	}
	if (subdevs > raiddisks+sparedisks) {
		fprintf(stderr, Name ": You have listed more devices (%d) than are in the array(%d)!\n", subdevs, raiddisks+sparedisks);
		return 1;
	}
	if (!have_container && subdevs < raiddisks+sparedisks) {
		fprintf(stderr, Name ": You haven't given enough devices (real or missing) to create this array\n");
		return 1;
	}
	if (bitmap_file && level <= 0) {
		fprintf(stderr, Name ": bitmaps not meaningful with level %s\n",
			map_num(pers, level)?:"given");
		return 1;
	}

	/* now set some defaults */


	if (layout == UnSet) {
		do_default_layout = 1;
		layout = default_layout(st, level, verbose);
	}

	if (level == 10)
		/* check layout fits in array*/
		if ((layout&255) * ((layout>>8)&255) > raiddisks) {
			fprintf(stderr, Name ": that layout requires at least %d devices\n",
				(layout&255) * ((layout>>8)&255));
			return 1;
		}

	switch(level) {
	case 4:
	case 5:
	case 10:
	case 6:
	case 0:
	case LEVEL_LINEAR: /* linear */
		if (chunk == 0) {
			chunk = 64;
			if (verbose > 0)
				fprintf(stderr, Name ": chunk size defaults to 64K\n");
		}
		break;
	case 1:
	case LEVEL_FAULTY:
	case LEVEL_MULTIPATH:
	case LEVEL_CONTAINER:
		if (chunk) {
			chunk = 0;
			if (verbose > 0)
				fprintf(stderr, Name ": chunk size ignored for this level\n");
		}
		break;
	default:
		fprintf(stderr, Name ": unknown level %d\n", level);
		return 1;
	}
	
	if (size && chunk)
		size &= ~(unsigned long long)(chunk - 1);
	newsize = size * 2;
	if (st && ! st->ss->validate_geometry(st, level, layout, raiddisks,
					      chunk, size*2, NULL, &newsize, verbose>=0))
		return 1;
	if (size == 0) {
		size = newsize / 2;
		if (size && verbose > 0)
			fprintf(stderr, Name ": setting size to %lluK\n",
				(unsigned long long)size);
	}

	/* now look at the subdevs */
	info.array.active_disks = 0;
	info.array.working_disks = 0;
	dnum = 0;
	for (dv=devlist; dv && !have_container; dv=dv->next, dnum++) {
		char *dname = dv->devname;
		unsigned long long freesize;
		if (strcasecmp(dname, "missing")==0) {
			if (first_missing > dnum)
				first_missing = dnum;
			if (second_missing > dnum && dnum > first_missing)
				second_missing = dnum;
			missing_disks ++;
			continue;
		}
		info.array.working_disks++;
		if (dnum < raiddisks)
			info.array.active_disks++;
		if (st == NULL) {
			struct createinfo *ci = conf_get_create_info();
			if (ci)
				st = ci->supertype;
		}
		if (st == NULL) {
			/* Need to choose a default metadata, which is different
			 * depending on geometry of array.
			 */
			int i;
			char *name = "default";
			for(i=0; !st && superlist[i]; i++) {
				st = superlist[i]->match_metadata_desc(name);
				if (do_default_layout)
					layout = default_layout(st, level, verbose);
				if (st && !st->ss->validate_geometry
					    	(st, level, layout, raiddisks,
						 chunk, size*2, dname, &freesize,
						 verbose > 0))
					st = NULL;
			}

			if (!st) {
				fprintf(stderr, Name ": device %s not suitable "
					"for any style of array\n",
					dname);
				exit(2);
			}
			if (st->ss != &super0 ||
			    st->minor_version != 90)
				did_default = 1;
		} else {
			if (do_default_layout)
				layout = default_layout(st, level, verbose);
			if (!st->ss->validate_geometry(st, level, layout,
						       raiddisks,
						       chunk, size*2, dname,
						       &freesize,
						       verbose > 0)) {

				fprintf(stderr,
					Name ": %s is not suitable for "
					"this array.\n",
					dname);
				fail = 1;
				continue;
			}
		}

		freesize /= 2; /* convert to K */
		if (chunk) {
			/* round to chunk size */
			freesize = freesize & ~(chunk-1);
		}

		if (size && freesize < size) {
			fprintf(stderr, Name ": %s is smaller than given size."
				" %lluK < %lluK + metadata\n",
				dname, freesize, size);
			fail = 1;
			continue;
		}
		if (maxdisc == NULL || (maxdisc && freesize > maxsize)) {
			maxdisc = dname;
			maxsize = freesize;
		}
		if (mindisc ==NULL || (mindisc && freesize < minsize)) {
			mindisc = dname;
			minsize = freesize;
		}
		if (runstop != 1 || verbose >= 0) {
			int fd = open(dname, O_RDONLY);
			if (fd <0 ) {
				fprintf(stderr, Name ": Cannot open %s: %s\n",
					dname, strerror(errno));
				fail=1;
				continue;
			}
			warn |= check_ext2(fd, dname);
			warn |= check_reiser(fd, dname);
			warn |= check_raid(fd, dname);
			close(fd);
		}
	}
	if (have_container)
		info.array.working_disks = raiddisks;
	if (fail) {
		fprintf(stderr, Name ": create aborted\n");
		return 1;
	}
	if (size == 0) {
		if (mindisc == NULL && !have_container) {
			fprintf(stderr, Name ": no size and no drives given - aborting create.\n");
			return 1;
		}
		if (level > 0 || level == LEVEL_MULTIPATH
		    || level == LEVEL_FAULTY
		    || st->ss->external ) {
			/* size is meaningful */
			if (!st->ss->validate_geometry(st, level, layout,
						       raiddisks,
						       chunk, minsize*2,
						       NULL, NULL, 0)) {
				fprintf(stderr, Name ": devices too large for RAID level %d\n", level);
				return 1;
			}
			size = minsize;
			if (verbose > 0)
				fprintf(stderr, Name ": size set to %lluK\n", size);
		}
	}
	if (!have_container && level > 0 && ((maxsize-size)*100 > maxsize)) {
		if (runstop != 1 || verbose >= 0)
			fprintf(stderr, Name ": largest drive (%s) exceeds size (%lluK) by more than 1%%\n",
				maxdisc, size);
		warn = 1;
	}

	if (st->ss->detail_platform && st->ss->detail_platform(0, 1) != 0) {
		if (runstop != 1 || verbose >= 0)
			fprintf(stderr, Name ": %s unable to enumerate platform support\n"
				"    array may not be compatible with hardware/firmware\n",
				st->ss->name);
		warn = 1;
	}

	if (warn) {
		if (runstop!= 1) {
			if (!ask("Continue creating array? ")) {
				fprintf(stderr, Name ": create aborted.\n");
				return 1;
			}
		} else {
			if (verbose > 0)
				fprintf(stderr, Name ": creation continuing despite oddities due to --run\n");
		}
	}

	/* If this is raid4/5, we want to configure the last active slot
	 * as missing, so that a reconstruct happens (faster than re-parity)
	 * FIX: Can we do this for raid6 as well?
	 */
	if (st->ss->external == 0 &&
	    assume_clean==0 && force == 0 && first_missing >= raiddisks) {
		switch ( level ) {
		case 4:
		case 5:
			insert_point = raiddisks-1;
			sparedisks++;
			info.array.active_disks--;
			missing_disks++;
			break;
		default:
			break;
		}
	}
	/* For raid6, if creating with 1 missing drive, make a good drive
	 * into a spare, else the create will fail
	 */
	if (assume_clean == 0 && force == 0 && first_missing < raiddisks &&
	    st->ss->external == 0 &&
	    second_missing >= raiddisks && level == 6) {
		insert_point = raiddisks - 1;
		if (insert_point == first_missing)
			insert_point--;
		sparedisks ++;
		info.array.active_disks--;
		missing_disks++;
	}

	if (level <= 0 && first_missing < subdevs * 2) {
		fprintf(stderr,
			Name ": This level does not support missing devices\n");
		return 1;
	}

	/* We need to create the device */
	map_lock(&map);
	mdfd = create_mddev(mddev, name, autof, LOCAL, chosen_name);
	if (mdfd < 0)
		return 1;
	mddev = chosen_name;

	vers = md_get_version(mdfd);
	if (vers < 9000) {
		fprintf(stderr, Name ": Create requires md driver version 0.90.0 or later\n");
		goto abort;
	} else {
		mdu_array_info_t inf;
		memset(&inf, 0, sizeof(inf));
		ioctl(mdfd, GET_ARRAY_INFO, &inf);
		if (inf.working_disks != 0) {
			fprintf(stderr, Name ": another array by this name"
				" is already running.\n");
			goto abort;
		}
	}

	/* Ok, lets try some ioctls */

	info.array.level = level;
	info.array.size = size;
	info.array.raid_disks = raiddisks;
	/* The kernel should *know* what md_minor we are dealing
	 * with, but it chooses to trust me instead. Sigh
	 */
	info.array.md_minor = 0;
	if (fstat(mdfd, &stb)==0)
		info.array.md_minor = minor(stb.st_rdev);
	info.array.not_persistent = 0;

	if ( ( (level == 4 || level == 5) &&
	       (insert_point < raiddisks || first_missing < raiddisks) )
	     ||
	     ( level == 6 && (insert_point < raiddisks
			      || second_missing < raiddisks))
	     ||
	     ( level <= 0 )
	     ||
	     assume_clean
		) {
		info.array.state = 1; /* clean, but one+ drive will be missing*/
		info.resync_start = ~0ULL;
	} else {
		info.array.state = 0; /* not clean, but no errors */
		info.resync_start = 0;
	}
	if (level == 10) {
		/* for raid10, the bitmap size is the capacity of the array,
		 * which is array.size * raid_disks / ncopies;
		 * .. but convert to sectors.
		 */
		int ncopies = ((layout>>8) & 255) * (layout & 255);
		bitmapsize = (unsigned long long)size * raiddisks / ncopies * 2;
/*		printf("bms=%llu as=%d rd=%d nc=%d\n", bitmapsize, size, raiddisks, ncopies);*/
	} else
		bitmapsize = (unsigned long long)size * 2;

	/* There is lots of redundancy in these disk counts,
	 * raid_disks is the most meaningful value
	 *          it describes the geometry of the array
	 *          it is constant
	 * nr_disks is total number of used slots.
	 *          it should be raid_disks+spare_disks
	 * spare_disks is the number of extra disks present
	 *          see above
	 * active_disks is the number of working disks in
	 *          active slots. (With raid_disks)
	 * working_disks is the total number of working disks,
	 *          including spares
	 * failed_disks is the number of disks marked failed
	 *
         * Ideally, the kernel would keep these (except raid_disks)
	 * up-to-date as we ADD_NEW_DISK, but it doesn't (yet).
	 * So for now, we assume that all raid and spare
	 * devices will be given.
	 */
	info.array.spare_disks=sparedisks;
	info.array.failed_disks=missing_disks;
	info.array.nr_disks = info.array.working_disks
		+ info.array.failed_disks;
	info.array.layout = layout;
	info.array.chunk_size = chunk*1024;

	if (name == NULL || *name == 0) {
		/* base name on mddev */
		/*  /dev/md0 -> 0
		 *  /dev/md_d0 -> d0
		 *  /dev/md/1 -> 1
		 *  /dev/md/d1 -> d1
		 *  /dev/md/home -> home
		 *  /dev/mdhome -> home
		 */
		/* FIXME compare this with rules in create_mddev */
		name = strrchr(mddev, '/');
		if (name) {
			name++;
			if (strncmp(name, "md_d", 4)==0 &&
			    strlen(name) > 4 &&
			    isdigit(name[4]) &&
			    (name-mddev) == 5 /* /dev/ */)
				name += 3;
			else if (strncmp(name, "md", 2)==0 &&
				 strlen(name) > 2 &&
				 isdigit(name[2]) &&
				 (name-mddev) == 5 /* /dev/ */)
				name += 2;
		}
	}
	if (!st->ss->init_super(st, &info.array, size, name, homehost, uuid))
		goto abort;

	total_slots = info.array.nr_disks;
	sysfs_init(&info, mdfd, 0);
	st->ss->getinfo_super(st, &info);

	if (did_default && verbose >= 0) {
		if (is_subarray(info.text_version)) {
			int dnum = devname2devnum(info.text_version+1);
			char *path;
			int mdp = get_mdp_major();
			struct mdinfo *mdi;
			if (dnum > 0)
				path = map_dev(MD_MAJOR, dnum, 1);
			else
				path = map_dev(mdp, (-1-dnum)<< 6, 1);

			mdi = sysfs_read(-1, dnum, GET_VERSION);

			fprintf(stderr, Name ": Creating array inside "
				"%s container %s\n", 
				mdi?mdi->text_version:"managed", path);
			sysfs_free(mdi);
		} else
			fprintf(stderr, Name ": Defaulting to version"
				" %s metadata\n", info.text_version);
	}

	map_update(&map, fd2devnum(mdfd), info.text_version,
		   info.uuid, chosen_name);
	map_unlock(&map);

	if (bitmap_file && vers < 9003) {
		major_num = BITMAP_MAJOR_HOSTENDIAN;
#ifdef __BIG_ENDIAN
		fprintf(stderr, Name ": Warning - bitmaps created on this kernel are not portable\n"
			"  between different architectured.  Consider upgrading the Linux kernel.\n");
#endif
	}

	if (bitmap_file && strcmp(bitmap_file, "internal")==0) {
		if ((vers%100) < 2) {
			fprintf(stderr, Name ": internal bitmaps not supported by this kernel.\n");
			goto abort;
		}
		if (!st->ss->add_internal_bitmap(st, &bitmap_chunk,
						 delay, write_behind,
						 bitmapsize, 1, major_num)) {
			fprintf(stderr, Name ": Given bitmap chunk size not supported.\n");
			goto abort;
		}
		bitmap_file = NULL;
	}


	sysfs_init(&info, mdfd, 0);

	if (st->ss->external && st->subarray[0]) {
		/* member */

		/* When creating a member, we need to be careful
		 * to negotiate with mdmon properly.
		 * If it is already running, we cannot write to
		 * the devices and must ask it to do that part.
		 * If it isn't running, we write to the devices,
		 * and then start it.
		 * We hold an exclusive open on the container
		 * device to make sure mdmon doesn't exit after
		 * we checked that it is running.
		 *
		 * For now, fail if it is already running.
		 */
		container_fd = open_dev_excl(st->container_dev);
		if (container_fd < 0) {
			fprintf(stderr, Name ": Cannot get exclusive "
				"open on container - weird.\n");
			goto abort;
		}
		if (mdmon_running(st->container_dev)) {
			if (verbose)
				fprintf(stderr, Name ": reusing mdmon "
					"for %s.\n",
					devnum2devname(st->container_dev));
			st->update_tail = &st->updates;
		} else
			need_mdmon = 1;
	}
	rv = set_array_info(mdfd, st, &info);
	if (rv) {
		fprintf(stderr, Name ": failed to set array info for %s: %s\n",
			mddev, strerror(errno));
		goto abort;
	}

	if (bitmap_file) {
		int uuid[4];

		st->ss->uuid_from_super(st, uuid);
		if (CreateBitmap(bitmap_file, force, (char*)uuid, bitmap_chunk,
				 delay, write_behind,
				 bitmapsize,
				 major_num)) {
			goto abort;
		}
		bitmap_fd = open(bitmap_file, O_RDWR);
		if (bitmap_fd < 0) {
			fprintf(stderr, Name ": weird: %s cannot be openned\n",
				bitmap_file);
			goto abort;
		}
		if (ioctl(mdfd, SET_BITMAP_FILE, bitmap_fd) < 0) {
			fprintf(stderr, Name ": Cannot set bitmap file for %s: %s\n",
				mddev, strerror(errno));
			goto abort;
		}
	}

	infos = malloc(sizeof(*infos) * total_slots);

	for (pass=1; pass <=2 ; pass++) {
		mddev_dev_t moved_disk = NULL; /* the disk that was moved out of the insert point */

		for (dnum=0, dv = devlist ; dv ;
		     dv=(dv->next)?(dv->next):moved_disk, dnum++) {
			int fd;
			struct stat stb;
			struct mdinfo *inf = &infos[dnum];

			if (dnum >= total_slots)
				abort();
			if (dnum == insert_point) {
				moved_disk = dv;
				continue;
			}
			if (strcasecmp(dv->devname, "missing")==0)
				continue;
			if (have_container)
				moved_disk = NULL;
			if (have_container && dnum < info.array.raid_disks - 1)
				/* repeatedly use the container */
				moved_disk = dv;

			switch(pass) {
			case 1:
				*inf = info;

				inf->disk.number = dnum;
				inf->disk.raid_disk = dnum;
				if (inf->disk.raid_disk < raiddisks)
					inf->disk.state = (1<<MD_DISK_ACTIVE) |
						(1<<MD_DISK_SYNC);
				else
					inf->disk.state = 0;

				if (dv->writemostly == 1)
					inf->disk.state |= (1<<MD_DISK_WRITEMOSTLY);

				if (have_container)
					fd = -1;
				else {
					if (st->ss->external && st->subarray[0])
						fd = open(dv->devname, O_RDWR);
					else
						fd = open(dv->devname, O_RDWR|O_EXCL);

					if (fd < 0) {
						fprintf(stderr, Name ": failed to open %s "
							"after earlier success - aborting\n",
							dv->devname);
						goto abort;
					}
					fstat(fd, &stb);
					inf->disk.major = major(stb.st_rdev);
					inf->disk.minor = minor(stb.st_rdev);
				}
				if (fd >= 0)
					remove_partitions(fd);
				if (st->ss->add_to_super(st, &inf->disk,
							 fd, dv->devname))
					goto abort;
				st->ss->getinfo_super(st, inf);
				safe_mode_delay = inf->safe_mode_delay;

				if (have_container && verbose > 0)
					fprintf(stderr, Name ": Using %s for device %d\n",
						map_dev(inf->disk.major,
							inf->disk.minor,
							0), dnum);

				if (!have_container) {
					/* getinfo_super might have lost these ... */
					inf->disk.major = major(stb.st_rdev);
					inf->disk.minor = minor(stb.st_rdev);
				}
				break;
			case 2:
				inf->errors = 0;
				rv = 0;

				rv = add_disk(mdfd, st, &info, inf);

				if (rv) {
					fprintf(stderr,
						Name ": ADD_NEW_DISK for %s "
						"failed: %s\n",
						dv->devname, strerror(errno));
					st->ss->free_super(st);
					goto abort;
				}
				break;
			}
			if (!have_container &&
			    dv == moved_disk && dnum != insert_point) break;
		}
		if (pass == 1) {
			st->ss->write_init_super(st);
			flush_metadata_updates(st);
		}
	}
	free(infos);
	st->ss->free_super(st);

	if (level == LEVEL_CONTAINER) {
		/* No need to start.  But we should signal udev to
		 * create links */
		sysfs_uevent(&info, "change");
		if (verbose >= 0)
			fprintf(stderr, Name ": container %s prepared.\n", mddev);
		wait_for(chosen_name, mdfd);
	} else if (runstop == 1 || subdevs >= raiddisks) {
		if (st->ss->external) {
			switch(level) {
			case LEVEL_LINEAR:
			case LEVEL_MULTIPATH:
			case 0:
				sysfs_set_str(&info, NULL, "array_state",
					      "active");
				need_mdmon = 0;
				break;
			default:
				sysfs_set_str(&info, NULL, "array_state",
					      "readonly");
				break;
			}
			sysfs_set_safemode(&info, safe_mode_delay);
		} else {
			/* param is not actually used */
			mdu_param_t param;
			if (ioctl(mdfd, RUN_ARRAY, &param)) {
				fprintf(stderr, Name ": RUN_ARRAY failed: %s\n",
					strerror(errno));
				Manage_runstop(mddev, mdfd, -1, 0);
				goto abort;
			}
		}
		if (verbose >= 0)
			fprintf(stderr, Name ": array %s started.\n", mddev);
		if (st->ss->external && st->subarray[0]) {
			if (need_mdmon)
				start_mdmon(st->container_dev);

			ping_monitor(devnum2devname(st->container_dev));
			close(container_fd);
		}
		wait_for(chosen_name, mdfd);
	} else {
		fprintf(stderr, Name ": not starting array - not enough devices.\n");
	}
	close(mdfd);
	return 0;

 abort:
	if (mdfd >= 0)
		close(mdfd);
	return 1;
}
