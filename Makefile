# YouTube Music — native PS5 payload
# Same toolchain contract as ps5-payload-dev samples and EVO Player:
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make
#   make test          # sends the ELF to elfldr (PS5_HOST:PS5_PORT, default port 9021)
#
# Requires pacbrew SDL2 and FFmpeg installed into the SDK homebrew prefix
# (the prefix EVO Player's build docs install).

PS5_HOST ?= ps5
PS5_PORT ?= 9021
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

HB_INC := $(PS5_PAYLOAD_SDK)/target/user/homebrew/include
HB_LIB := $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib

ifeq ($(wildcard $(HB_INC)/SDL2/SDL.h),)
$(error SDL2 headers not found at $(HB_INC)/SDL2/SDL.h. Install pacbrew SDL2 into $(PS5_PAYLOAD_SDK) — see EVO Player docs/build/building.md)
endif
ifeq ($(wildcard $(HB_INC)/libavformat/avformat.h),)
$(error FFmpeg headers not found at $(HB_INC)/libavformat/avformat.h. Install pacbrew FFmpeg into $(PS5_PAYLOAD_SDK))
endif

ELF := ytmusic.elf

SRCS := src/main.c src/text.c src/net.c src/player.c src/latebind.c src/art.c src/daemon_client.c

# ytmusicd, the background payload. It is always a loader payload (the app sends it to the
# ELF loader on port 9021), stripped and embedded in both app builds by src/daemon_blob.S.
DAEMON := build/ytmusicd.elf
DAEMON_STRIPPED := build/ytmusicd.stripped.elf
BLOB := src/daemon_blob.S

CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -g \
	-I$(HB_INC) -I$(HB_INC)/SDL2 -D_REENTRANT

SDL_LIBS :=
ifneq ($(wildcard $(HB_LIB)/libSDL2.a),)
SDL_LIBS += $(HB_LIB)/libSDL2main.a $(HB_LIB)/libSDL2.a
else
SDL_LIBS += -L$(HB_LIB) -lSDL2main -lSDL2
endif

FF_LIBS :=
ifneq ($(wildcard $(HB_LIB)/pkgconfig/libavformat.pc),)
FF_LIBS := $(shell PKG_CONFIG_PATH="$(HB_LIB)/pkgconfig" pkg-config --libs libavformat libavcodec libswresample libavutil)
endif
ifeq ($(strip $(FF_LIBS)),)
ifneq ($(wildcard $(HB_LIB)/libavformat.a),)
FF_LIBS := $(HB_LIB)/libavformat.a $(HB_LIB)/libavcodec.a \
	$(HB_LIB)/libswresample.a $(HB_LIB)/libavutil.a
else
FF_LIBS := -L$(HB_LIB) -lavformat -lavcodec -lswresample -lavutil
endif
endif

LIBS := $(SDL_LIBS) $(FF_LIBS) \
	-lssl -lcrypto -lz -lm -lbz2 -lpng -liconv \
	-lkernel_sys -lSceSystemService -lSceUserService \
	-lScePad -lSceVideoOut -lSceAudioOut \
	-lSceNet -lSceSsl -lSceHttp2 \
	-lSceKeyboard -lSceImeDialog -pthread

# Home-screen eboot is not the elfldr payload. Link a PIE at address 0,
# then convert it to a PS5 module (e_type 0xFE10) before signing.
NATIVE_DIR := build/native
NATIVE_OBJS := $(patsubst src/%.c,$(NATIVE_DIR)/%.o,$(SRCS)) $(NATIVE_DIR)/daemon_blob.o
NATIVE_APP := $(NATIVE_DIR)/bigapp.o
NATIVE_CRT := $(NATIVE_DIR)/app_crt.o
NATIVE_PIE := $(NATIVE_DIR)/llvm-pie.elf
NATIVE_ELF := $(NATIVE_DIR)/eboot.elf
NATIVE_LIBS := $(filter-out -pthread -lkernel_sys -lSceKeyboard -lSceImeDialog,$(LIBS)) -lkernel -lSceLibcInternal
NATIVE_STUBS := $(filter-out %/libkernel_sys.so %/libkernel_web.so %/libkernel_stub_weak.so,$(wildcard $(PS5_PAYLOAD_SDK)/target/lib/*.so))
NATIVE_LIBC := $(NATIVE_DIR)/libc-nodl.a
NATIVE_COMPAT := $(NATIVE_DIR)/payload_compat.o

.PHONY: all clean test package probe daemon

all: $(ELF)

$(DAEMON): src/daemon.c src/ytmd.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -O2 -o $@ src/daemon.c

$(DAEMON_STRIPPED): $(DAEMON)
	$(STRIP) --strip-all -o $@ $<

daemon: $(DAEMON)

$(ELF): $(SRCS) $(BLOB) $(DAEMON_STRIPPED) src/ui_font.h src/app.h src/ytmd.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(BLOB) $(LIBS)

$(NATIVE_DIR)/%.o: src/%.c src/ui_font.h src/app.h src/ytmd.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -ffunction-sections -fdata-sections -c -o $@ $<

$(NATIVE_DIR)/daemon_blob.o: $(BLOB) $(DAEMON_STRIPPED)
	mkdir -p $(dir $@)
	$(CC) -fPIC -c -o $@ $(BLOB)

$(NATIVE_CRT): third_party/ps5-native/app_crt.cpp
	mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O2 -fno-exceptions -fno-rtti -fPIC \
		-ffunction-sections -fdata-sections -c -o $@ $<

$(NATIVE_COMPAT): third_party/ps5-native/payload_compat.c
	mkdir -p $(dir $@)
	$(CC) -O2 -fPIC -ffunction-sections -fdata-sections -c -o $@ $<

# Drop payload libc objects that shadow libkernel and leave weak imports
# (__dlopen, kernel_mprotect) the PS5 module converter cannot bind.
$(NATIVE_LIBC): $(PS5_PAYLOAD_SDK)/target/lib/libc.a
	mkdir -p $(dir $@)
	cp $< $@
	for obj in dlfcn.o mman.o; do \
		if $(AR) t $@ | grep -qx "$$obj"; then $(AR) d $@ "$$obj"; fi; \
	done

$(NATIVE_PIE): $(NATIVE_CRT) $(NATIVE_OBJS) $(NATIVE_APP) $(NATIVE_COMPAT) $(NATIVE_LIBC) \
		third_party/ps5-native/ps5-pie.ld \
		third_party/ps5-native/app-symbols.map
	$(LD) -T third_party/ps5-native/ps5-pie.ld \
		--version-script third_party/ps5-native/app-symbols.map \
		--allow-multiple-definition \
		-e _start -o $@ \
		$(NATIVE_CRT) $(NATIVE_OBJS) $(NATIVE_APP) $(NATIVE_COMPAT) \
		-L$(PS5_PAYLOAD_SDK)/target/lib -L$(HB_LIB) \
		$(NATIVE_LIBS) \
		$(NATIVE_LIBC) \
		--as-needed $(NATIVE_STUBS)

$(NATIVE_ELF): $(NATIVE_PIE) scripts/build-self-tool.sh
	tool=$$(sh scripts/build-self-tool.sh); \
	"$$tool" link --in $(NATIVE_PIE) --out $@ \
		--stub-dir $(PS5_PAYLOAD_SDK)/target/lib \
		--module-sdk 0x02000009 --companion-sdk 0x08050001 \
		--heap-size 0x10000000 --file-name eboot.elf
	python3 -c 'import sys; b=open("$(NATIVE_ELF)","rb").read(18); t=int.from_bytes(b[16:18],"little"); print("native eboot type", hex(t)); sys.exit(t!=0xfe10)'

# Diagnostic: which audio path stays audible while a game has focus (tools/audioprobe.c).
PROBE := ytm-audioprobe.elf
probe: $(PROBE)
$(PROBE): tools/audioprobe.c
	$(CC) -std=c11 -Wall -Wextra -O2 -o $@ $< -lSceAudioOut -lSceUserService -lkernel_sys -lm

clean:
	rm -f $(ELF) $(PROBE)
	rm -rf dist build/native $(DAEMON) $(DAEMON_STRIPPED)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

package: $(ELF) $(NATIVE_ELF)
	sh scripts/package-homebrew.sh
