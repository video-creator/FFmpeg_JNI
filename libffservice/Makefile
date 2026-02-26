#include $(SUBDIR)../config.mak
DESC = FFmpeg service  library
NAME = ffservice

HEADERS = ffservice.h           \

FFTOOLS = ../fftools
OBJS +=                  \
    ${FFTOOLS}/ffmpeg_jni.o        \
    ${FFTOOLS}/ffmpeg_dec.o        \
    ${FFTOOLS}/ffmpeg_demux.o      \
    ${FFTOOLS}/ffmpeg_enc.o        \
    ${FFTOOLS}/ffmpeg_filter.o     \
    ${FFTOOLS}/ffmpeg_hw.o         \
    ${FFTOOLS}/ffmpeg_mux.o        \
    ${FFTOOLS}/ffmpeg_mux_init.o   \
    ${FFTOOLS}/ffmpeg_opt.o        \
    ${FFTOOLS}/ffmpeg_sched.o      \
    ${FFTOOLS}/graph/graphprint.o        \
    ${FFTOOLS}/sync_queue.o        \
    ${FFTOOLS}/thread_queue.o      \
    ${FFTOOLS}/textformat/avtextformat.o \
    ${FFTOOLS}/textformat/tf_compact.o   \
    ${FFTOOLS}/textformat/tf_default.o   \
    ${FFTOOLS}/textformat/tf_flat.o      \
    ${FFTOOLS}/textformat/tf_ini.o       \
    ${FFTOOLS}/textformat/tf_json.o      \
    ${FFTOOLS}/textformat/tf_mermaid.o   \
    ${FFTOOLS}/textformat/tf_xml.o       \
    ${FFTOOLS}/textformat/tw_avio.o      \
    ${FFTOOLS}/textformat/tw_buffer.o    \
    ${FFTOOLS}/textformat/tw_stdout.o    \
    $(OBJS-resman)                    \
    $(RESOBJS)                        \
