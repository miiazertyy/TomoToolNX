#---------------------------------------------------------------------------------
# Tomodachi Life UGC Editor - Nintendo Switch NRO
# Requires devkitPro + libnx + SDL2 + SDL2_image + SDL2_ttf + zstd (portlibs)
#
# MTP (USB file-sharing via libhaze) is always compiled in.
# One-time setup: run  make setup  to clone libhaze into libs/libhaze/.
# After that, plain  make  builds everything — no flags, no submodules.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitpro>")
endif

TOPDIR ?= $(CURDIR)

# Windows-native toolchain (gcc.exe / ld.exe) reads TMP/TEMP from the
# environment. If they're unset or point at unwritable locations, GCC falls
# back to C:\WINDOWS and fails with "Cannot create temporary file". Pin them
# to the user's local temp here so plain `make` works regardless of the
# launching shell's environment.
ifeq ($(strip $(TMP)),)
TMP := /tmp
endif
ifeq ($(strip $(TEMP)),)
TEMP := /tmp
endif
ifeq ($(strip $(TMPDIR)),)
TMPDIR := /tmp
endif
export TMP
export TEMP
export TMPDIR

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# EXEFS_SRC is the optional input directory containing data copied into exefs
# ROMFS is the optional read only filesystem for this app
#---------------------------------------------------------------------------------
TARGET      := TomoToolNX
APP_TITLE   := TomoToolNX
APP_AUTHOR  := Imprimante
APP_VERSION := 1.3.4
# Title ID intentionally left blank for emulator compatibility:
# Ryubing 1.3.279's Discord-integration module reacts to the "active title
# changed" event during NRO load and dereferences the not-yet-registered
# process, throwing on anything in the 0x01... (real-Switch-app) range.
# Plenty of established Switch homebrew (NX-Shell, JKSV, etc.) ship without
# a title ID for the same reason. The on-device launcher and our app don't
# need this field — it's only used by emulators / Atmosphère's overlay menu.
APP_TITLEID :=
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs

#---------------------------------------------------------------------------------
# libhaze — vendored into libs/libhaze/ (run  make setup  once to clone it)
#---------------------------------------------------------------------------------
LIBHAZE_DIR  := $(TOPDIR)/libs/libhaze
LIBHAZE_COMMIT := 81154c1

LIBHAZE_SRCS := \
	$(LIBHAZE_DIR)/source/async_usb_server.cpp \
	$(LIBHAZE_DIR)/source/device_properties.cpp \
	$(LIBHAZE_DIR)/source/event_reactor.cpp \
	$(LIBHAZE_DIR)/source/haze.cpp \
	$(LIBHAZE_DIR)/source/ptp_object_database.cpp \
	$(LIBHAZE_DIR)/source/ptp_object_heap.cpp \
	$(LIBHAZE_DIR)/source/ptp_responder_mtp_operations.cpp \
	$(LIBHAZE_DIR)/source/ptp_responder_ptp_operations.cpp \
	$(LIBHAZE_DIR)/source/ptp_responder.cpp \
	$(LIBHAZE_DIR)/source/usb_session.cpp \
	$(LIBHAZE_DIR)/source/threaded_file_transfer.cpp \
	$(LIBHAZE_DIR)/source/log.cpp

LIBHAZE_LIB  := $(TOPDIR)/$(BUILD)/libhaze.a

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ \
			-DSDL_DISABLE_IMMINTRIN_H

CXXFLAGS	:= $(CFLAGS) -std=c++17 -fno-rtti -fno-exceptions

# libhaze needs C++20 and tolerates tautological-compare warnings
HAZE_CXXFLAGS := -g -Os -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
                 -std=c++20 -fno-rtti -fno-exceptions \
                 -D__SWITCH__ \
                 -I$(LIBHAZE_DIR)/include \
                 -I$(DEVKITPRO)/libnx/include \
                 -I$(DEVKITPRO)/portlibs/switch/include \
                 -Wno-tautological-compare

ASFLAGS	:=	-g $(ARCH)
# --gc-sections strips unreferenced functions and data emitted under
# -ffunction-sections / -fdata-sections. The other devkitPro Switch
# templates pair these the same way; without --gc-sections the per-section
# emission only bloats relocation tables and saves nothing.
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map),--gc-sections

LIBS	:= -lSDL2_ttf -lfreetype -lharfbuzz -lbz2 -lSDL2_image -lSDL2_mixer -lvorbisidec -lmodplug -lmpg123 -lopusfile -lopus -lFLAC -logg -lSDL2 -lpng -ljpeg -lwebp -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lzstd -lEGL -lglapi -ldrm_nouveau -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, libhaze.a is in $(BUILD)/
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX) $(DEVKITPRO)/portlibs/switch

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))
# Raw seed_*.bin payloads are the source-of-truth fed to the outer-Makefile
# zstd step; the compressed seed_*_z.bin derivatives are what actually gets
# linked into the NRO. Embedding both would add ~4.4 MB of dead .rodata.
BINFILES	:=	$(filter-out seed_Mii.bin seed_Map.bin seed_Player.bin,$(BINFILES))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SRC	:= $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN	:= $(addsuffix .o,$(BINFILES))
export OFILES 		:= $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:= $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(LIBHAZE_DIR)/include \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(CURDIR)/$(BUILD)

export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(strip $(APP_TITLEID)),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	# elf2nro is a Windows-native binary that requires a Windows-form path. On
	# MSYS / devkitPro's bundled msys2 `$(CURDIR)` is unix-style ("/c/…") and
	# `cygpath -m` correctly converts it. On Git-Bash there's a fallback alias
	# `/home/<user>` pointing under "C:\Program Files\Git\…", so cygpath
	# produces a path with a space that breaks elf2nro's argv split. Prefer
	# bash's builtin `pwd -W` (always gives a Windows path) and fall back to
	# cygpath only when we're not in a shell that supports it.
	export NROFLAGS += --romfsdir='$(shell pwd -W 2>/dev/null || cygpath -m $(CURDIR)/$(ROMFS) 2>/dev/null || echo $(CURDIR))/$(ROMFS)'
endif

.PHONY: $(BUILD) clean all setup

#---------------------------------------------------------------------------------
all: $(BUILD)

#---------------------------------------------------------------------------------
# setup: one-time clone of libhaze — run this once after cloning the repo
#---------------------------------------------------------------------------------
setup:
	@if [ -d "$(LIBHAZE_DIR)/.git" ]; then \
		echo "libhaze already present at $(LIBHAZE_DIR)"; \
	else \
		echo "Cloning libhaze @ $(LIBHAZE_COMMIT)..."; \
		mkdir -p libs; \
		git clone https://github.com/ITotalJustice/libhaze.git $(LIBHAZE_DIR); \
		git -C $(LIBHAZE_DIR) checkout $(LIBHAZE_COMMIT); \
		echo "libhaze ready."; \
	fi

#---------------------------------------------------------------------------------
# Pre-compressed data assets. The Switch links zstd already (-lzstd in LIBS),
# so we compress the seed .sav payloads at build time and decompress them on
# demand at runtime. Each seed compresses to <3% of its raw size (mostly
# zeros / repetitive data), shrinking the NRO by ~4.4 MB.
#
# The WebUI HTML/JS blob is shipped pre-gzipped and served verbatim with
# `Content-Encoding: gzip`; the browser decompresses, so the Switch pays
# zero CPU on this path while NRO size drops by ~340 KB.
#
# Source-of-truth files live in data/. Generated *_z.bin / *_gz.bin
# derivatives are .gitignored and rebuilt whenever the source is newer.
#---------------------------------------------------------------------------------
DATA_DIR := $(TOPDIR)/data
GENERATED_DATA := \
	$(DATA_DIR)/seed_Mii_z.bin \
	$(DATA_DIR)/seed_Map_z.bin \
	$(DATA_DIR)/seed_Player_z.bin \
	$(DATA_DIR)/webui_gz.bin

$(DATA_DIR)/seed_%_z.bin: $(DATA_DIR)/seed_%.bin
	@echo "  [zstd] $(notdir $@)"
	@zstd -19 -q -f $< -o $@

$(DATA_DIR)/webui_gz.bin: $(DATA_DIR)/webui.html
	@echo "  [gzip] $(notdir $@)"
	@gzip -9 -n -c $< > $@

$(BUILD): $(GENERATED_DATA) | $(LIBHAZE_LIB)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Build libhaze as a static archive using the Switch cross-compiler.
# Object files land in $(BUILD)/haze_objs/ to stay separate from app objects.
#---------------------------------------------------------------------------------
HAZE_OBJS := $(patsubst $(LIBHAZE_DIR)/source/%.cpp,$(TOPDIR)/$(BUILD)/haze_objs/%.o,$(LIBHAZE_SRCS))

$(TOPDIR)/$(BUILD)/haze_objs/%.o: $(LIBHAZE_DIR)/source/%.cpp
	@mkdir -p $(dir $@)
	@echo "  [libhaze] $(notdir $<)"
	@$(CXX) $(HAZE_CXXFLAGS) -MMD -MP -c $< -o $@

$(LIBHAZE_LIB): $(HAZE_OBJS)
	@if [ ! -f "$(LIBHAZE_DIR)/include/haze.h" ]; then \
		echo "error: libhaze not found — run 'make setup' first"; \
		exit 1; \
	fi
	@mkdir -p $(BUILD)
	@echo "  [AR] libhaze.a"
	@$(AR) rcs $@ $^

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).pfs0 $(TARGET).nso $(TARGET).nro $(TARGET).nacp $(TARGET).elf
	@rm -f $(GENERATED_DATA)

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets — link against the libhaze archive built in the outer pass
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

# Link: app objects + libhaze.a + system libs
$(OUTPUT).elf	:	$(OFILES)
	@echo "  [LD] $(notdir $@)"
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) -Wl,--start-group $(LIBS) $(TOPDIR)/$(BUILD)/libhaze.a -Wl,--end-group -o $@

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)
# also depend on libhaze object deps
-include $(patsubst %.o,%.d,$(wildcard $(TOPDIR)/$(BUILD)/haze_objs/*.o))

endif
