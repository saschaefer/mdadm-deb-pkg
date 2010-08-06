#
# mdadm - manage Linux "md" devices aka RAID arrays.
#
# Copyright (C) 2001-2002 Neil Brown <neilb@cse.unsw.edu.au>
#
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
#    Author: Neil Brown
#    Email: <neilb@cse.unsw.edu.au>
#    Paper: Neil Brown
#           School of Computer Science and Engineering
#           The University of New South Wales
#           Sydney, 2052
#           Australia
#

# define "CXFLAGS" to give extra flags to CC.
# e.g.  make CXFLAGS=-O to optimise
TCC = tcc
UCLIBC_GCC = $(shell for nm in i386-uclibc-linux-gcc i386-uclibc-gcc; do which $$nm > /dev/null && { echo $$nm ; exit; } ; done; echo false No uclibc found )
#DIET_GCC = diet gcc
# sorry, but diet-libc doesn't know about posix_memalign, 
# so we cannot use it any more.
DIET_GCC = gcc -DHAVE_STDINT_H

KLIBC=/home/src/klibc/klibc-0.77

KLIBC_GCC = gcc -nostdinc -iwithprefix include -I$(KLIBC)/klibc/include -I$(KLIBC)/linux/include -I$(KLIBC)/klibc/arch/i386/include -I$(KLIBC)/klibc/include/bits32

CC = $(CROSS_COMPILE)gcc
CXFLAGS = -ggdb
CWFLAGS = -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter
ifdef WARN_UNUSED
CWFLAGS += -Wp,-D_FORTIFY_SOURCE=2 -O
endif

ifdef DEBIAN
CPPFLAGS := -DDEBIAN
else
CPPFLAGS :=
endif
ifdef DEFAULT_OLD_METADATA
 CPPFLAG += -DDEFAULT_OLD_METADATA
 DEFAULT_METADATA=0.90
else
 DEFAULT_METADATA=1.2
endif

SYSCONFDIR = /etc
CONFFILE = $(SYSCONFDIR)/mdadm.conf
CONFFILE2 = $(SYSCONFDIR)/mdadm/mdadm.conf
MAILCMD =/usr/sbin/sendmail -t
CONFFILEFLAGS = -DCONFFILE=\"$(CONFFILE)\" -DCONFFILE2=\"$(CONFFILE2)\"
# Both MAP_DIR and MDMON_DIR should be somewhere that persists across the
# pivotroot from early boot to late boot.
# /dev is an odd place to put this, but it is the only directory that
# meets the requirements.
MAP_DIR=/dev/.mdadm
MAP_FILE = map
MDMON_DIR = /dev/.mdadm
DIRFLAGS = -DMAP_DIR=\"$(MAP_DIR)\" -DMAP_FILE=\"$(MAP_FILE)\"
DIRFLAGS += -DMDMON_DIR=\"$(MDMON_DIR)\"
CFLAGS = $(CWFLAGS) $(CXFLAGS) -DSendmail=\""$(MAILCMD)"\" $(CONFFILEFLAGS) $(DIRFLAGS)

# The glibc TLS ABI requires applications that call clone(2) to set up
# TLS data structures, use pthreads until mdmon implements this support
USE_PTHREADS = 1
ifdef USE_PTHREADS
CFLAGS += -DUSE_PTHREADS
LDFLAGS += -pthread
endif

# If you want a static binary, you might uncomment these
# LDFLAGS = -static
# STRIP = -s

INSTALL = /usr/bin/install
DESTDIR = 
BINDIR  = /sbin
MANDIR  = /usr/share/man
MAN4DIR = $(MANDIR)/man4
MAN5DIR = $(MANDIR)/man5
MAN8DIR = $(MANDIR)/man8

OBJS =  mdadm.o config.o mdstat.o  ReadMe.o util.o Manage.o Assemble.o Build.o \
	Create.o Detail.o Examine.o Grow.o Monitor.o dlink.o Kill.o Query.o \
	Incremental.o \
	mdopen.o super0.o super1.o super-ddf.o super-intel.o bitmap.o \
	restripe.o sysfs.o sha1.o mapfile.o crc32.o sg_io.o msg.o \
	platform-intel.o probe_roms.o

SRCS =  mdadm.c config.c mdstat.c  ReadMe.c util.c Manage.c Assemble.c Build.c \
	Create.c Detail.c Examine.c Grow.c Monitor.c dlink.c Kill.c Query.c \
	Incremental.c \
	mdopen.c super0.c super1.c super-ddf.c super-intel.c bitmap.c \
	restripe.c sysfs.c sha1.c mapfile.c crc32.c sg_io.c msg.c \
	platform-intel.c probe_roms.c

MON_OBJS = mdmon.o monitor.o managemon.o util.o mdstat.o sysfs.o config.o \
	Kill.o sg_io.o dlink.o ReadMe.o super0.o super1.o super-intel.o \
	super-ddf.o sha1.o crc32.o msg.o bitmap.o \
	platform-intel.o probe_roms.o

MON_SRCS = mdmon.c monitor.c managemon.c util.c mdstat.c sysfs.c config.c \
	Kill.c sg_io.c dlink.c ReadMe.c super0.c super1.c super-intel.c \
	super-ddf.c sha1.c crc32.c msg.c bitmap.c \
	platform-intel.c probe_roms.c

STATICSRC = pwgr.c
STATICOBJS = pwgr.o

ASSEMBLE_SRCS := mdassemble.c Assemble.c Manage.c config.c dlink.c util.c \
	super0.c super1.c super-ddf.c super-intel.c sha1.c crc32.c sg_io.c mdstat.c \
	platform-intel.c probe_roms.c sysfs.c
ASSEMBLE_AUTO_SRCS := mdopen.c
ASSEMBLE_FLAGS:= $(CFLAGS) -DMDASSEMBLE
ifdef MDASSEMBLE_AUTO
ASSEMBLE_SRCS += $(ASSEMBLE_AUTO_SRCS)
ASSEMBLE_FLAGS += -DMDASSEMBLE_AUTO
endif

all : mdadm mdmon mdadm.man md.man mdadm.conf.man mdmon.man

everything: all mdadm.static swap_super test_stripe \
	mdassemble mdassemble.auto mdassemble.static mdassemble.man \
	mdadm.Os mdadm.O2
everything-test: all mdadm.static swap_super test_stripe \
	mdassemble.auto mdassemble.static mdassemble.man \
	mdadm.Os mdadm.O2
# mdadm.uclibc and mdassemble.uclibc don't work on x86-64
# mdadm.tcc doesn't work..

mdadm : $(OBJS)
	$(CC) $(LDFLAGS) -o mdadm $(OBJS) $(LDLIBS)

mdadm.static : $(OBJS) $(STATICOBJS)
	$(CC) $(LDFLAGS) -static -o mdadm.static $(OBJS) $(STATICOBJS)

mdadm.tcc : $(SRCS) mdadm.h
	$(TCC) -o mdadm.tcc $(SRCS)

mdadm.klibc : $(SRCS) mdadm.h
	rm -f $(OBJS) 
	$(CC) -nostdinc -iwithprefix include -I$(KLIBC)/klibc/include -I$(KLIBC)/linux/include -I$(KLIBC)/klibc/arch/i386/include -I$(KLIBC)/klibc/include/bits32 $(CFLAGS) $(SRCS)

mdadm.Os : $(SRCS) mdadm.h
	$(CC) -o mdadm.Os $(CFLAGS) $(LDFLAGS) -DHAVE_STDINT_H -Os $(SRCS)

mdadm.O2 : $(SRCS) mdadm.h mdmon.O2
	$(CC) -o mdadm.O2 $(CFLAGS) $(LDFLAGS) -DHAVE_STDINT_H -O2 -D_FORTIFY_SOURCE=2 $(SRCS)

mdmon.O2 : $(MON_SRCS) mdadm.h mdmon.h
	$(CC) -o mdmon.O2 $(CFLAGS) $(LDFLAGS) -DHAVE_STDINT_H -O2 -D_FORTIFY_SOURCE=2 $(MON_SRCS)

# use '-z now' to guarantee no dynamic linker interactions with the monitor thread
mdmon : $(MON_OBJS)
	$(CC) $(LDFLAGS) -z now -o mdmon $(MON_OBJS) $(LDLIBS)
msg.o: msg.c msg.h

test_stripe : restripe.c mdadm.h
	$(CC) $(CXFLAGS) $(LDFLAGS) -o test_stripe -DMAIN restripe.c

mdassemble : $(ASSEMBLE_SRCS) mdadm.h
	rm -f $(OBJS)
	$(DIET_GCC) $(ASSEMBLE_FLAGS) -o mdassemble $(ASSEMBLE_SRCS)  $(STATICSRC)

mdassemble.static : $(ASSEMBLE_SRCS) mdadm.h
	rm -f $(OBJS)
	$(CC) $(LDFLAGS) $(ASSEMBLE_FLAGS) -static -DHAVE_STDINT_H -o mdassemble.static $(ASSEMBLE_SRCS) $(STATICSRC)

mdassemble.auto : $(ASSEMBLE_SRCS) mdadm.h $(ASSEMBLE_AUTO_SRCS)
	rm -f mdassemble.static
	$(MAKE) MDASSEMBLE_AUTO=1 mdassemble.static
	mv mdassemble.static mdassemble.auto

mdassemble.uclibc : $(ASSEMBLE_SRCS) mdadm.h
	rm -f $(OJS)
	$(UCLIBC_GCC) $(ASSEMBLE_FLAGS) -DUCLIBC -DHAVE_STDINT_H -static -o mdassemble.uclibc $(ASSEMBLE_SRCS) $(STATICSRC)

# This doesn't work
mdassemble.klibc : $(ASSEMBLE_SRCS) mdadm.h
	rm -f $(OBJS)
	$(KLIBC_GCC) $(ASSEMBLE_FLAGS) -o mdassemble $(ASSEMBLE_SRCS)

mdadm.8 : mdadm.8.in
	sed -e 's/{DEFAULT_METADATA}/$(DEFAULT_METADATA)/g' mdadm.8.in > mdadm.8

mdadm.man : mdadm.8
	nroff -man mdadm.8 > mdadm.man

mdmon.man : mdmon.8
	nroff -man mdmon.8 > mdmon.man

md.man : md.4
	nroff -man md.4 > md.man

mdadm.conf.man : mdadm.conf.5
	nroff -man mdadm.conf.5 > mdadm.conf.man

mdassemble.man : mdassemble.8
	nroff -man mdassemble.8 > mdassemble.man

$(OBJS) : mdadm.h mdmon.h bitmap.h
$(MON_OBJS) : mdadm.h mdmon.h bitmap.h

sha1.o : sha1.c sha1.h md5.h
	$(CC) $(CFLAGS) -DHAVE_STDINT_H -o sha1.o -c sha1.c

install : mdadm mdmon install-man install-udev
	$(INSTALL) -D $(STRIP) -m 755 mdadm $(DESTDIR)$(BINDIR)/mdadm
	$(INSTALL) -D $(STRIP) -m 755 mdmon $(DESTDIR)$(BINDIR)/mdmon

install-static : mdadm.static install-man
	$(INSTALL) -D $(STRIP) -m 755 mdadm.static $(DESTDIR)$(BINDIR)/mdadm

install-tcc : mdadm.tcc install-man
	$(INSTALL) -D $(STRIP) -m 755 mdadm.tcc $(DESTDIR)$(BINDIR)/mdadm

install-uclibc : mdadm.uclibc install-man
	$(INSTALL) -D $(STRIP) -m 755 mdadm.uclibc $(DESTDIR)$(BINDIR)/mdadm

install-klibc : mdadm.klibc install-man
	$(INSTALL) -D $(STRIP) -m 755 mdadm.klibc $(DESTDIR)$(BINDIR)/mdadm

install-man: mdadm.8 md.4 mdadm.conf.5 mdmon.8
	$(INSTALL) -D -m 644 mdadm.8 $(DESTDIR)$(MAN8DIR)/mdadm.8
	$(INSTALL) -D -m 644 mdmon.8 $(DESTDIR)$(MAN8DIR)/mdmon.8
	$(INSTALL) -D -m 644 md.4 $(DESTDIR)$(MAN4DIR)/md.4
	$(INSTALL) -D -m 644 mdadm.conf.5 $(DESTDIR)$(MAN5DIR)/mdadm.conf.5

install-udev: udev-md-raid.rules
	$(INSTALL) -D -m 644 udev-md-raid.rules $(DESTDIR)/lib/udev/rules.d/64-md-raid.rules

uninstall:
	rm -f $(DESTDIR)$(MAN8DIR)/mdadm.8 $(DESTDIR)$(MAN8DIR)/mdmon.8 $(DESTDIR)$(MAN4DIR)/md.4 $(DESTDIR)$(MAN5DIR)/mdadm.conf.5 $(DESTDIR)$(BINDIR)/mdadm

test: mdadm mdmon test_stripe swap_super
	@echo "Please run 'sh ./test' as root"

clean : 
	rm -f mdadm mdmon $(OBJS) $(MON_OBJS) $(STATICOBJS) core *.man \
	mdadm.tcc mdadm.uclibc mdadm.static *.orig *.porig *.rej *.alt .merge_file_* \
	mdadm.Os mdadm.O2 mdmon.O2 \
	mdassemble mdassemble.static mdassemble.auto mdassemble.uclibc \
	mdassemble.klibc swap_super \
	init.cpio.gz mdadm.uclibc.static test_stripe mdmon \
	mdadm.8

dist : clean
	./makedist

testdist : everything-test clean
	./makedist test

TAGS :
	etags *.h *.c

DISTRO_MAKEFILE := $(wildcard distropkg/Makefile)
ifdef DISTRO_MAKEFILE
include $(DISTRO_MAKEFILE)
endif

