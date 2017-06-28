
# Makefile for keeping objects in their own  platform-specific
# subdirectory

# build platform
platform := $(shell uname -s)
odirsuff := dbg

ifeq ($(O),1)
	OPTIMIZE := -O3
	odirsuff := rel
endif


ifeq ($(ANDROID),1)
	CROSS 	 := arm-linux-androideabi-
	platform := android
endif


ifeq ($(ANDROID64),1)
	CROSS 	 := aarch64-linux-android-
	platform := android64
endif

Linux_DEFS  = -D_GNU_SOURCE=1
Linux_LIBS  = -lpthread -lbsd

# Android build flags
android_LIBS	:= -llog
android_SOFLAGS = -shared

android64_LIBS	:= -llog
android64_SOFLAGS = -shared

INCDIRS = . ./$(platform) .. $($(platform)_INCDIRS)

# address sanitizer: in newer versions of gcc and clang
sanitize = -fsanitize=address \
		   -fsanitize=bounds \
		   -fstack-protector

CC = $(CROSS)gcc
LD = $(CC) -g
AR = $(CROSS)ar
INCS    = $(addprefix -I, $(INCDIRS))
DEFS    = -D__$(platform)__=1 $($(platform)_DEFS)
CFLAGS  = -g -Wall -fPIC $(sanitize) $(DEFS) $(INCS) $(OPTIMIZE)
LDLIBS  = $($(platform)_LIBS)
LDFLAGS = $($(platform)_SOFLAGS) $(sanitize)

# These are unadorned objects and exes
objs = ns.o error.o getopt_long.o

exe = ns

vpath %.c . ..

# objdir
o := ./$(platform)-$(odirsuff)

# hack to always make the obj dir
__j := $(shell mkdir -p $(o))

# objs and libs prefixed by the dest-dir
xexe  = $(addprefix $(o)/, $(exe))
xobjs = $(addprefix $(o)/, $(objs))
xdeps = $(xobjs:.o=.d) $(xlibobjs:.o=.d)

all: $(xexe)


$(xexe): $(xobjs)
	$(CC) -o $@ $(LDFLAGS) $^ $(LDLIBS)


objs: $(xobjs)

.PHONY: clean


clean:
	-rm -rf $(o)


$(o)/%.o: %.c
	$(CC) -MMD $(_MP) -MT '$@ $(@:.o=.d)' -MF "$(@:.o=.d)" $(CFLAGS) $($(notdir $@)_CFLAGS) -c -o $@ $<

$(o)/%.i: %.c
	$(CC) -E $(CFLAGS) $($(notdir $@)_CFLAGS) -c -o $@ $<

ifneq ($(xdeps),)
-include $(xdeps)
endif
