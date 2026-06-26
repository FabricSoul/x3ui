# x3ui overlay — build a 32-bit LD_PRELOAD .so.
# MUST be run inside the 32-bit nix-shell:   nix-shell --run 'make'
# ($CC there is i686-unknown-linux-gnu; pkg-config finds 32-bit SDL2/GL.)

IMGUI    := third_party/imgui
BUILD    := build
TARGET   := libx3ui.so

CXX      ?= g++

# Dear ImGui core + SDL2 platform backend + OpenGL2 renderer backend (X3 is GL 2.1).
IMGUI_SRC := \
	$(IMGUI)/imgui.cpp \
	$(IMGUI)/imgui_draw.cpp \
	$(IMGUI)/imgui_tables.cpp \
	$(IMGUI)/imgui_widgets.cpp \
	$(IMGUI)/backends/imgui_impl_sdl2.cpp \
	$(IMGUI)/backends/imgui_impl_opengl3.cpp

SRC      := src/overlay.cpp src/sector_reader.cpp $(IMGUI_SRC)
OBJ      := $(patsubst %.cpp,$(BUILD)/%.o,$(SRC))

CXXFLAGS := -std=c++17 -O2 -fPIC -Wall \
	-I$(IMGUI) -I$(IMGUI)/backends \
	$(shell pkg-config --cflags sdl2) \
	$(shell pkg-config --cflags gl 2>/dev/null)

# -static-libstdc++/-static-libgcc: don't depend on the container's (older)
# libstdc++ ABI. We keep dynamic links only to soname'd SDL2/GL/libc, which the
# game already provides at runtime.
LDFLAGS  := -shared -static-libstdc++ -static-libgcc \
	$(shell pkg-config --libs sdl2) -lGL -ldl

.PHONY: all clean inspect test
all: $(TARGET)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@
	@# Strip the nix /nix/store RUNPATH so the loader resolves libs inside the
	@# Steam Runtime container instead of dead nix paths.
	patchelf --remove-rpath $(TARGET)
	@echo "=== built $(TARGET) ==="
	@$(MAKE) --no-print-directory inspect

inspect:
	@file $(TARGET)
	@echo "--- DT_NEEDED (want bare sonames only) ---"
	@readelf -d $(TARGET) | grep -E "NEEDED|RUNPATH|RPATH" || true
	@echo "--- our interposed symbols must be GLOBAL DEFAULT ---"
	@readelf --dyn-syms $(TARGET) | grep -E "SDL_GL_SwapWindow|SDL_PeepEvents" || true
	@echo "--- highest glibc symbol version we reference (vs container glibc) ---"
	@readelf -V $(TARGET) 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -5 || true

clean:
	rm -rf $(BUILD) $(TARGET)

# Host unit tests for the pure walk logic (no SDL/GL/ImGui). Runs in or out of
# the nix-shell — sector_reader is arch-independent (fixed-width types only).
test:
	@mkdir -p $(BUILD)
	g++ -std=c++17 -I src -O0 -g \
		src/sector_reader.cpp tests/test_sector_reader.cpp \
		-o $(BUILD)/test_sector_reader
	$(BUILD)/test_sector_reader
