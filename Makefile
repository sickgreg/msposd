VERSION_STRING := $(shell date +"%Y%m%d_%H%M%S")
CFLAGS ?=
CFLAGS += -Wno-address-of-packed-member -DVERSION_STRING="\"$(VERSION_STRING)\""

SRCS := compat.c msposd2.c bmp/bitmap.c bmp/region.c bmp/lib/schrift.c bmp/text.c osd/msp/msp.c osd/msp/msp_displayport.c libpng/lodepng.c osd/util/interface.c osd/util/settings.c osd/util/ini_parser.c osd/msp/vtxmenu.c osd/util/subtitle.c
OUTPUT ?= $(CURDIR)/msposd2
DRV ?=
BUILD = $(CC) $(SRCS) -I$(SDK)/include -I$(TOOLCHAIN)/usr/include -I$(CURDIR) -L$(DRV) $(CFLAGS) $(LIB) -levent_core -Os -s -o $(OUTPUT)

VERSION := $(shell git describe --always --dirty)

version.h:
	@echo "Git version: $(VERSION)"
	@echo "#ifndef VERSION_H" > version.h
	@echo "#define VERSION_H" >> version.h
	@echo "#define GIT_VERSION \"$(VERSION)\"" >> version.h
	@echo "#endif // VERSION_H" >> version.h

all: star6e

clean:
	rm -f version.h msposd2

star6e: version.h
	$(eval SDK = ./sdk/infinity6)
	$(eval CFLAGS += -D__SIGMASTAR__ -D__INFINITY6__ -D__INFINITY6E__)
	$(eval LIB = -lcam_os_wrapper -lm -lmi_rgn -lmi_sys)
	$(BUILD)
