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
	src/clip_renderer.o \
	src/x2q_renderer.o \
	src/x2c_renderer.o \
	src/x2g_renderer.o \
	src/x2r_renderer.o \
	src/x2p_renderer.o \
	src/x2b_renderer.o \
	src/x2e_renderer.o \
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

EE_OBJS += src/unlit_renderer.o

RENDERERS = \
	fast_nolights \
	fast \
	general_clip_tri \
	general_clip_tri_x2 \
	general_clip_tri_x2d_decode \
	general_clip_tri_x2q_decode \
	general_clip_tri_x2c_decode \
	general_clip_glow_x2g_decode \
	general_clip_road_x2r \
	general_clip_pool_x2p \
	general_clip_billboard_x2b \
	general_clip_billboard_x2a \
	general_clip_decal_x2e \
	general_nospec_quad \
	general_nospec_tri \
	general_nospec \
	general_pv_diff_quad \
	general_pv_diff_tri \
	general_unlit_tex_tri \
	general_pv_diff \
	general_quad \
	general_tri \
	general \
	indexed \
	scei

EE_OBJS += $(addsuffix .vo, $(addprefix vu1/, $(RENDERERS)))

VSM_SOURCES = $(addsuffix _vcl.vsm, $(addprefix vu1/, $(RENDERERS)))
X2_VSM = vu1/general_clip_tri_x2_vcl.vsm
X2D_DECODER_VSM = vu1/general_clip_tri_x2d_decode_vcl.vsm
X2D_GUARD = vu1/x2d_microcode_guard.py
X2Q_DECODER_VSM = vu1/general_clip_tri_x2q_decode_vcl.vsm
X2Q_GUARD = vu1/x2q_microcode_guard.py
X2C_DECODER_VSM = vu1/general_clip_tri_x2c_decode_vcl.vsm
X2C_GUARD = vu1/x2c_microcode_guard.py
X2G_DECODER_VSM = vu1/general_clip_glow_x2g_decode_vcl.vsm
X2G_GUARD = vu1/x2g_microcode_guard.py
X2R_VSM = vu1/general_clip_road_x2r_vcl.vsm
X2R_GUARD = vu1/x2r_microcode_guard.py
X2P_VSM = vu1/general_clip_pool_x2p_vcl.vsm
X2P_GUARD = vu1/x2p_microcode_guard.py
X2B_VSM = vu1/general_clip_billboard_x2b_vcl.vsm
X2B_GUARD = vu1/x2b_microcode_guard.py
X2A_VSM = vu1/general_clip_billboard_x2a_vcl.vsm
X2A_GUARD = vu1/x2a_microcode_guard.py
X2E_VSM = vu1/general_clip_decal_x2e_vcl.vsm
X2E_GUARD = vu1/x2e_microcode_guard.py

all: $(VSM_SOURCES) x2d-microcode-guard $(EE_LIB)

$(EE_LIB): x2d-microcode-guard
$(EE_LIB): x2q-microcode-guard
$(EE_LIB): x2g-microcode-guard
$(EE_LIB): x2r-microcode-guard
$(EE_LIB): x2e-microcode-guard
$(EE_LIB): x2c-microcode-guard x2p-microcode-guard
$(EE_LIB): x2b-microcode-guard
$(EE_LIB): x2a-microcode-guard

.PHONY: x2a-microcode-guard
x2a-microcode-guard: $(X2A_VSM) $(X2A_GUARD) $(X2R_GUARD)
	python3 $(X2A_GUARD) $(X2A_VSM)

.PHONY: x2b-microcode-guard
x2b-microcode-guard: $(X2B_VSM) $(X2B_GUARD) $(X2R_GUARD)
	python3 $(X2B_GUARD) $(X2B_VSM)

.PHONY: x2c-microcode-guard x2p-microcode-guard
x2c-microcode-guard: $(X2_VSM) $(X2D_DECODER_VSM) $(X2C_DECODER_VSM) $(X2C_GUARD) $(X2Q_GUARD) $(X2D_GUARD)
	python3 $(X2C_GUARD) $(X2_VSM) $(X2D_DECODER_VSM) $(X2C_DECODER_VSM)

x2p-microcode-guard: $(X2P_VSM) $(X2P_GUARD) $(X2R_GUARD)
	python3 $(X2P_GUARD) $(X2P_VSM)

.PHONY: x2e-microcode-guard
x2e-microcode-guard: $(X2E_VSM) $(X2E_GUARD)
	python3 $(X2E_GUARD) $(X2E_VSM)

.PHONY: x2r-microcode-guard
x2r-microcode-guard: $(X2R_VSM) $(X2R_GUARD)
	python3 $(X2R_GUARD) $(X2R_VSM)

.PHONY: x2g-microcode-guard
x2g-microcode-guard: $(X2_VSM) $(X2D_DECODER_VSM) $(X2G_DECODER_VSM) $(X2D_GUARD) $(X2Q_GUARD) $(X2G_GUARD)
	python3 $(X2G_GUARD) $(X2_VSM) $(X2D_DECODER_VSM) $(X2G_DECODER_VSM)

.PHONY: x2q-microcode-guard
x2q-microcode-guard: $(X2_VSM) $(X2D_DECODER_VSM) $(X2Q_DECODER_VSM) $(X2D_GUARD) $(X2Q_GUARD)
	python3 $(X2Q_GUARD) $(X2_VSM) $(X2D_DECODER_VSM) $(X2Q_DECODER_VSM)

.PHONY: x2d-microcode-guard
x2d-microcode-guard: $(X2_VSM) $(X2D_DECODER_VSM) $(X2D_GUARD)
	python3 $(X2D_GUARD) $(X2_VSM) $(X2D_DECODER_VSM)

# Regenerate all VU1 microcode IN PARALLEL. vcl (Sony, 2001) is a
# single-threaded exhaustive scheduler that takes minutes per renderer; the 13
# renderers are independent, so -j collapses the wall time from the sum of all
# files (~20+ min) to the slowest single file (~2-3 min). Use this instead of
# a serial `make REBUILD_VU1=1` whenever a .vcl or a shared .i/.h changes.
# NOTE: make's dependency chain starts at the .vcl only — after editing a
# shared include (geometry.i, clip_cull.i, vu1_context.h, vu1_mem_*.h), delete
# the affected .vsm (or all of them) first so they regenerate.
NPROC ?= $(shell nproc 2>/dev/null || echo 8)
vsm:
	$(MAKE) -j$(NPROC) REBUILD_VU1=1 $(VSM_SOURCES)

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

vu1/general_clip_road_x2r.vo: $(X2R_VSM) $(X2R_GUARD)
	python3 $(X2R_GUARD) $(X2R_VSM)
	dvp-as -o $@ $(X2R_VSM)

vu1/general_clip_decal_x2e.vo: $(X2E_VSM) $(X2E_GUARD)
	python3 $(X2E_GUARD) $(X2E_VSM)
	dvp-as -o $@ $(X2E_VSM)

vu1/general_clip_pool_x2p.vo: $(X2P_VSM) $(X2P_GUARD) $(X2R_GUARD)
	python3 $(X2P_GUARD) $(X2P_VSM)
	dvp-as -o $@ $(X2P_VSM)

vu1/general_clip_billboard_x2b.vo: $(X2B_VSM) $(X2B_GUARD) $(X2R_GUARD)
	python3 $(X2B_GUARD) $(X2B_VSM)
	dvp-as -o $@ $(X2B_VSM)

vu1/general_clip_billboard_x2a.vo: $(X2A_VSM) $(X2A_GUARD) $(X2R_GUARD)
	python3 $(X2A_GUARD) $(X2A_VSM)
	dvp-as -o $@ $(X2A_VSM)

vu1/general_clip_tri_x2c_decode.vo: $(X2C_DECODER_VSM) $(X2C_GUARD) $(X2Q_GUARD) $(X2D_GUARD)
	python3 $(X2C_GUARD) $(X2_VSM) $(X2D_DECODER_VSM) $(X2C_DECODER_VSM)
	dvp-as -o $@ $(X2C_DECODER_VSM)

ifeq ($(REBUILD_VU1),1)
$(X2C_DECODER_VSM): vu1/general_clip_tri_x2c_decode_pp4.vcl $(X2C_GUARD) $(X2Q_GUARD) $(X2D_GUARD) $(X2_VSM) $(X2D_DECODER_VSM)
	vcl -o$@ $<
	python3 $(X2C_GUARD) --fix-decoder $(X2_VSM) $(X2D_DECODER_VSM) $@
	rm -f $<

$(X2G_DECODER_VSM): vu1/general_clip_glow_x2g_decode_pp4.vcl $(X2G_GUARD) $(X2Q_GUARD) $(X2D_GUARD) $(X2_VSM) $(X2D_DECODER_VSM)
	vcl -o$@ $<
	python3 $(X2G_GUARD) --fix-decoder $(X2_VSM) $(X2D_DECODER_VSM) $@
	rm -f $<

$(X2Q_DECODER_VSM): vu1/general_clip_tri_x2q_decode_pp4.vcl $(X2Q_GUARD) $(X2D_GUARD) $(X2_VSM) $(X2D_DECODER_VSM)
	vcl -o$@ $<
	python3 $(X2Q_GUARD) --fix-decoder $(X2_VSM) $(X2D_DECODER_VSM) $@
	rm -f $<

$(X2D_DECODER_VSM): vu1/general_clip_tri_x2d_decode_pp4.vcl $(X2D_GUARD) $(X2_VSM)
	vcl -o$@ $<
	python3 $(X2D_GUARD) --fix-decoder $(X2_VSM) $@
	rm -f $<

%_vcl.vsm: %_pp4.vcl
	vcl -o$@ $<

%indexed_pp4.vcl: %indexed_pp3.vcl
	cat $< | cc -E -P -imacros vu1/vu1_mem_indexed.h -o $@ -

# GASP emits assembly comments containing apostrophes from shared includes.
# Remove only those comments before the C preprocessor for this new module;
# the established preprocessing rules and existing VU images stay unchanged.
.INTERMEDIATE: vu1/general_clip_road_x2r_pp3.vcl vu1/general_clip_road_x2r_pp4.vcl
vu1/general_clip_road_x2r_pp4.vcl: vu1/general_clip_road_x2r_pp3.vcl
	sed 's/;.*//' $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -

vu1/general_clip_road_x2r_pp1.vcl vu1/general_clip_pool_x2p_pp1.vcl: vu1/ground_clip_shared.i
vu1/general_clip_pool_x2p_pp1.vcl: vu1/source_unlit_emit.i vu1/source_eye_classify.i vu1/source_clip_polygon.i vu1/source_fan_emit.i
.INTERMEDIATE: vu1/general_clip_pool_x2p_pp1.vcl vu1/general_clip_pool_x2p_pp3.vcl vu1/general_clip_pool_x2p_pp4.vcl
vu1/general_clip_pool_x2p_pp4.vcl: vu1/general_clip_pool_x2p_pp3.vcl
	sed 's/;.*//' $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -

vu1/general_clip_billboard_x2b_pp1.vcl: vu1/ground_clip_shared.i vu1/source_unlit_emit.i vu1/source_eye_classify.i vu1/source_clip_polygon.i vu1/source_fan_emit.i
vu1/general_clip_billboard_x2b_pp1.vcl: vu1/source_billboard_begin.i vu1/source_billboard_end.i
vu1/general_clip_billboard_x2a_pp1.vcl: vu1/ground_clip_shared.i vu1/source_unlit_emit.i vu1/source_eye_classify.i vu1/source_clip_polygon.i vu1/source_fan_emit.i vu1/source_billboard_begin.i vu1/source_billboard_end.i
.INTERMEDIATE: vu1/general_clip_billboard_x2a_pp1.vcl vu1/general_clip_billboard_x2a_pp3.vcl vu1/general_clip_billboard_x2a_pp4.vcl
vu1/general_clip_billboard_x2a_pp4.vcl: vu1/general_clip_billboard_x2a_pp3.vcl
	sed 's/;.*//' $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -
.INTERMEDIATE: vu1/general_clip_billboard_x2b_pp1.vcl vu1/general_clip_billboard_x2b_pp3.vcl vu1/general_clip_billboard_x2b_pp4.vcl
vu1/general_clip_billboard_x2b_pp4.vcl: vu1/general_clip_billboard_x2b_pp3.vcl
	sed 's/;.*//' $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -

.INTERMEDIATE: vu1/general_clip_decal_x2e_pp3.vcl vu1/general_clip_decal_x2e_pp4.vcl
vu1/general_clip_decal_x2e_pp4.vcl: vu1/general_clip_decal_x2e_pp3.vcl
	sed 's/;.*//' $< | cc -E -P -imacros vu1/vu1_mem_linear.h -o $@ -

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
