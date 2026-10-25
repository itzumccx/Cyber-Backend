# Cyber - Fortnite Backend
# Compiler: gcc (MinGW on Windows)

CC     = gcc
TARGET = cyber.exe

# Source files
SRCS = src/main.c      \
       src/router.c    \
       src/auth.c      \
       src/eos.c       \
       src/mcp.c       \
       src/friends.c   \
       src/locker.c    \
       src/shop.c      \
       src/cloudstorage.c \
       src/profile.c   \
       src/log.c       \
       src/errors.c    \
       src/version.c   \
       vendor/cjson/cJSON.c     \
       vendor/mongoose/mongoose.c

# Include paths
INCLUDES = -Ivendor/cjson -Ivendor/mongoose

# Flags
CFLAGS  = -Wall -O2 $(INCLUDES) -DMG_ENABLE_LOG=0
# Windows needs ws2_32 for networking
LDFLAGS = -lws2_32

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	@echo Build complete: $(TARGET)

clean:
	del /f $(TARGET) 2>nul || true

.PHONY: all clean
