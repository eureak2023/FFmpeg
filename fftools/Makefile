AVPROGS-$(CONFIG_FFMPEG)   += ffmpeg
AVPROGS-$(CONFIG_FFPLAY)   += ffplay
AVPROGS-$(CONFIG_FFPROBE)  += ffprobe

AVPROGS     := $(AVPROGS-yes:%=%$(PROGSSUF)$(EXESUF))
PROGS       += $(AVPROGS)

AVBASENAMES  = ffmpeg ffplay ffprobe
ALLAVPROGS   = $(AVBASENAMES:%=%$(PROGSSUF)$(EXESUF))
ALLAVPROGS_G = $(AVBASENAMES:%=%$(PROGSSUF)_g$(EXESUF))

include $(SRC_PATH)/fftools/resources/Makefile

OBJS-ffmpeg +=                  \
    fftools/ffmpeg_dec.o        \
    fftools/ffmpeg_demux.o      \
    fftools/ffmpeg_enc.o        \
    fftools/ffmpeg_filter.o     \
    fftools/ffmpeg_hw.o         \
    fftools/ffmpeg_mux.o        \
    fftools/ffmpeg_mux_init.o   \
    fftools/ffmpeg_opt.o        \
    fftools/ffmpeg_sched.o      \
    fftools/graph/graphprint.o        \
    fftools/sync_queue.o        \
    fftools/thread_queue.o      \
    fftools/textformat/avtextformat.o \
    fftools/textformat/tf_compact.o   \
    fftools/textformat/tf_default.o   \
    fftools/textformat/tf_flat.o      \
    fftools/textformat/tf_ini.o       \
    fftools/textformat/tf_json.o      \
    fftools/textformat/tf_mermaid.o   \
    fftools/textformat/tf_xml.o       \
    fftools/textformat/tw_avio.o      \
    fftools/textformat/tw_buffer.o    \
    fftools/textformat/tw_stdout.o    \
    $(OBJS-resman)                    \
    $(RESOBJS)                        \

OBJS-ffprobe +=                       \
    fftools/textformat/avtextformat.o \
    fftools/textformat/tf_compact.o   \
    fftools/textformat/tf_default.o   \
    fftools/textformat/tf_flat.o      \
    fftools/textformat/tf_ini.o       \
    fftools/textformat/tf_json.o      \
    fftools/textformat/tf_mermaid.o   \
    fftools/textformat/tf_xml.o       \
    fftools/textformat/tw_avio.o      \
    fftools/textformat/tw_buffer.o    \
    fftools/textformat/tw_stdout.o    \

OBJS-ffmpeg += $(COMPAT_OBJS:%=compat/%)
OBJS-ffplay += fftools/ffplay_renderer.o
OBJS-ffplay += fftools/ffplay_fsr.o
OBJS-ffplay += fftools/ffplay_ui.o
OBJS-ffplay += fftools/ffplay_thumb.o
# RIFE frame interpolation (ncnn + Vulkan). The wrapper under fftools/rife/ is
# rife-ncnn-vulkan by nihui (MIT, see fftools/rife/LICENSE); ncnn itself is
# vendored at the repo root like SDL2/dav1d/nvheaders. Vulkan is reached
# through the system vulkan-1.dll, which ncnn loads at runtime, so the
# executable stays free of non-system DLL imports.
OBJS-ffplay += fftools/ffplay_rife.o
OBJS-ffplay += fftools/rife/rife.o
OBJS-ffplay += fftools/rife/warp.o
NCNN_DIR := $(SRC_PATH)/../ncnn
fftools/ffplay_rife.o fftools/rife/rife.o fftools/rife/warp.o: \
    CXXFLAGS += -I$(SRC_PATH)/fftools/rife -I$(NCNN_DIR)/include/ncnn -fopenmp
EXTRALIBS-ffplay += -L$(NCNN_DIR)/lib -lncnn -lglslang -lSPIRV \
    -lMachineIndependent -lGenericCodeGen -lOSDependent \
    -lglslang-default-resource-limits -fopenmp -lstdc++
# DLSS 5 neural rendering (NGX "dlssnr"). The model runs on a private D3D12
# device; d3d12.dll is loaded at runtime, so only the static GUID archive is
# linked and the executable's import table is unchanged.
OBJS-ffplay += fftools/ffplay_ngx.o
EXTRALIBS-ffplay += -ldxguid
# The NGX snippet refuses callers whose module path does not contain
# "nvngx.dll", so the calls have to come from a separately named module. It is
# built here and embedded into ffplay.exe by ffplay_res.rc, which means the
# resource compile has to wait for it.
FFPLAY_NGX_SHIM := fftools/nvngx.dll_ffplay.dll
$(FFPLAY_NGX_SHIM): $(SRC_PATH)/fftools/ffplay_ngxshim.c \
                    $(SRC_PATH)/fftools/ffplay_ngx.h
	$(CC) -O2 -fno-optimize-sibling-calls -shared -static -static-libgcc \
	    -I$(SRC_PATH)/fftools -o $@ $< -lkernel32
fftools/ffplay_res.o: $(FFPLAY_NGX_SHIM)
OBJS-ffplay-$(HAVE_GNU_WINDRES) += fftools/ffplay_res.o
# GUI subsystem: no console window flashes on double-click launches;
# fsr_detach_console() re-attaches to the parent console when run from one.
EXTRALIBS-ffplay += $(if $(HAVE_GNU_WINDRES),-mwindows)

define DOFFTOOL
OBJS-$(1) += fftools/cmdutils.o fftools/opt_common.o fftools/$(1).o $(OBJS-$(1)-yes)
ifdef HAVE_GNU_WINDRES
OBJS-$(1) += fftools/fftoolsres.o
endif
$(1)$(PROGSSUF)_g$(EXESUF): $$(OBJS-$(1))
$$(OBJS-$(1)): | fftools fftools/rife fftools/textformat fftools/resources fftools/graph
$$(OBJS-$(1)): CFLAGS  += $(CFLAGS-$(1))
$(1)$(PROGSSUF)_g$(EXESUF): LDFLAGS += $(LDFLAGS-$(1))
$(1)$(PROGSSUF)_g$(EXESUF): FF_EXTRALIBS += $(EXTRALIBS-$(1))
-include $$(OBJS-$(1):.o=.d)
endef

$(foreach P,$(AVPROGS-yes),$(eval $(call DOFFTOOL,$(P))))

all: $(AVPROGS)

fftools/ffprobe.o fftools/cmdutils.o: libavutil/ffversion.h | fftools
OUTDIRS += fftools
OUTDIRS += fftools/rife
OUTDIRS += fftools/textformat
OUTDIRS += fftools/resources
OUTDIRS += fftools/graph

ifdef AVPROGS
install: install-progs install-data
endif

install-progs-yes:
install-progs-$(CONFIG_SHARED): install-libs

install-progs: install-progs-yes $(AVPROGS)
	$(Q)mkdir -p "$(BINDIR)"
	$(INSTALL) -c -m 755 $(AVPROGS) "$(BINDIR)"

uninstall: uninstall-progs

uninstall-progs:
	$(RM) $(addprefix "$(BINDIR)/", $(ALLAVPROGS))

clean::
	$(RM) $(ALLAVPROGS) $(ALLAVPROGS_G) $(CLEANSUFFIXES:%=fftools/%) $(CLEANSUFFIXES:%=fftools/graph/%) $(CLEANSUFFIXES:%=fftools/rife/%) $(CLEANSUFFIXES:%=fftools/textformat/%)
