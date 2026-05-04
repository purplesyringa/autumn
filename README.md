# autumn

> Looking for a Wasm interpreter? Try `autumn` — it's slow, it's insecure, and it's feature-incomplete.

WebAssembly interpreter that fits in a QR code, taking 2945 bytes:

![QR code](autumn.png)

This is a static x86-64 Linux executable. Scan the QR code with `zbarimg --raw -Sbinary` or another QR decoder that supports binary data, or [directly download the program](https://github.com/purplesyringa/autumn/raw/refs/heads/master/autumn). Run it with `./autumn <path to wasm file>`. Example programs: [a guessing game](https://github.com/purplesyringa/autumn/raw/refs/heads/master/tests/guessing-game/guessing-game.wasm) and [QuickJS](https://github.com/quickjs-ng/quickjs/releases/download/v0.14.0/qjs-wasi.wasm).

How this works: [a blog post](https://purplesyringa.moe/blog/this-wasm-interpreter-fits-in-a-qr-code/).

Since compilers are finicky, there's a nix package for reproducibility.

There are at least three dozen intended CVEs in this project. It is licensed under AGPL to motivate you not to run it in prod.


## 📁 Project Structure

```
autumn/
└── interp.c
```
