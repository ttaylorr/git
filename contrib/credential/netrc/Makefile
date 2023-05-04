# The default target of this Makefile is...
all::

SCRIPT_PERL = git-credential-netrc.perl
GIT_ROOT_DIR = ../../..
HERE = contrib/credential/netrc

SCRIPT_PERL_FULL = $(patsubst %,$(HERE)/%,$(SCRIPT_PERL))

all:: build

build:
	$(MAKE) -C $(GIT_ROOT_DIR) SCRIPT_PERL="$(SCRIPT_PERL_FULL)" \
                build-perl-script

install: build
	$(MAKE) -C $(GIT_ROOT_DIR) SCRIPT_PERL="$(SCRIPT_PERL_FULL)" \
                install-perl-script

clean:
	$(MAKE) -C $(GIT_ROOT_DIR) SCRIPT_PERL="$(SCRIPT_PERL_FULL)" \
                clean-perl-script

test: build
	./t-git-credential-netrc.sh

testverbose: build
	./t-git-credential-netrc.sh -d -v

testgit: build
	PATH=$$PWD:$$PATH && \
	cd ../../../t && \
	GIT_TEST_CREDENTIAL_HELPER=git-credential-netrc \
	./t0303-credential-external.sh

.PHONY: all build install clean test testverbose testgit
