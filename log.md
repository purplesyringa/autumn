Let's try something. I'm thinking using assembly directly isn't a good idea, since I won't be able to debug OOB accesses. So I'll get some familiarity with the binary wasm format first. I know how code looks, approximately, and the general shape of a wasm module, but I don't yet know which specific details I need to parse and which ones I can skip. So I'll make a C prototype first to learn that.

I also don't know which API to use for the module yet. I'm thinking WASI 0.1 (component model is too complex for the purposes of this project) and just compile a few simple programs and see what they import.

Hmm, simple programs... maybe a Rust hello-world?

```
cargo new hello-world
cd hello-world
cargo build --target wasm32-wasip1 --release
wasm-dis target/wasm32-wasip1/release/hello-world.wasm
```

Here's the imports:

```
(import "wasi_snapshot_preview1" "environ_get" (func $__imported_wasi_snapshot_preview1_environ_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "environ_sizes_get" (func $__imported_wasi_snapshot_preview1_environ_sizes_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "fd_write" (func $__imported_wasi_snapshot_preview1_fd_write (param i32 i32 i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "proc_exit" (func $__imported_wasi_snapshot_preview1_proc_exit (param i32)))
```

This feels reasonable. I think I can implement this.

I'm going to read the file from `argv[1]`. I'm already thinking about syscalls. I could `open` + `read`, but I wouldn't know how much data to read, so I wouldn't know how much memory to allocate. I guess I could just make a large static allocation and use the return value of `read` as the size (modules are not null-terminated or anything).

Looking at the docs, parsing sections already requires parsing `u32`, which is a LEB128 varint. The good news is that it uses the same format for all data types (at least unsigned ones), so I can use a single `read_uint` function. The bad news is that it's a function call and not just a memory access.
