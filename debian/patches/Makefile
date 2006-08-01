.PHONY: 00list
00list:
	find -maxdepth 1 -name Makefile -o -name 00list -prune -o -type f -print \
		| sed -e 's,\./\(.*\)\.dpatch,\1,' | sort -n > $@
