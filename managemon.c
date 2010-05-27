/*
 * mdmon - monitor external metadata arrays
 *
 * Copyright (C) 2007-2009 Neil Brown <neilb@suse.de>
 * Copyright (C) 2007-2009 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * The management thread for monitoring active md arrays.
 * This thread does things which might block such as memory
 * allocation.
 * In particular:
 *
 * - Find out about new arrays in this container.
 *   Allocate the data structures and open the files.
 *
 *   For this we watch /proc/mdstat and find new arrays with
 *   metadata type that confirms sharing. e.g. "md4"
 *   When we find a new array we slip it into the list of
 *   arrays and signal 'monitor' by writing to a pipe.
 *
 * - Respond to reshape requests by allocating new data structures
 *   and opening new files.
 *
 *   These come as a change to raid_disks.  We allocate a new
 *   version of the data structures and slip it into the list.
 *   'monitor' will notice and release the old version.
 *   Changes to level, chunksize, layout.. do not need re-allocation.
 *   Reductions in raid_disks don't really either, but we handle
 *   them the same way for consistency.
 *
 * - When a device is added to the container, we add it to the metadata
 *   as a spare.
 *
 * - Deal with degraded array
 *    We only do this when first noticing the array is degraded.
 *    This can be when we first see the array, when sync completes or
 *    when recovery completes.
 *
 *    Check if number of failed devices suggests recovery is needed, and
 *    skip if not.
 *    Ask metadata to allocate a spare device
 *    Add device as not in_sync and give a role
 *    Update metadata.
 *    Open sysfs files and pass to monitor.
 *    Make sure that monitor Starts recovery....
 *
 * - Pass on metadata updates from external programs such as
 *   mdadm creating a new array.
 *
 *   This is most-messy.
 *   It might involve adding a new array or changing the status of
 *   a spare, or any reconfig that the kernel doesn't get involved in.
 *
 *   The required updates are received via a named pipe.  There will
 *   be one named pipe for each container. Each message contains a
 *   sync marker: 0x5a5aa5a5, A byte count, and the message.  This is
 *   passed to the metadata handler which will interpret and process it.
 *   For 'DDF' messages are internal data blocks with the leading
 *   'magic number' signifying what sort of data it is.
 *
 */

/*
 * We select on /proc/mdstat and the named pipe.
 * We create new arrays or updated version of arrays and slip
 * them into the head of the list, then signal 'monitor' via a pipe write.
 * 'monitor' will notice and place the old array on a return list.
 * Metadata updates are placed on a queue just like they arrive
 * from the named pipe.
 *
 * When new arrays are found based on correct metadata string, we
 * need to identify them with an entry in the metadata.  Maybe we require
 * the metadata to be mdX/NN  when NN is the index into an appropriate table.
 *
 */

/*
 * List of tasks:
 * - Watch for spares to be added to the container, and write updated
 *   metadata to them.
 * - Watch for new arrays using this container, confirm they match metadata
 *   and if so, start monitoring them
 * - Watch for spares being added to monitored arrays.  This shouldn't
 *   happen, as we should do all the adding.  Just remove them.
 * - Watch for change in raid-disks, chunk-size, etc.  Update metadata and
 *   start a reshape.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include	"mdadm.h"
#include	"mdmon.h"
#include	<sys/syscall.h>
#include	<sys/socket.h>
#include	<signal.h>

static void close_aa(struct active_array *aa)
{
	struct mdinfo *d;

	for (d = aa->info.devs; d; d = d->next) {
		close(d->recovery_fd);
		close(d->state_fd);
	}

	close(aa->action_fd);
	close(aa->info.state_fd);
	close(aa->resync_start_fd);
}

static void free_aa(struct active_array *aa)
{
	/* Note that this doesn't close fds if they are being used
	 * by a clone.  ->container will be set for a clone
	 */
	dprintf("%s: devnum: %d\n", __func__, aa->devnum);
	if (!aa->container)
		close_aa(aa);
	while (aa->info.devs) {
		struct mdinfo *d = aa->info.devs;
		aa->info.devs = d->next;
		free(d);
	}
	free(aa);
}

static struct active_array *duplicate_aa(struct active_array *aa)
{
	struct active_array *newa = malloc(sizeof(*newa));
	struct mdinfo **dp1, **dp2;

	*newa = *aa;
	newa->next = NULL;
	newa->replaces = NULL;
	newa->info.next = NULL;

	dp2 = &newa->info.devs;

	for (dp1 = &aa->info.devs; *dp1; dp1 = &(*dp1)->next) {
		struct mdinfo *d;
		if ((*dp1)->state_fd < 0)
			continue;

		d = malloc(sizeof(*d));
		*d = **dp1;
		*dp2 = d;
		dp2 = & d->next;
	}
	*dp2 = NULL;

	return newa;
}

static void wakeup_monitor(void)
{
	/* tgkill(getpid(), mon_tid, SIGUSR1); */
	int pid = getpid();
	syscall(SYS_tgkill, pid, mon_tid, SIGUSR1);
}

static void remove_old(void)
{
	if (discard_this) {
		discard_this->next = NULL;
		free_aa(discard_this);
		if (pending_discard == discard_this)
			pending_discard = NULL;
		discard_this = NULL;
		wakeup_monitor();
	}
}

static void replace_array(struct supertype *container,
			  struct active_array *old,
			  struct active_array *new)
{
	/* To replace an array, we add it to the top of the list
	 * marked with ->replaces to point to the original.
	 * 'monitor' will take the original out of the list
	 * and put it on 'discard_this'.  We take it from there
	 * and discard it.
	 */
	remove_old();
	while (pending_discard) {
		while (discard_this == NULL)
			sleep(1);
		remove_old();
	}
	pending_discard = old;
	new->replaces = old;
	new->next = container->arrays;
	container->arrays = new;
	wakeup_monitor();
}

struct metadata_update *update_queue = NULL;
struct metadata_update *update_queue_handled = NULL;
struct metadata_update *update_queue_pending = NULL;

static void free_updates(struct metadata_update **update)
{
	while (*update) {
		struct metadata_update *this = *update;

		*update = this->next;
		free(this->buf);
		free(this->space);
		free(this);
	}
}

void check_update_queue(struct supertype *container)
{
	free_updates(&update_queue_handled);

	if (update_queue == NULL &&
	    update_queue_pending) {
		update_queue = update_queue_pending;
		update_queue_pending = NULL;
		wakeup_monitor();
	}
}

static void queue_metadata_update(struct metadata_update *mu)
{
	struct metadata_update **qp;

	qp = &update_queue_pending;
	while (*qp)
		qp = & ((*qp)->next);
	*qp = mu;
}

static void add_disk_to_container(struct supertype *st, struct mdinfo *sd)
{
	int dfd;
	char nm[20];
	struct supertype *st2;
	struct metadata_update *update = NULL;
	struct mdinfo info;
	mdu_disk_info_t dk = {
		.number = -1,
		.major = sd->disk.major,
		.minor = sd->disk.minor,
		.raid_disk = -1,
		.state = 0,
	};

	dprintf("%s: add %d:%d to container\n",
		__func__, sd->disk.major, sd->disk.minor);

	sd->next = st->devs;
	st->devs = sd;

	sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
	dfd = dev_open(nm, O_RDWR);
	if (dfd < 0)
		return;

	/* Check the metadata and see if it is already part of this
	 * array
	 */
	st2 = dup_super(st);
	if (st2->ss->load_super(st2, dfd, NULL) == 0) {
		st2->ss->getinfo_super(st, &info);
		if (st->ss->compare_super(st, st2) == 0 &&
		    info.disk.raid_disk >= 0) {
			/* Looks like a good member of array.
			 * Just accept it.
			 * mdadm will incorporate any parts into
			 * active arrays.
			 */
			st2->ss->free_super(st2);
			return;
		}
	}
	st2->ss->free_super(st2);

	st->update_tail = &update;
	st->ss->add_to_super(st, &dk, dfd, NULL);
	st->ss->write_init_super(st);
	queue_metadata_update(update);
	st->update_tail = NULL;
}

static void manage_container(struct mdstat_ent *mdstat,
			     struct supertype *container)
{
	/* The only thing of interest here is if a new device
	 * has been added to the container.  We add it to the
	 * array ignoring any metadata on it.
	 * FIXME should we look for compatible metadata and take hints
	 * about spare assignment.... probably not.
	 */
	if (mdstat->devcnt != container->devcnt) {
		struct mdinfo **cdp, *cd, *di, *mdi;
		int found;

		/* read /sys/block/NAME/md/dev-??/block/dev to find out
		 * what is there, and compare with container->info.devs
		 * To see what is removed and what is added.
		 * These need to be remove from, or added to, the array
		 */
		mdi = sysfs_read(-1, mdstat->devnum, GET_DEVS|SKIP_GONE_DEVS);
		if (!mdi) {
			/* invalidate the current count so we can try again */
			container->devcnt = -1;
			return;
		}

		/* check for removals */
		for (cdp = &container->devs; *cdp; ) {
			found = 0;
			for (di = mdi->devs; di; di = di->next)
				if (di->disk.major == (*cdp)->disk.major &&
				    di->disk.minor == (*cdp)->disk.minor) {
					found = 1;
					break;
				}
			if (!found) {
				cd = *cdp;
				*cdp = (*cdp)->next;
				free(cd);
			} else
				cdp = &(*cdp)->next;
		}

		/* check for additions */
		for (di = mdi->devs; di; di = di->next) {
			for (cd = container->devs; cd; cd = cd->next)
				if (di->disk.major == cd->disk.major &&
				    di->disk.minor == cd->disk.minor)
					break;
			if (!cd) {
				struct mdinfo *newd = malloc(sizeof(*newd));

				if (!newd) {
					container->devcnt = -1;
					continue;
				}
				*newd = *di;
				add_disk_to_container(container, newd);
			}
		}
		sysfs_free(mdi);
		container->devcnt = mdstat->devcnt;
	}
}

static void manage_member(struct mdstat_ent *mdstat,
			  struct active_array *a)
{
	/* Compare mdstat info with known state of member array.
	 * We do not need to look for device state changes here, that
	 * is dealt with by the monitor.
	 *
	 * We just look for changes which suggest that a reshape is
	 * being requested.
	 * Unfortunately decreases in raid_disks don't show up in
	 * mdstat until the reshape completes FIXME.
	 *
	 * Actually, we also want to handle degraded arrays here by
	 * trying to find and assign a spare.
	 * We do that whenever the monitor tells us too.
	 */
	// FIXME
	a->info.array.raid_disks = mdstat->raid_disks;
	a->info.array.chunk_size = mdstat->chunk_size;
	// MORE

	if (a->check_degraded) {
		struct metadata_update *updates = NULL;
		struct mdinfo *newdev = NULL;
		struct active_array *newa;
		struct mdinfo *d;

		a->check_degraded = 0;

		/* The array may not be degraded, this is just a good time
		 * to check.
		 */
		newdev = a->container->ss->activate_spare(a, &updates);
		if (!newdev)
			return;

		newa = duplicate_aa(a);
		if (!newa)
			goto out;
		/* Cool, we can add a device or several. */

		/* Add device to array and set offset/size/slot.
		 * and open files for each newdev */
		for (d = newdev; d ; d = d->next) {
			struct mdinfo *newd;

			newd = malloc(sizeof(*newd));
			if (!newd)
				continue;
			if (sysfs_add_disk(&newa->info, d, 0) < 0) {
				free(newd);
				continue;
			}
			*newd = *d;
			newd->next = newa->info.devs;
			newa->info.devs = newd;

			newd->state_fd = sysfs_open(a->devnum, newd->sys_name,
						    "state");
			newd->prev_state = read_dev_state(newd->state_fd);
			newd->curr_state = newd->prev_state;
		}
		queue_metadata_update(updates);
		updates = NULL;
		replace_array(a->container, a, newa);
		sysfs_set_str(&a->info, NULL, "sync_action", "recover");
 out:
		while (newdev) {
			d = newdev->next;
			free(newdev);
			newdev = d;
		}
		free_updates(&updates);
	}
}

static int aa_ready(struct active_array *aa)
{
	struct mdinfo *d;
	int level = aa->info.array.level;

	for (d = aa->info.devs; d; d = d->next)
		if (d->state_fd < 0)
			return 0;

	if (aa->info.state_fd < 0)
		return 0;

	if (level > 0 && (aa->action_fd < 0 || aa->resync_start_fd < 0))
		return 0;

	if (!aa->container)
		return 0;

	return 1;
}

static void manage_new(struct mdstat_ent *mdstat,
		       struct supertype *container,
		       struct active_array *victim)
{
	/* A new array has appeared in this container.
	 * Hopefully it is already recorded in the metadata.
	 * Check, then create the new array to report it to
	 * the monitor.
	 */

	struct active_array *new;
	struct mdinfo *mdi, *di;
	char *inst;
	int i;
	int failed = 0;

	/* check if array is ready to be monitored */
	if (!mdstat->active)
		return;

	mdi = sysfs_read(-1, mdstat->devnum,
			 GET_LEVEL|GET_CHUNK|GET_DISKS|GET_COMPONENT|
			 GET_DEGRADED|GET_DEVS|GET_OFFSET|GET_SIZE|GET_STATE);

	new = malloc(sizeof(*new));

	if (!new || !mdi) {
		if (mdi)
			sysfs_free(mdi);
		if (new)
			free(new);
		return;
	}
	memset(new, 0, sizeof(*new));

	new->devnum = mdstat->devnum;
	strcpy(new->info.sys_name, devnum2devname(new->devnum));

	new->prev_state = new->curr_state = new->next_state = inactive;
	new->prev_action= new->curr_action= new->next_action= idle;

	new->container = container;

	inst = &mdstat->metadata_version[10+strlen(container->devname)+1];

	new->info.array = mdi->array;
	new->info.component_size = mdi->component_size;

	for (i = 0; i < new->info.array.raid_disks; i++) {
		struct mdinfo *newd = malloc(sizeof(*newd));

		for (di = mdi->devs; di; di = di->next)
			if (i == di->disk.raid_disk)
				break;

		if (di && newd) {
			memcpy(newd, di, sizeof(*newd));

			newd->state_fd = sysfs_open(new->devnum,
						    newd->sys_name,
						    "state");
			newd->recovery_fd = sysfs_open(new->devnum,
						      newd->sys_name,
						      "recovery_start");

			newd->prev_state = read_dev_state(newd->state_fd);
			newd->curr_state = newd->prev_state;
		} else {
			if (newd)
				free(newd);

			failed++;
			if (failed > new->info.array.failed_disks) {
				/* we cannot properly monitor without all working disks */
				new->container = NULL;
				break;
			}
			continue;
		}
		sprintf(newd->sys_name, "rd%d", i);
		newd->next = new->info.devs;
		new->info.devs = newd;
	}

	new->action_fd = sysfs_open(new->devnum, NULL, "sync_action");
	new->info.state_fd = sysfs_open(new->devnum, NULL, "array_state");
	new->resync_start_fd = sysfs_open(new->devnum, NULL, "resync_start");
	new->metadata_fd = sysfs_open(new->devnum, NULL, "metadata_version");
	dprintf("%s: inst: %d action: %d state: %d\n", __func__, atoi(inst),
		new->action_fd, new->info.state_fd);

	sysfs_free(mdi);

	/* if everything checks out tell the metadata handler we want to
	 * manage this instance
	 */
	if (!aa_ready(new) || container->ss->open_new(container, new, inst) < 0) {
		fprintf(stderr, "mdmon: failed to monitor %s\n",
			mdstat->metadata_version);
		new->container = NULL;
		free_aa(new);
	} else {
		replace_array(container, victim, new);
		if (failed) {
			new->check_degraded = 1;
			manage_member(mdstat, new);
		}
	}
}

void manage(struct mdstat_ent *mdstat, struct supertype *container)
{
	/* We have just read mdstat and need to compare it with
	 * the known active arrays.
	 * Arrays with the wrong metadata are ignored.
	 */

	for ( ; mdstat ; mdstat = mdstat->next) {
		struct active_array *a;
		if (mdstat->devnum == container->devnum) {
			manage_container(mdstat, container);
			continue;
		}
		if (!is_container_member(mdstat, container->devname))
			/* Not for this array */
			continue;
		/* Looks like a member of this container */
		for (a = container->arrays; a; a = a->next) {
			if (mdstat->devnum == a->devnum) {
				if (a->container)
					manage_member(mdstat, a);
				break;
			}
		}
		if (a == NULL || !a->container)
			manage_new(mdstat, container, a);
	}
}

static void handle_message(struct supertype *container, struct metadata_update *msg)
{
	/* queue this metadata update through to the monitor */

	struct metadata_update *mu;

	if (msg->len <= 0)
		while (update_queue_pending || update_queue) {
			check_update_queue(container);
			usleep(15*1000);
		}

	if (msg->len == 0) { /* ping_monitor */
		int cnt;
		
		cnt = monitor_loop_cnt;
		if (cnt & 1)
			cnt += 2; /* wait until next pselect */
		else
			cnt += 3; /* wait for 2 pselects */
		wakeup_monitor();

		while (monitor_loop_cnt - cnt < 0)
			usleep(10 * 1000);
	} else if (msg->len == -1) { /* ping_manager */
		struct mdstat_ent *mdstat = mdstat_read(1, 0);

		manage(mdstat, container);
		free_mdstat(mdstat);
	} else if (!sigterm) {
		mu = malloc(sizeof(*mu));
		mu->len = msg->len;
		mu->buf = msg->buf;
		msg->buf = NULL;
		mu->space = NULL;
		mu->next = NULL;
		if (container->ss->prepare_update)
			container->ss->prepare_update(container, mu);
		queue_metadata_update(mu);
	}
}

void read_sock(struct supertype *container)
{
	int fd;
	struct metadata_update msg;
	int terminate = 0;
	long fl;
	int tmo = 3; /* 3 second timeout before hanging up the socket */

	fd = accept(container->sock, NULL, NULL);
	if (fd < 0)
		return;

	fl = fcntl(fd, F_GETFL, 0);
	fl |= O_NONBLOCK;
	fcntl(fd, F_SETFL, fl);

	do {
		msg.buf = NULL;

		/* read and validate the message */
		if (receive_message(fd, &msg, tmo) == 0) {
			handle_message(container, &msg);
			if (ack(fd, tmo) < 0)
				terminate = 1;
		} else
			terminate = 1;

	} while (!terminate);

	close(fd);
}

int exit_now = 0;
int manager_ready = 0;
void do_manager(struct supertype *container)
{
	struct mdstat_ent *mdstat;
	sigset_t set;

	sigprocmask(SIG_UNBLOCK, NULL, &set);
	sigdelset(&set, SIGUSR1);
	sigdelset(&set, SIGTERM);

	do {

		if (exit_now)
			exit(0);

		/* Can only 'manage' things if 'monitor' is not making
		 * structural changes to metadata, so need to check
		 * update_queue
		 */
		if (update_queue == NULL) {
			mdstat = mdstat_read(1, 0);

			manage(mdstat, container);

			read_sock(container);

			free_mdstat(mdstat);
		}
		remove_old();

		check_update_queue(container);

		manager_ready = 1;

		if (sigterm)
			wakeup_monitor();

		if (update_queue == NULL)
			mdstat_wait_fd(container->sock, &set);
		else
			/* If an update is happening, just wait for signal */
			pselect(0, NULL, NULL, NULL, NULL, &set);
	} while(1);
}
