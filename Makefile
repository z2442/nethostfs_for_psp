OUTPUT=nethostfs
OBJS=main.o
CFLAGS=-Wall -I.
LDFLAGS=-L. -lpthread
STRIP=strip
PSP_DIRS=psp_client psp_launcher
PSP_CLIENT_PRX=psp_client/nethostfs_psp.prx
PSP_LAUNCHER_CLIENT_PRX=psp_launcher/nethostfs_psp.prx

.PHONY: all clean $(PSP_DIRS)

all: $(OUTPUT) $(PSP_DIRS)

clean:
	rm -f $(OUTPUT) *.o
	for dir in $(PSP_DIRS); do $(MAKE) -C $$dir clean; done
	rm -f $(PSP_LAUNCHER_CLIENT_PRX)

$(OUTPUT): $(OBJS)
	$(LINK.c) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) $@

psp_client:
	$(MAKE) -C $@

psp_launcher: psp_client
	cp -f $(PSP_CLIENT_PRX) $(PSP_LAUNCHER_CLIENT_PRX)
	$(MAKE) -C $@
