{ pkgs ? import (builtins.fetchTree (builtins.fromJSON (builtins.readFile ./nixpkgs.json))) { system = builtins.currentSystem; }
, source ? ../..
, revision ? "development"
}:
let
  inherit (pkgs) lib;
  static = pkgs.pkgsStatic;
  solo = static.stdenv.mkDerivation {
    pname = "solo";
    version = "ac74ba96895e76688994dbb35ef3bfae81a0541f";
    src = pkgs.fetchFromGitHub {
      owner = "pg83";
      repo = "solo";
      rev = "ac74ba96895e76688994dbb35ef3bfae81a0541f";
      sha256 = "1p3yzd1mpzi3rb6jzim3pkk9ax9vp249ih4k4q1rzbjw47cb9hdm";
    };
    nativeBuildInputs = [ pkgs.python3 ];
    dontConfigure = true;
    buildPhase = ''
      export XDG_CACHE_HOME=$TMPDIR/cache
      python3 ./build -j "$NIX_BUILD_CORES"
    '';
    installPhase = ''
      mkdir -p $out/lib $out/include/solo $out/share/licenses/solo
      cp -L dlfcn $out/lib/libdlfcn.a
      cp lib/dlfcn.h $out/include/solo/
      cp LICENSE $out/share/licenses/solo/
    '';
  };
  epoxy = (static.libepoxy.override { libGL = pkgs.libglvnd.dev; }).overrideAttrs (old: {
    patches = [];
    env.NIX_CFLAGS_COMPILE = "-include ${./solo-dlfcn.h} -I${solo}/include -I${pkgs.libglvnd.dev}/include";
    propagatedBuildInputs = [ static.libx11 ];
    doCheck = false;
  });
  pulse = static.stdenv.mkDerivation {
    pname = "libpulseaudio-static";
    version = pkgs.pulseaudio.version;
    src = pkgs.pulseaudio.src;
    nativeBuildInputs = [ pkgs.meson pkgs.ninja pkgs.pkg-config pkgs.m4 ];
    buildInputs = [ (static.libsndfile.overrideAttrs (old: { configureFlags = (old.configureFlags or []) ++ [ "--disable-external-libs" "--disable-mpeg" ]; })) static.gettext ];
    postPatch = ''
      substituteInPlace src/meson.build src/pulse/meson.build \
        --replace-fail "shared_library(" "library("
      substituteInPlace meson.build --replace-fail \
        "cc.has_function('dgettext')" "cc.has_function('dgettext', prefix : '#include <libintl.h>')"
    '';
    mesonFlags = [ "-Ddefault_library=static" "-Dauto_features=disabled" "-Dprefer_static=true"
      "-Ddaemon=false" "-Dclient=true" "-Dtests=false" "-Ddoxygen=false"
      "-Dman=false" "-Ddatabase=simple" "--sysconfdir=/etc" "--localstatedir=/var" ];
    installPhase = ''
      meson install --destdir "$TMPDIR/staging"
      mkdir -p $out
      cp -a "$TMPDIR/staging/$out/." $out/
    '';
  };
  client = static.stdenv.mkDerivation {
    pname = "looking-glass-client-static";
    version = revision;
    src = lib.cleanSourceWith {
      src = source;
      filter = path: type: lib.cleanSourceFilter path type && !(builtins.elem (baseNameOf path) [ "build" "out" "result" "__pycache__" ]);
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.wayland-scanner pkgs.python3 pkgs.binutils ];
    buildInputs = with static; [
      fontconfig fuse3 libx11 libxi libxfixes libxscrnsaver libxinerama libxcursor
      libxpresent libxrandr libxkbcommon wayland nettle gmp libsamplerate
    ] ++ [ pkgs.spice-protocol epoxy solo pulse pkgs.libglvnd.dev ];
    env.NIX_CFLAGS_COMPILE = "-I${solo}/include";
    preConfigure = ''
      echo '${revision}' > VERSION
      cmakeDir="$PWD/client"
      python3 packaging/generate-providers.py client/src/solo-providers.c \
        libwayland-client.so.0=${lib.getLib static.wayland}/lib/libwayland-client.a \
        libwayland-egl.so.1=${lib.getLib static.wayland}/lib/libwayland-egl.a \
        libX11.so.6=${lib.getLib static.libx11}/lib/libX11.a \
        libX11-xcb.so.1=${lib.getLib static.libx11}/lib/libX11-xcb.a \
        libxcb.so.1=${lib.getLib static.libxcb}/lib/libxcb.a
    '';
    cmakeFlags = [
      "-DCMAKE_BUILD_TYPE=Release"
      "-DCMAKE_POLICY_VERSION_MINIMUM=3.10"
      "-DPKG_CONFIG_ARGN=--static"
      "-DOPTIMIZE_FOR_NATIVE=OFF"
      "-DENABLE_OPENGL=OFF"
      "-DENABLE_EPOXY=ON"
      "-DENABLE_BACKTRACE=OFF"
      "-DENABLE_PIPEWIRE=OFF"
      "-DENABLE_LIBDECOR=OFF"
      "-DENABLE_USB_AUDIO=OFF"
      "-DCMAKE_EXE_LINKER_FLAGS=-static"
      "-DSOLO_LIBRARY=${solo}/lib/libdlfcn.a"
    ];
    enableParallelBuilding = true;
    dontStrip = true;
    postInstall = ''
      if ${pkgs.binutils}/bin/readelf -lW $out/bin/looking-glass-client | grep -q INTERP; then
        echo "The client has an ELF interpreter" >&2; exit 1
      fi
      if ${pkgs.binutils}/bin/readelf -dW $out/bin/looking-glass-client | grep -q NEEDED; then
        echo "The client has shared-library dependencies" >&2; exit 1
      fi
      mkdir -p "$TMPDIR/config" "$TMPDIR/data"
      XDG_CONFIG_HOME="$TMPDIR/config" XDG_DATA_HOME="$TMPDIR/data" \
        $out/bin/looking-glass-client --help > $out/help.txt
      mkdir -p $out/share/licenses/LookingGlass
      cp ../LICENSE $out/share/licenses/LookingGlass/
    '';
  };
  dependencyClosure = builtins.genericClosure {
    startSet = map (drv: { key = drv.outPath; inherit drv; }) (client.buildInputs ++ [ static.stdenv.cc.cc static.stdenv.cc.libc ]);
    operator = item: map (drv: { key = drv.outPath; inherit drv; })
      (builtins.filter lib.isDerivation ((item.drv.buildInputs or []) ++ (item.drv.propagatedBuildInputs or [])));
  };
  sourceInputs = pkgs.runCommand "looking-glass-sources" {} (lib.concatMapStringsSep "\n" (item:
    let drv = item.drv;
    in lib.optionalString (drv ? src) ''
      mkdir -p $out/${drv.name}
      cp -rL ${drv.src} $out/${drv.name}/source
      ${lib.concatImapStringsSep "\n" (i: patch: "cp ${patch} $out/${drv.name}/patch-${toString i}") (builtins.filter (p: builtins.isPath p || lib.isDerivation p || builtins.isString p) (drv.patches or []))}
    ''
  ) dependencyClosure);
in { inherit client solo epoxy sourceInputs; }
