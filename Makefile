# Makefile — PSP PMD Visualizer (sceGu + pmdmini + sceAudio + ME Custom Core)
export PATH := /usr/local/pspdev/bin:$(PATH)
export PSPDEV := /usr/local/pspdev

TARGET   = pmd_psp

RAWDIR   = .

OBJS     = main.o \
           pmdmini.o pmdwincore.o pmdwin.o opnaw.o p86drv.o ppsdrv.o ppz8l.o table.o util.o \
           opna.o file_fmgen.o sjis2utf.o ymfm_adpcm.o ymfm_ssg.o ymfm_opn.o

INCDIR   = pmdmini/src pmdmini/src/pmdwin pmdmini/src/ymfm . me-custom-core
CFLAGS   = -O3 -G0 -Wall -DPSP -fno-pic -funroll-loops
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fpermissive
ASFLAGS  = $(CFLAGS)

LIBDIR   = me-custom-core/build
LDFLAGS  =
LIBS     = -lme-core -lintrafont -lpspgu -lpspge -lpspaudio -lpsppower -lpspctrl -lpsputility -lm -lstdc++ -lsupc++

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = PMD Visualizer
PSP_EBOOT_ICON  = ICON0.PNG

BUILD_PRX = 0
PSP_FW_VERSION = 660

VPATH = pmdmini/src:pmdmini/src/pmdwin:pmdmini/src/ymfm

# デフォルトターゲット: ビルド→デプロイ一気通貫 (build.makのallより先に宣言)
.DEFAULT_GOAL := build-and-deploy

PSPSDK=$(shell /usr/local/pspdev/bin/psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

build-and-deploy: all
	@echo "=== AUTO DEPLOY ==="
	@bash $(CURDIR)/deploy.sh

