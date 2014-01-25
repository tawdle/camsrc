PROGRAM = camsrc
PROGRAM_FILES = camsrc.c

CFLAGS += $(shell pkg-config --cflags --libs gstreamer-1.0 glib-2.0 gio-2.0)

$(PROGRAM): $(PROGRAM_FILES)
	libtool --mode=link gcc -Wall $(PROGRAM_FILES) -o $(PROGRAM) $(CFLAGS)

