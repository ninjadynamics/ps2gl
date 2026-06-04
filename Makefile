EE_LIB = libps2gl.a

EE_LDFLAGS  += -L. -L$(PS2SDK)/ports/lib
# The sibling ps2stuff fork's headers must precede the toolchain-installed copy
# so ps2gl compiles against the fork (e.g. CMemManager::GetMemInfo). Mirrors the
# original Makefile.org's INCDIRS, which referenced ../ps2stuff/include.
EE_INCS     += -I./include -I./vu1 -I../ps2stuff/include -I$(PS2SDK)/ports/include

ifeq ($(DEBUG), 1)
    EE_CFLAGS   += -D_DEBUG
    EE_CXXFLAGS += -D_DEBUG
endif

# Disabling warnings
WARNING_FLAGS = -Wno-strict-aliasing -Wno-conversion-null 

# VU0 code is broken so disable for now
EE_CFLAGS   += $(WARNING_FLAGS) -DNO_VU0_VECTORS -DNO_ASM
EE_CXXFLAGS += $(WARNING_FLAGS) -DNO_VU0_VECTORS -DNO_ASM

EE_OBJS = \
	src/base_renderer.o \
	src/clear.o \
	src/displaycontext.o \
	src/dlgmanager.o \
	src/dlist.o \
	src/drawcontext.o \
	src/gblock.o \
	src/glcontext.o \
	src/gmanager.o \
	src/gsmemory.o \
	src/immgmanager.o \
	src/indexed_renderer.o \
	src/inverse.o \
	src/lighting.o \
	src/linear_renderer.o \
	src/material.o \
	src/matrix.o \
	src/metrics.o \
	src/renderermanager.o \
	src/texture.o

RENDERERS = \
	fast_nolights \
	fast \
	general_nospec_quad \
	general_nospec_tri \
	general_nospec \
	general_pv_diff_quad \
	general_pv_diff_tri \
	general_pv_diff \
	general_quad \
	general_tri \
	general \
	indexed \
	scei

EE_OBJS += $(addsuffix .vo, $(addprefix vu1/, $(RENDERERS)))

VSM_SOURCES = $(addsuffix _vcl.vsm, $(addprefix vu1/, $(RENDERERS)))

all: $(VSM_SOURCES) $(EE_LIB)

install: all
	mkdir -p $(PS2SDK)/ports/include
	mkdir -p $(PS2SDK)/ports/lib
	cp -rf include/GL    $(PS2SDK)/ports/include
	cp -rf include/ps2gl $(PS2SDK)/ports/include
	cp -f  $(EE_LIB) $(PS2SDK)/ports/lib

clean:
	rm -f $(EE_OBJS_LIB) $(EE_OBJS) $(EE_BIN) $(EE_LIB)

realclean: clean
	rm -rf $(PS2SDK)/ports/include/ps2gl
	rm -f  $(PS2SDK)/ports/lib/$(EE_LIB)
	rm -f  $(VSM_SOURCES)

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.eeglobal

%.vo: %_vcl.vsm
	dvp-as -o $@ $<

ifeq ($(REBUILD_VU1),1)
%_vcl.vsm: %_pp4.vcl
	vcl -o$@ $<

%indexed_pp4.vcl: %indexed_pp3.vcl
	cat $< | cc -E -P -imacros vu1/vu1_mem_indexed.h -o $@ -

%_pp4.vcl: %_pp3.vcl
	cat $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -

%_pp3.vcl: %_pp2.vcl
	cat $< | sed 's/\[\([0-9]\)\]/_\1/g ; s/\[\([w-zW-Z]\)\]/\1/g' - > $@

%_pp2.vcl: %_pp1.vcl
	python3 vu1/gasp.py -c ';' -Ivu1 -o $@ $<

%_pp1.vcl: %.vcl
	cat $< | sed 's/#include[ 	]\+.\+// ; s/#define[ 	]\+.\+// ; s|\(\.include[ 	]\+\)"\([^/].\+\)"|\1"$(<D)/\2"|' - > $@
else
%_vcl.vsm:
	@echo "$@ is missing; checked-in VU1 .vsm files are required for normal builds."
	@echo "Install vcl and run with REBUILD_VU1=1 only if you intentionally want to regenerate them."
	@false
endif
