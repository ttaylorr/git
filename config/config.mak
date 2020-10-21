Meta = $(HOME)/src/git/Meta
ifeq ($(GIT_VERSION),omitted)
prefix := /none
else
prefix_base := $(shell $(Meta)/install/prefix)
ifeq ($(prefix_base), detached)
prefix := /do/not/install
else
prefix := $(HOME)/local/git/$(prefix_base)
endif
endif

CFLAGS =

COMPILER ?= gcc
O = 0
CC = ccache $(COMPILER)
export CCACHE_CPP2=1
CFLAGS += -g -O$(O) -Wall
LDFLAGS = -g

USE_LIBPCRE = YesPlease

GIT_TEST_OPTS = --root=/var/ram/git-tests -x --verbose-log
GIT_PROVE_OPTS= -j16 --state=slow,save
DEFAULT_TEST_TARGET = prove
export GIT_TEST_HTTPD = Yes
export GIT_TEST_GIT_DAEMON = Yes

GNU_ROFF = Yes
MAN_BOLD_LITERAL = Yes

NO_GETTEXT = Nope
NO_OPENSSL = Nope
NO_TCLTK = Nope
XDL_FAST_HASH =

GIT_PERF_MAKE_OPTS = O=2 strict= -j16
GIT_INTEROP_MAKE_OPTS = strict= -j16

CFLAGS += $(EXTRA_CFLAGS)

-include $(Meta)/config/config.mak.local
