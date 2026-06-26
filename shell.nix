{ pkgs ? import <nixpkgs> {} }:

# 32-bit dev shell for building an LD_PRELOAD overlay that injects into
# X3: Terran Conflict (a 32-bit i386 ELF running inside the Steam Runtime).
#
# Use pkgsi686Linux so $CC / pkg-config target i686 natively (no -m32 multilib).
let
  p = pkgs.pkgsi686Linux;
in
p.mkShell {
  nativeBuildInputs = [ p.pkg-config pkgs.patchelf ];
  buildInputs = [
    p.SDL2          # SDL.h + libSDL2  (we interpose SDL_GL_SwapWindow / SDL_PeepEvents)
    p.libGL         # GL/gl.h + libGL  (ImGui opengl2 backend renders through this)
    p.glew
    p.xorg.libX11   # X11/Xlib.h — pulled in by SDL_syswm.h via imgui_impl_sdl2
    p.xorg.xorgproto
  ];

  # Sanity banner when entering the shell.
  shellHook = ''
    echo "[x3ui] 32-bit shell:  CC=$CC"
    echo "[x3ui] target:        $($CC -dumpmachine)"
  '';
}
