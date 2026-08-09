CC ?= clang
CFLAGS += -Wall -Wextra -O3
LIBS = -lcurl -lcrypto -lpthread

TARGET = adrive
SRCS = adrive.c helpers.c cmd_upload.c cmd_list.c cmd_download.c

# Optional: Local cJSON support
# Usage: make USE_LOCAL_CJSON=1
ifdef USE_LOCAL_CJSON
    CFLAGS += -DUSE_LOCAL_CJSON
    SRCS += cJSON.c
else
    LIBS += -lcjson
endif

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Dependencies
adrive.o helpers.o cmd_upload.o cmd_list.o cmd_download.o: adrive.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf *.o

.PHONY: all clean