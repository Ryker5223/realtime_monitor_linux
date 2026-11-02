CROSS_COMPILE = /opt/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

CC = $(CROSS_COMPILE)gcc

TARGET = audio_encoder
SRC = audio_encoder.c

# ALSA
ALSA_DIR = /home/liu/alsa_arm

# FFmpeg
FFMPEG_INC = /home/liu/ffmpeg-3.0
FFMPEG_LIB = /home/liu/imx6ull_ffmpeg/lib

# 编译参数
CFLAGS = -Wall -O2
CFLAGS += -I$(ALSA_DIR)/include
CFLAGS += -I$(FFMPEG_INC)

# 链接参数
LDFLAGS = -L$(ALSA_DIR)/lib
LDFLAGS += -L$(FFMPEG_LIB)
LDFLAGS += -Wl,-rpath-link,$(ALSA_DIR)/lib
LDFLAGS += -Wl,-rpath-link,$(FFMPEG_LIB)

# 依赖库
LDLIBS = -lasound
LDLIBS += -lavformat
LDLIBS += -lavcodec
LDLIBS += -lavutil
LDLIBS += -lswresample
LDLIBS += -lm -lpthread

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean