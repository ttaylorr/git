all:: git-credential-osxkeychain

CC = gcc
RM = rm -f
CFLAGS = -g -O2 -Wall

-include ../../../config.mak.autogen
-include ../../../config.mak

git-credential-osxkeychain: git-credential-osxkeychain.o
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -Wl,-framework -Wl,Security

git-credential-osxkeychain.o: git-credential-osxkeychain.c
	$(CC) -c $(CFLAGS) $<

clean:
	$(RM) git-credential-osxkeychain git-credential-osxkeychain.o

.PHONY: test
test: all
	PATH=$$PWD:$$PATH && \
	cd ../../../t && \
	GIT_TEST_CREDENTIAL_HELPER=osxkeychain \
	GIT_TEST_CREDENTIAL_HELPER_SETUP='export HOME=$$USER_HOME' \
	./t0303-credential-external.sh
