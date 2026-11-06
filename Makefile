# vsemvamdam-switcher — build with MinGW-w64 (cross from Linux or natively in MSYS2)
#   make            -> build/vsemvamdam-switcher.exe
#   make test       -> run portable unit tests on the host
#   make wintest    -> build the Windows integration test (run under Wine or Windows)

ifeq ($(origin CXX),default)
  CXX := x86_64-w64-mingw32-g++-posix
endif
WINDRES  ?= x86_64-w64-mingw32-windres
HOSTCXX  ?= g++
OUT      := build
EXE      := $(OUT)/vsemvamdam-switcher.exe

ifeq ($(shell command -v $(CXX) 2>/dev/null),)
  CXX := x86_64-w64-mingw32-g++
endif

CXXFLAGS := -std=c++20 -O2 -DNDEBUG -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
            -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_USE_WCHAR32 \
            -Ithird_party -Ithird_party/imgui -Ithird_party/imgui/backends -Ithird_party/stb \
            -Wall -Wno-unused-function -Wno-missing-field-initializers -ffunction-sections -fdata-sections
LDFLAGS  := -static -municode -mwindows -s -Wl,--gc-sections
LIBS     := -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lshcore -lwinhttp -lshell32 -lshlwapi -lole32 -loleaut32 \
            -luuid -ladvapi32 -luser32 -lgdi32 -limm32 -lcomctl32 -lcomdlg32 -lcrypt32

CORE_SRC := src/core/str.cpp src/core/vdf.cpp src/core/fsx.cpp src/core/image.cpp src/core/steam_core.cpp \
            src/core/log.cpp src/core/settings.cpp src/core/i18n.cpp src/core/basic_core.cpp
APP_SRC  := src/main.cpp src/os/win.cpp src/app/app.cpp src/app/app_ui.cpp src/app/app_settings.cpp \
            src/app/steam_platform.cpp src/app/basic_platform.cpp src/app/platform.cpp src/ui/theme.cpp src/ui/widgets.cpp src/ui/textures.cpp
IMGUI_SRC := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp \
             third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_win32.cpp \
             third_party/imgui/backends/imgui_impl_dx11.cpp

OBJ := $(patsubst %.cpp,$(OUT)/obj/%.o,$(CORE_SRC) $(APP_SRC) $(IMGUI_SRC)) $(OUT)/obj/app_res.o

all: $(EXE)

$(OUT)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/app_res.o: res/app.rc res/app.manifest res/app.ico src/version.h src/core/platforms.json res/fonts/*.ttf
	@mkdir -p $(dir $@)
	$(WINDRES) -I res -O coff res/app.rc -o $@

$(EXE): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)
	@ls -la $@

# ---- portable unit tests (host compiler)
TEST_SRC := tests/test_core.cpp src/core/str.cpp src/core/vdf.cpp src/core/fsx.cpp src/core/image.cpp \
            src/core/steam_core.cpp src/core/log.cpp src/core/basic_core.cpp
test:
	@mkdir -p $(OUT)
	$(HOSTCXX) -std=c++20 -O1 -g -Wall -Ithird_party -Ithird_party/stb $(TEST_SRC) -o $(OUT)/test_core
	./$(OUT)/test_core

# ---- Windows integration test (exercises registry, processes, files) — run with: wine build/wintest.exe
wintest:
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) tests/test_win.cpp src/os/win.cpp src/core/*.cpp src/app/steam_platform.cpp src/app/basic_platform.cpp src/app/platform.cpp \
	  tests/stub_host.cpp -o $(OUT)/wintest.exe -static -municode $(LIBS) -lcomdlg32 -lcrypt32
	$(CXX) -O2 -static tests/fake_client.cpp -o $(OUT)/fake_client.exe

clean:
	rm -rf $(OUT)

-include $(OBJ:.o=.d)
.PHONY: all test wintest clean
