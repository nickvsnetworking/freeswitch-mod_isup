include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_isup

if HAVE_OSMO_SIGTRAN
mod_LTLIBRARIES = mod_isup.la
mod_isup_la_SOURCES  = isup_codec.c isup_param.c isup_sm.c isup_cgm.c \
                       isup_segment.c isup_map.c isup_trace.c isup_m3ua.c \
                       bearer_mgcp.c bearer_megaco.c mod_isup.c
mod_isup_la_CFLAGS   = $(AM_CFLAGS)
# _GNU_SOURCE: the osmo headers (osmocom/core/timer.h) use clockid_t / struct
# timezone, which glibc only exposes with a POSIX/GNU feature-test macro. The
# osmo-linked TUs don't include switch.h, so set it here rather than rely on the
# FreeSWITCH build's -std.
mod_isup_la_CPPFLAGS = -D_GNU_SOURCE $(LIBOSMO_SIGTRAN_CFLAGS) $(AM_CPPFLAGS)
mod_isup_la_LIBADD   = $(LIBOSMO_SIGTRAN_LIBS) -losmovty -losmocore -ltalloc \
                       $(switch_builddir)/libfreeswitch.la
mod_isup_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
else
install: error
all: error
error:
	$(error You must install libosmo-sigtran-dev to build mod_isup)
endif
