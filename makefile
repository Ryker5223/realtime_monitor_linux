CROSS = /opt/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

CC = $(CROSS)gcc

TARGET = video_rtmp

SRCS = main.c rtmp_push.c
OBJS = $(SRCS:.c=.o)

FFMPEG_SRC = /home/liu/ffmpeg-3.0
FFMPEG_LIB = /home/liu/imx6ull_ffmpeg/lib

CFLAGS = -Wall -O2 \
         -I$(FFMPEG_SRC)

LDFLAGS = -L$(FFMPEG_LIB) \
          -Wl,-rpath-link,$(FFMPEG_LIB)

LIBS = -lavformat \
       -lavcodec \
       -lavutil \
       -lswscale \
       -lswresample \
       -lpthread \
       -lm \
       -ldl

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean