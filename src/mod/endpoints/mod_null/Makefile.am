include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_null

mod_LTLIBRARIES = mod_null.la
mod_null_la_SOURCES  = mod_null.c
mod_null_la_CFLAGS   = $(AM_CFLAGS)
mod_null_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_null_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
