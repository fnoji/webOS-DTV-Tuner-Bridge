CC = arm-linux-gnueabi-gcc
CFLAGS = -std=gnu99 -O2 -Wall -Wextra -fno-stack-protector -U_FORTIFY_SOURCE -U_TIME_BITS -U_FILE_OFFSET_BITS -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
TVLIBS_DIR = ../tv_libs
LDFLAGS_BIN = -Wl,--dynamic-linker=/lib/ld-linux.so.3 $(TVLIBS_DIR)/libpthread.so.0 $(TVLIBS_DIR)/libdl.so.2 $(TVLIBS_DIR)/libc.so.6 $(TVLIBS_DIR)/ld-linux.so.3
LDFLAGS_SO  = -shared -nodefaultlibs $(TVLIBS_DIR)/libdl.so.2 $(TVLIBS_DIR)/libpthread.so.0 $(TVLIBS_DIR)/libc.so.6 -lgcc

all: libwebos_tuner.so webos_inject webos_tuner_d

libwebos_tuner.so: libwebos_tuner.c
	$(CC) $(CFLAGS) -fPIC libwebos_tuner.c -o $@ $(LDFLAGS_SO)

webos_inject: webos_inject.c
	$(CC) $(CFLAGS) webos_inject.c -o $@ $(LDFLAGS_BIN)

webos_tuner_d: webos_tuner_d.c tv_db.c multi2.c eit.c arib_text.c tv_db.h multi2.h eit.h arib_text.h arib_jis0208.h
	$(CC) $(CFLAGS) webos_tuner_d.c tv_db.c multi2.c eit.c arib_text.c -o $@ $(LDFLAGS_BIN)

clean:
	rm -f libwebos_tuner.so webos_inject webos_tuner_d
