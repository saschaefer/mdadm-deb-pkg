.PHONY: 00list
00list:
	find -maxdepth 1 -type f -name \*.dpatch -print \
		| sed -re 's,^\./(.*)\.dpatch$$,\1,' | sort -n > $@
