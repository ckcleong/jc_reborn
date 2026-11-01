
CFLAGS=-O2 -Wall -Wpedantic -std=c99

# To build with memory instrumentation:  make memtrack   (or: make MEMTRACK=1)
ifeq ($(MEMTRACK),1)
CFLAGS += -DMEMTRACK
endif

LDLIBS=-lSDL2 -rdynamic

EXE =   jc_reborn

OBJ =   jc_reborn.o \
        graphics_pc.o \
        utils.o \
        uncompress.o \
        resource.o \
        resmgr.o \
        dump.o \
        story.o \
        walk.o \
        calcpath.o \
        ads.o \
        ttm.o \
        island.o \
        bench.o \
        graphics.o \
        sound.o \
        events.o \
        config.o


all: $(EXE)

$(EXE): $(OBJ)

# Rebuild from scratch with memory instrumentation enabled
memtrack:
	$(MAKE) -s clean
	$(MAKE) -s MEMTRACK=1

clean:
	rm -v $(OBJ) $(EXE) 2> /dev/null ; true

