EXTRA_DIST = README.devel

sbin_PROGRAMS = platsch

platsch_SOURCES = platsch.c
platsch_CFLAGS = $(LIBDRM_CFLAGS)
platsch_LDADD = $(LIBDRM_LIBS)

CLEANFILES = \
        $(DIST_ARCHIVES)

DISTCLEAN = \
        config.log \
        config.status \
        Makefile

MAINTAINERCLEANFILES = \
	aclocal.m4 \
	configure \
	depcomp \
	install-sh \
	Makefile.in \
	missing
