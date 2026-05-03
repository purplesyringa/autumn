# SPDX-FileCopyrightText: 2026 Yuki Sireneva
#
# SPDX-License-Identifier: CC0-1.0 OR Unlicense

{
  nixpkgs ? builtins.fetchTarball {
    # curl -i https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz | grep location
    url = "https://releases.nixos.org/nixos/unstable/nixos-26.05pre987561.1c3fe55ad329/nixexprs.tar.xz";
    sha256 = "sha256-e1tDUQMbFCxCnke314UpghgRqg3FJLtcXFfq/WTRLYI=";
  },
  pkgs ? import nixpkgs {
    system = "x86_64-linux";
  },
}:
pkgs.callPackage ./package.nix {}
