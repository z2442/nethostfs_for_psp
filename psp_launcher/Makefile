TARGET = nethostfs_launcher
OBJS = main.o
BUILD_PRX = 1
WLAN_PROBE ?= 0
KERNEL_BRIDGE ?= 1

INCDIR =
CFLAGS = -O2 -Wall
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LIBS =

ifeq ($(WLAN_PROBE),1)
CFLAGS += -DENABLE_WLAN_SAMPLE_INIT
LIBS += -lpspwlan
endif

ifeq ($(KERNEL_BRIDGE),1)
CFLAGS += -DENABLE_KUBRIDGE_LOAD
LIBS += -lpspkubridge
endif

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = NetHostFS Launcher

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
