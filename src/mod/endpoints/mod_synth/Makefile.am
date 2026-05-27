include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_synth

mod_LTLIBRARIES = mod_synth.la
mod_synth_la_SOURCES  = mod_synth.c
mod_synth_la_CFLAGS   = $(AM_CFLAGS)
mod_synth_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_synth_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
