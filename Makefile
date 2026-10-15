# airTime: native Haiku build.
#   make              builds build-haiku/airTime
#   make package      builds artifacts/airtime-<version>-<arch>.hpkg
#   make check-host   builds and runs the platform independent tests (Linux)
#
# FFmpeg comes from the system's ffmpeg6 and ffmpeg6_devel packages. Where
# the devel package cannot be installed, point FFMPEG_CFLAGS and
# FFMPEG_LDFLAGS at extracted headers and at a directory of links to the
# libraries (tools/setup-ffmpeg-devel.sh makes one).
.DEFAULT_GOAL := all
CXX ?= g++
BUILD ?= build-haiku
RC ?= rc
XRES ?= xres
MIMESET ?= mimeset
HAIKU_HEADERS ?= /boot/system/develop/headers

FFMPEG_CFLAGS ?=
FFMPEG_LDFLAGS ?=
FFMPEG_LIBS ?= -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil

CPPFLAGS += -Isrc -Isrc/engine -Isrc/ui \
	-I$(HAIKU_HEADERS)/private/media \
	-I$(HAIKU_HEADERS)/private/shared \
	-I$(HAIKU_HEADERS)/private/interface \
	$(FFMPEG_CFLAGS) $(CROSS_CPPFLAGS)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-multichar -Wno-unused-parameter \
	-Wno-sign-compare -Wno-missing-field-initializers -pthread

ENGINE_SRC = $(wildcard src/engine/*.cpp)
UI_SRC = $(wildcard src/ui/*.cpp) $(wildcard src/*.cpp)
APP_OBJ = $(ENGINE_SRC:%.cpp=$(BUILD)/%.o) $(UI_SRC:%.cpp=$(BUILD)/%.o)
APP_LIBS = -lbe -lgame -lmedia -ltracker -ltranslation -llocalestub $(FFMPEG_LIBS)

# The Quick Look add-on (src/quicklook): the engine and the video view built
# again as position independent code, for the quick_look_server to load.
# It needs air/OS's Quick Look headers and libquicklook; where the SDK is
# an older Haiku without them the add-on is left out. QUICKLOOK=1 forces
# it, with QUICKLOOK_CPPFLAGS and QUICKLOOK_LDFLAGS pointing at them.
QUICKLOOK_CPPFLAGS ?=
QUICKLOOK_LDFLAGS ?=
QUICKLOOK ?= $(if $(wildcard $(HAIKU_HEADERS)/os/add-ons/quick_look/QuickLook.h),1,)
QUICKLOOK_UI_SRC = $(filter-out src/ui/PlayerWindow.cpp src/ui/GoToTimeWindow.cpp \
	src/ui/InspectorWindow.cpp src/ui/DirectRescue.cpp,$(wildcard src/ui/*.cpp))
QUICKLOOK_SRC = $(ENGINE_SRC) $(QUICKLOOK_UI_SRC) src/Settings.cpp \
	src/quicklook/QuickLookAddOn.cpp
QUICKLOOK_OBJ = $(QUICKLOOK_SRC:%.cpp=$(BUILD)/quicklook/%.o)
QUICKLOOK_TARGET = $(if $(QUICKLOOK),$(BUILD)/airTime_quicklook,)

.PHONY: all engine package clean check check-host icon quicklook

all: $(BUILD)/airTime $(QUICKLOOK_TARGET)

quicklook: $(BUILD)/airTime_quicklook

$(BUILD)/airTime_quicklook: $(QUICKLOOK_OBJ)
	$(CXX) -shared -Wl,-soname=airTime -pthread -o $@.new $(QUICKLOOK_OBJ) \
		$(FFMPEG_LDFLAGS) $(QUICKLOOK_LDFLAGS) -lquicklook $(APP_LIBS) $(APP_LDEND)
	mv $@.new $@

$(BUILD)/quicklook/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(QUICKLOOK_CPPFLAGS) $(CXXFLAGS) -fPIC -MMD -MP -c $< -o $@

-include $(QUICKLOOK_OBJ:.o=.d)

engine: $(ENGINE_SRC:%.cpp=$(BUILD)/%.o)

$(BUILD)/airTime: $(APP_OBJ) resources/airTime.rdef resources/branding/airtime-icon.hvif
	$(CXX) $(APP_LDFLAGS) -pthread -o $@.new $(APP_OBJ) $(FFMPEG_LDFLAGS) $(APP_LIBS) $(APP_LDEND)
	$(RC) -o $(BUILD)/airTime.rsrc resources/airTime.rdef
	$(XRES) -o $@.new $(BUILD)/airTime.rsrc
	$(MIMESET) -f $@.new
	mv $@.new $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(APP_OBJ:.o=.d)

# Tests of the parts that do not need Haiku: time and language helpers,
# subtitle markup, caption extraction, the ten-bit scaler. They build against the host's FFmpeg
# and a few stand-ins for Haiku types (tests/host).
HOST_CXX ?= g++
HOST_BUILD ?= build-host
HOST_FFMPEG_CFLAGS ?= $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale 2>/dev/null \
	|| echo -I/mnt/HaikuWork/artifacts/ffmpeg-arm64/stage/boot/system/non-packaged/include)
HOST_FFMPEG_LIBS ?= $(shell pkg-config --libs libavformat libavcodec libavutil libswscale 2>/dev/null \
	|| echo -l:libavformat.so.60 -l:libavcodec.so.60 -l:libavutil.so.58 -l:libswscale.so.7)
TEST_SRC = tests/EngineTests.cpp src/engine/Tracks.cpp src/engine/Languages.cpp \
	src/engine/Bitstream.cpp src/engine/Subtitles.cpp src/ui/YuvScaler.cpp
$(HOST_BUILD)/engine_tests: $(TEST_SRC) $(wildcard tests/host/*.h) $(wildcard src/engine/*.h) \
		src/ui/YuvScaler.h
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) -std=c++17 -O1 -g -Wall -Wno-multichar -Itests/host -Isrc/engine -Isrc/ui \
		$(HOST_FFMPEG_CFLAGS) -o $@ $(TEST_SRC) $(HOST_FFMPEG_LIBS) -pthread

$(HOST_BUILD)/caption_dump: tests/CaptionDump.cpp src/engine/Subtitles.cpp \
		src/engine/Bitstream.cpp src/engine/Tracks.cpp src/engine/Languages.cpp
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) -std=c++17 -O1 -g -Itests/host -Isrc/engine $(HOST_FFMPEG_CFLAGS) \
		-o $@ $^ $(HOST_FFMPEG_LIBS) -pthread

$(HOST_BUILD)/renderer_tests: tests/RendererTests.cpp src/ui/FrameRenderer.cpp \
		src/ui/YuvScaler.cpp src/ui/SdrScaler.cpp $(wildcard src/ui/*.h) $(wildcard tests/host/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) -std=c++17 -O2 -g -Wall -Wno-multichar -Itests/host -Isrc/engine -Isrc/ui \
		$(HOST_FFMPEG_CFLAGS) -o $@ tests/RendererTests.cpp src/ui/FrameRenderer.cpp \
		src/ui/YuvScaler.cpp src/ui/SdrScaler.cpp $(HOST_FFMPEG_LIBS) -pthread

AIRTIME_TEST_MEDIA ?= $(wildcard /mnt/HaikuWork/artifacts/airtime-media)
export AIRTIME_TEST_MEDIA

check-host: $(HOST_BUILD)/engine_tests $(HOST_BUILD)/caption_dump \
		$(HOST_BUILD)/renderer_tests
	$(HOST_BUILD)/engine_tests
	$(HOST_BUILD)/renderer_tests 1

check: check-host

icon:
	python3 tools/make-icon.py resources/branding/airtime-icon.hvif resources/branding/airtime-icon-preview.png

package: all
	tools/package-haiku.sh

clean:
	rm -rf $(BUILD) $(HOST_BUILD)
