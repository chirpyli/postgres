#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/oraparser
#
# IDENTIFICATION
#    src/backend/oraparser/Makefile
#
# add the file for requirement "SQL PARSER"
#
#-------------------------------------------------------------------------
subdir = src/backend/oraparser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PGFILEDESC = "liboraparser - raw parser for oracle"
NAME = liboraparser

# include our headers first
override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)
rpath =

OBJS = orakeywords.o oragram.o orascan.o liboraparser.o

# where to find ora_gen_keywordlist.pl and subsidiary files
TOOLSDIR = $(top_srcdir)/src/tools
GEN_KEYWORDLIST = $(PERL) -I $(TOOLSDIR) $(TOOLSDIR)/ora_gen_keywordlist.pl
GEN_KEYWORDLIST_DEPS = $(TOOLSDIR)/ora_gen_keywordlist.pl $(TOOLSDIR)/PerfectHash.pm

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib


# See notes in src/backend/oraparser/Makefile about the following two rules
oragram.h: oragram.c
	touch $@

oragram.c: BISONFLAGS += -d
oragram.c: BISON_CHECK_CMD = $(PERL) $(srcdir)/../parser/check_keywords.pl $< $(srcdir)/../../include/oraparser/orakwlist.h

orascan.c: FLEXFLAGS = -CF -p -p
orascan.c: FLEX_NO_BACKUP=yes
orascan.c: FLEX_FIX_WARNING=yes

# Force these dependencies to be known even without dependency info built:
orascan.o oragram.o orakeywords.o liboraparser.o: oragram.h

# generate SQL keyword lookup table to be included into orakeywords*.o.
orakwlist_d.h: $(top_srcdir)/src/include/oraparser/orakwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --extern $<

# Dependencies of orakeywords*.o need to be managed explicitly to make sure
# that you don't get broken parsing code, even in a non-enable-depend build.
orakeywords.o : orakwlist_d.h


distprep: orascan.c oragram.c orakwlist_d.h

# oragram.c, oragram.h, and orascan.c are in the distribution tarball, so they
# are not clean here.
clean distclean: clean-lib
	rm -f lex.backup $(OBJS)

maintainer-clean: clean
	rm -f orascan.c oragram.c orakwlist_d.h
