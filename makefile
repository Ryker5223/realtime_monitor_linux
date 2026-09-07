CROSS_COMPILE := /opt/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc

TARGET := video_rtmp

SRCS := main.c \
        rtmp_push.c \
        http.c \
        status.c

OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

FFMPEG_SRC := /home/liu/ffmpeg-3.0
FFMPEG_LIB := /home/liu/imx6ull_ffmpeg/lib

CPPFLAGS := -I$(FFMPEG_SRC)
CFLAGS := -Wall -O2 -MMD -MP

LDFLAGS := -L$(FFMPEG_LIB) \
           -Wl,-rpath-link,$(FFMPEG_LIB)

LDLIBS := -lavformat \
          -lavcodec \
          -lavutil \
          -lswscale \
          -lswresample \
          -lpthread \
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