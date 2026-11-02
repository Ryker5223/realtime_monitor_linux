ARCH := arm

CROSS_COMPILE := /opt/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

CC := $(CROSS_COMPILE)gcc

TARGET := video_rtmp

SRCS := main.c \
        rtmp_push.c \
        http.c \
        status.c

OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

# ===================================
# FFmpeg 3.0
# ===================================

FFMPEG_SRC := /home/liu/ffmpeg-3.0
FFMPEG_LIB := /home/liu/imx6ull_ffmpeg/lib

# ===================================
# ALSA
# ===================================

ALSA_DIR := /home/liu/alsa_arm

ALSA_INC := $(ALSA_DIR)/include
ALSA_LIB := $(ALSA_DIR)/lib

# ===================================
# 编译参数
# ===================================

CPPFLAGS := -I$(FFMPEG_SRC) \
            -I$(ALSA_INC)

CFLAGS := -std=gnu99 -Wall -O2 -MMD -MP -pthread

# ===================================
# 链接参数
# ===================================

LDFLAGS := -L$(FFMPEG_LIB) \
           -L$(ALSA_LIB) \
           -Wl,-rpath-link,$(FFMPEG_LIB) \
           -Wl,-rpath-link,$(ALSA_LIB)

# ===================================
# 依赖库
# ===================================

LDLIBS := -lavformat \
          -lavcodec \
          -lavutil \
          -lswscale \
          -lswresample \
          -lasound \
          -pthread \
          -lm \
          -ldl

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)