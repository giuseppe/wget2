DEFS = @DEFS@ -DSYSCONFDIR=\"$(sysconfdir)/@PACKAGE@\" -DLOCALEDIR=\"$(localedir)\"

bin_PROGRAMS = wget2
wget2_SOURCES = bar.c bar.h blacklist.c blacklist.h host.c host.h job.c job.h log.c log.h\
 wget.c wget.h options.c options.h compat_fnmatch.c compat_fnmatch.h
wget2_CPPFLAGS = -I$(top_srcdir)/include
wget2_LDADD = ../libwget/libwget-@LIBWGET_API_VERSION@.la @LTLIBICONV@ @LTLIBINTL@
wget2_LDFLAGS = -static

#x_SOURCES = test_linking.c
#x_CPPFLAGS = -I$(top_srcdir)/include
#x_LDADD = ../libwget/libwget.la
#x_LDFLAGS = -static

#css_tokenizer.c: $(srcdir)/css_tokenizer.lex css_tokenizer.h
#	$(LEX) -o $@ $<
