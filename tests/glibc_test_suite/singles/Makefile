include ${ZRT_ROOT}/Makefile.env
#it should use release version of zerovm
ifndef TESTS_ROOT
TESTS_ROOT=src
endif
ifndef MEMMAX
MEMMAX=234217728
endif


OUTPUT_LAST=last_test.output
OUTPUT_FAIL=fail_tests.output
TEST_INCLUDES=$(shell find $(TESTS_ROOT) -name "*.h")
TEST_SOURCES=$(shell find $(TESTS_ROOT) -name "*.c")
TEST_OBJECTS=$(addsuffix .o, $(basename $(TEST_SOURCES) ) )
TEST_MANIFESTS=$(addsuffix .manifest, $(basename $(TEST_SOURCES) ) )
TEST_STDOUTS=$(addsuffix .stdout.std, $(basename $(TEST_SOURCES) ) )
TEST_STDERRS=$(addsuffix .stderr.std, $(basename $(TEST_SOURCES) ) )
TEST_LOG_ZVM=$(addsuffix .zerovm.log, $(basename $(TEST_SOURCES) ) )
TEST_LOG_DEBUG=$(addsuffix .zrtdebug.log, $(basename $(TEST_SOURCES) ) )
TEST_NEXES=$(patsubst %.o, %.nexe, $(TEST_OBJECTS))
TEST_LIST=$(basename $(TEST_SOURCES) )
COUNTER=0

#get pipe from zerovm and parse output
PRINT_ERRCODE=awk '{ \
if (NR == 2) { RETCODE=$$1 } \
if (NR == 5) { \
	if (RETCODE == 0) { RESULT="PASS" } \
	else { RESULT="FAIL" } \
	if ($$0 != "ok") { RESULT="FAIL" } \
print RESULT,"zvmstatus=",$$0 } \
}'

DISPLAY_FAIL=awk '{\
if ( match($$0, "FAIL zvmstatus") != 0 ) {\
	print $$0\
	}\
}'

TESTBASE=
EXCL=

#CFLAGS+=-O2 #optimization
CFLAGS+=-g #debug
CFLAGS+=-std=gnu99
CFLAGS+=-D_GNU_SOURCE
#CFLAGS+=-Werror-implicit-function-declaration
CFLAGS+=-I.. -I. -I../api -Isrc
CFLAGS+=-Iinclude
CFLAGS+=-I$(ZRT_ROOT)/lib

#libzrt should be encluded before generic object file of test, because it contains itself main
#declaration, in case if test source didn't explicitly defined zmain it raises linking error;
#in case if libzrt linking after test object file, then linker is using wrong entry point
#if test was not properly ported to zerovm
#LDFLAGS_ZMAIN+= -L$(ZRT_ROOT)/lib -lzrt

#For rt tests
LDFLAGS+= -lrt
#For dlopen
LDFLAGS+= -ldl
#math
LDFLAGS+= -lm
#crypt
LDFLAGS+= -lcrypt

################ strings CFLAGS
CFLAGS-inl-tester.c = -fno-builtin
CFLAGS-noinl-tester.c = -fno-builtin
CFLAGS-tst-strlen.c = -fno-builtin
CFLAGS-stratcliff.c = -fno-builtin
CFLAGS-test-ffs.c = -fno-builtin
CFLAGS-tst-inlcall.c = -fno-builtin
################ libio CFLAGS
CFLAGS-tst_putwc.c = -DOBJPFX=\"/tmp/\"
################ math CFLAGS
CFLAGS-test-float.c= -fno-inline -ffloat-store -fno-builtin
CFLAGS-test-double.c= -fno-inline -ffloat-store -fno-builtin
CFLAGS-test-ldouble.c= -fno-inline -ffloat-store -fno-builtin
CFLAGS-test-tgmath.c= -fno-builtin
CFLAGS-test-tgmath2.c= -fno-builtin
CFLAGS-test-tgmath-ret.c= -fno-builtin
# CPPFLAGS was in origin math Makefile
CLAGS-test-ifloat.c= -U__LIBC_INTERNAL_MATH_INLINES -D__FAST_MATH__ \
			 -DTEST_FAST_MATH -fno-builtin
CLAGS-test-idouble.c= -U__LIBC_INTERNAL_MATH_INLINES -D__FAST_MATH__ \
			 -DTEST_FAST_MATH -fno-builtin
CLAGS-test-ildoubl.c= -U__LIBC_INTERNAL_MATH_INLINES -D__FAST_MATH__ \
			  -DTEST_FAST_MATH -fno-builtin
################ time CFLAGS
CFLAGS-getdate.c = -fexceptions

# it' needed to be adopted to our test suite
# It's copied as is:
# The following is a hack since we must compile scanf1{5,7}.c without any
# GNU extension.  The latter are needed, though, when internal headers
# are used.  So made sure we see the installed headers first.
CFLAGS-scanf15.c = -I../libio -I../stdlib -I../wcsmbs -I../time -I../string \
		   -I../wctype
CFLAGS-scanf17.c = -I../libio -I../stdlib -I../wcsmbs -I../time -I../string \
		   -I../wctype

################ posix CommandLine
CMDLINE-tst-pcre.c=args=/tmp/PCRE.tests
CMDLINE-tst-boost.c=args=/tmp/BOOST.tests
CMDLINE-bug-getdate1.c=args=/tmp/bug-getdate1-fmt
CMDLINE-tst-rxspencer.c=args=--utf8 /tmp/rxspencer/tests
CMDLINE-bug-glob1.c=args=/tmp

################ libio Environment 
#LOCPATH is set to /lib/locale only for demonstration of setting env var
#by glibc test suite, in anyway envvar was set into default path for localedata 
#that uses glibc, so another localisation tests will work withouth LOCPATH env
ENV-tst-swscanf.c=name=LOCPATH, value=/lib/locale

############### time Environment
ENV-tst-getdate.c=name=DATEMSK, value=/tmp/datemsk {BR}name=TZDIR, value=/tmp/getdate-data
ENV-tst-widetext.c=name=LOCPATH, value=$(common-objpfx)localedata {BR}name=LANGUAGE, value=C
ENV-tst-putenv.c=name=SOMETHING_NOBODY_USES, value=

all: clean $(ZEROVM) $(TEST_NEXES) report

resume:$(ZEROVM) $(TEST_NEXES)

clean:
	@rm -f *.o
	@rm -f $(OUTPUT_LAST)
	@rm -f $(OUTPUT_FAIL)
	@rm -f $(TEST_OBJECTS) 
	@rm -f $(TEST_STDOUTS)
	@rm -f $(TEST_STDERRS)
	@rm -f $(TEST_LOG_ZVM)
	@rm -f $(TEST_NEXES)
	@rm -f $(TEST_MANIFESTS)
	@rm -f $(TEST_LOG_DEBUG)

$(ZEROVM):
	$(error "$(ZEROVM) does not exist")

$(TEST_NEXES): $(TEST_SOURCES) $(TEST_INCLUDES)
#increase test counter
	$(eval COUNTER:=$(shell echo $$(($(COUNTER)+1))) )
#prrepare variables
	$(eval BASENAME:=$(basename $@))
	$(eval NAMEONLY:=$(notdir $(BASENAME)))
	$(eval SPECIFIC_TEST_FLAGS:=$(CFLAGS-$(NAMEONLY).c))
	$(eval SPECIFIC_TEST_CMDLINE:=$(CMDLINE-$(NAMEONLY).c))
	$(eval SPECIFIC_TEST_ENV:=$(ENV-$(NAMEONLY).c))
	@echo -n "TEST RUN $@ "
#compile
	@$(CC) -c -o $(BASENAME).o $(CFLAGS) $(SPECIFIC_TEST_FLAGS) $(BASENAME).c
	@$(CC) -o $@ $(LDFLAGS_ZMAIN) $(BASENAME).o $(LDFLAGS)
#prepare manifest
	@sed s@{OUTFILE}@$(CURDIR)/$(BASENAME)@g ../manifest_glibc_tests.template | \
	 sed s@{ABS_PATH}@$(CURDIR)/@g | \
	 sed s@{MOUNTS_PATH}@$(ZRT_ROOT)/mounts/@g | \
	 sed s@{MEMMAX}@$(MEMMAX)@g > $(CURDIR)/$(BASENAME).manifest
#prepare nvram
	@sed s@{ENVIRONMENT}@"$(SPECIFIC_TEST_ENV)"@g ../nvram_glibc_tests.template | \
	 sed s@{BR}@'\n'@g | \
	 sed s@{COMMAND_LINE}@"$(SPECIFIC_TEST_CMDLINE)"@g > $(CURDIR)/$(BASENAME).nvram

#Run test under zerovm and display returned error code
#$shell should could not be used to get test result from file because it unexpectadly
#using contents of previosly saved test output, and it is the main reason why using awk;
#parsing correct output retrieved via pipe
	@setarch x86_64 -R $(ZEROVM) -M$(CURDIR)/$(BASENAME).manifest \
	| $(PRINT_ERRCODE) > $(OUTPUT_LAST)
	@cat $(OUTPUT_LAST)
#save failed tests to displaytheir at report
	@cat $(OUTPUT_LAST) | awk '{\
if ( match($$0, "FAIL zvmstatus") != 0 ) {\
	print test,$$0\
	}\
}' test="$@" >> $(OUTPUT_FAIL)

report: $(OUTPUT_FAIL)
#display tests resuts
	@echo --------------------------------------------
	@echo $(COUNTER) tests was executed
	@echo -n "Failed "
	@echo -n `cat $(OUTPUT_FAIL) | wc -l`
	@echo " tests:"
	@cat $(OUTPUT_FAIL)

$(OUTPUT_FAIL):
#has no output fail if this rule is active
	$(error Nothing output)
