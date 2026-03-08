TARGET = nethostfs_psp
OBJS = main.o exports.o

# HostFS driver registration requires kernel mode.
USE_KERNEL_LIBS = 1

INCDIR =
CFLAGS = -O2 -Wall -fno-builtin-printf
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LIBS = -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpspnet

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
