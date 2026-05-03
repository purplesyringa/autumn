# SPDX-FileCopyrightText: 2026 Yuki Sireneva
#
# SPDX-License-Identifier: CC0-1.0 OR Unlicense

{
  stdenv,
  lib,
  python3,
  cargo,
  nasm,
  qrencode,
}:
stdenv.mkDerivation {
  __structuredAttrs = true;
  name = "autumn";
  src = ./.;
  outputs = [
    "out"
    "unwrapped"
    "png"
  ];
  hardeningDisable = ["all"]; # YOLO
  nativeBuildInputs = [
    python3
    cargo
    nasm
    qrencode
  ];
  buildPhase = ''
    runHook preBuild
    make autumn
    make autumn-unwrapped
    make autumn.png
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm0755 autumn $out/bin/autumn
    install -Dm0755 autumn-unwrapped $unwrapped/bin/autumn-unwrapped
    install -Dm0644 autumn.png $png/share/autumn.png
    runHook postInstall
  '';
  meta = {
    description = "Tiny WASM interpreter";
    # license = ?
    platforms = ["x86_64-linux"];
    homepage = "https://github.com/purplesyringa/autumn";
    mainProgram = "autumn";
  };
}
