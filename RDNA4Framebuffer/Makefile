SDK_PATH = ../navi48/MacKernelSDK

CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -target x86_64-apple-macos11.0 -mkernel -nostdlib \
           -fno-builtin -fno-exceptions -fno-rtti -fno-common -fno-stack-protector \
           -fcheck-new -fno-c++-static-destructors -arch x86_64 \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -I$(SDK_PATH)/Headers -Isrc

CFLAGS = -Wall -O2 -target x86_64-apple-macos11.0 -mkernel -nostdlib \
         -fno-builtin -fno-common -fno-stack-protector -arch x86_64 \
         -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -I$(SDK_PATH)/Headers -Isrc

LDFLAGS = -arch x86_64 -target x86_64-apple-macos11.0 \
          -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
          -nostdlib -Xlinker -kext \
          -L$(SDK_PATH)/Library/x86_64 -lkmodc++ -lkmod -lcc_kext \
          -Wl,-no_deduplicate

KEXT = build/RDNA4Framebuffer.kext
OBJS = build/RDNA4Framebuffer.o build/kmod_info.o

all: $(KEXT)

$(KEXT): $(OBJS) Info.plist
	@echo "Building kext..."
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OBJS) -o $(KEXT)/Contents/MacOS/RDNA4Framebuffer
	cp Info.plist $(KEXT)/Contents/
	chmod -R 755 $(KEXT)
	chmod 644 $(KEXT)/Contents/Info.plist
	@echo "✓ Kext built: $(KEXT)"

build/RDNA4Framebuffer.o: src/RDNA4Framebuffer.cpp
	@mkdir -p build
	@echo "Compiling RDNA4Framebuffer.cpp..."
	$(CXX) $(CXXFLAGS) -c src/RDNA4Framebuffer.cpp -o build/RDNA4Framebuffer.o

build/kmod_info.o: src/kmod_info.c
	@mkdir -p build
	@echo "Compiling kmod_info.c..."
	$(CC) $(CFLAGS) -c src/kmod_info.c -o build/kmod_info.o

clean:
	@echo "Cleaning..."
	rm -rf build

.PHONY: all clean
