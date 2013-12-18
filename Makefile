PROGRAM = gerald
PROGRAM_FILES = gerald.c

CFLAGS += $(shell pkg-config --cflags --libs gstreamer-1.0)

$(PROGRAM): $(PROGRAM_FILES)
	/bin/sh ~/gst/master/gstreamer/libtool --mode=link gcc -Wall $(PROGRAM_FILES) -o $(PROGRAM) $(CFLAGS)
