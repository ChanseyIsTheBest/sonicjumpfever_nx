#---------------------------------------------------------------------------------
# sonicjumpfever_nx -- Sonic Jump Fever wrapper for Nintendo Switch
#
# Requires devkitPro with switch-dev, plus:
#     dkp-pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-zlib \
#                   switch-ffmpeg switch-libpng switch-pkg-config
#
# Ships no game data: the user copies their own APK next to the .nro.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Source $$DEVKITPRO/switchvars.sh or install devkitPro.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET      := sonicjumpfever_nx
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

APP_TITLE   := Sonic Jump Fever
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0
# icon.jpg if there is one, otherwise libnx's default icon, so a missing icon
# never breaks the build.
ICON        := $(if $(wildcard icon.jpg),icon.jpg,)

#---------------------------------------------------------------------------------
# The loaded module is built for bionic and expects soft TLS; -mtp=soft is not
# optional. -fPIE matches libnx's NRO model.
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES := -D__SWITCH__

# nx_pointer's gyro pointing is compiled out: the six-axis sensors are read by
# sj_sensor.c and reported to the game as the Android accelerometer, which is
# how Sonic Jump Fever steers. Two consumers would fight over the same handles.
DEFINES += -DNXP_NO_GYRO=1

CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -ffunction-sections -fdata-sections \
           $(ARCH) $(DEFINES)

# An implicit declaration is not a style problem on aarch64: the compiler
# assumes an int return and guesses at the arguments, so a call through one can
# silently corrupt registers. Newer GCC makes this an error anyway; make it one
# everywhere so a build on an older toolchain fails the same way.
CFLAGS  += -Werror=implicit-function-declaration -Werror=implicit-int
CFLAGS  += -Werror=incompatible-pointer-types

# A "/*" inside a comment usually means a block comment was accidentally ended
# early, which silently swallows code.
CFLAGS  += -Wcomment

CFLAGS  += -Wno-unused-but-set-variable

CFLAGS  += $(INCLUDE)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS  := $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections \
            -Wl,-Map,$(notdir $*.map)

# ffmpeg decodes the .m4a soundtrack on-device (sj_decode.c), reading it
# straight out of the user's APK. Resolved through pkg-config because
# switch-ffmpeg's transitive deps (opus, vorbis, ogg, dav1d, ...) vary with how
# the portlib was built, and a hand-written list goes stale.
FFMPEG_PKGS := libavformat libavcodec libswresample libavutil

# devkitPro puts pkg-config in different places depending on version and host,
# so try the known spellings before falling back to a plain list. --static
# matters: the portlib archives need their own dependencies named explicitly.
PKGCONF := $(firstword $(wildcard \
             $(PORTLIBS)/bin/aarch64-none-elf-pkg-config \
             $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config \
             $(PORTLIBS)/bin/pkg-config))
ifneq ($(PKGCONF),)
FFMPEG_LIBS := $(shell $(PKGCONF) --libs --static $(FFMPEG_PKGS) 2>/dev/null)
endif
ifeq ($(strip $(FFMPEG_LIBS)),)
# Fallback. If the link fails on undefined av*/swr* symbols, install
# switch-pkg-config, or append whatever switch-ffmpeg was built against here.
FFMPEG_LIBS := -lavformat -lavcodec -lswresample -lavutil -lz -lbz2
endif

# SDL2 is required: opensles.c uses it for the single audio output device and
# for its mixer mutexes. sj_music.c does NOT open a second device -- it feeds
# the same mix through opensles_set_music_source().
#
# mesa provides GLESv2/EGL on top of drm_nouveau. zlib is for our own use:
# sj_apkzip.c inflates the game library out of the APK. libsonicjumpfever.so
# statically links its own copy and imports no zlib symbols, so do NOT let a
# shim shadow inflate()/deflate().
# -lpng is for nx_pointer.c, which decodes an optional cursor.png. libpng
# depends on zlib, so it must come BEFORE -lz on the link line: the linker
# resolves left to right and would otherwise have finished with zlib before it
# learned libpng needed it.
LIBS    := -lSDL2 $(FFMPEG_LIBS) \
           -Wl,--start-group -lGLESv2 -lEGL -lglapi -ldrm_nouveau -Wl,--end-group \
           -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# Link with the C++ driver even though every source here is C.
#
# switch-mesa's libEGL.a contains the nouveau shader compiler (nv50_ir), which
# is C++. Linking with $(CC) leaves its `operator new`, `operator delete`,
# `std::__detail::_List_node_base::_M_hook` and `std::__throw_length_error`
# references unresolved, producing several hundred lines of undefined-symbol
# errors that all point into libEGL.a and none of which are your code. The
# stock devkitPro template picks $(CC) when there are no .cpp files, which is
# wrong the moment you link mesa.
export LD := $(CXX)

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 \
                  -I$(PORTLIBS)/include \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    export APP_ICON := $(LIBNX)/default_icon.jpg
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

.PHONY: $(BUILD) clean all check check-includes dist

all: check-includes $(BUILD)

# Cheap guard against glibc-only headers, which compile on a Linux host and
# then fail on devkitA64. Runs before every build; skipped if python3 is absent.
check-includes:
	@command -v python3 >/dev/null && python3 tools/check_includes.py source/ || true

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).map sd_card

# Verify the resolver table still covers every import of the game library.
# SO may be the extracted .so or the APK itself:
#   make check SO=/path/to/sonic-jump-fever.apk
check:
	@python3 tools/verify_imports.py $(SO)

# Lay out sd_card/switch/sonicjumpfever_nx/ ready to copy to the SD card.
# Pass APK= to copy your APK in as well:
#   make dist APK=/path/to/sonic-jump-fever.apk
dist: all
	@sh tools/prepare_game.sh $(APK)

#---------------------------------------------------------------------------------
else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif

# Introspection, for debugging a link problem without reading the whole file:
#   make print-LD      -> which driver links the .elf
#   make print-LIBS    -> the fully expanded library line
#   make print-PKGCONF -> which pkg-config was found, if any
print-%:
	@echo "$* = $($*)"
