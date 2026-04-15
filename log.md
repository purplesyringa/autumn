Let's try something. I'm thinking using assembly directly isn't a good idea, since I won't be able to debug OOB accesses. So I'll get some familiarity with the binary wasm format first. I know how code looks, approximately, and the general shape of a wasm module, but I don't yet know which specific details I need to parse and which ones I can skip. So I'll make a C prototype first to learn that.

I also don't know which API to use for the module yet. I'm thinking WASI 0.1 (component model is too complex for the purposes of this project) and just compile a few simple programs and see what they import.

Hmm, simple programs... maybe a Rust hello-world?

```shell
cargo new hello-world
cd hello-world
cargo build --target wasm32-wasip1 --release
wasm-dis target/wasm32-wasip1/release/hello-world.wasm
```

Here's the imports:

```wast
(import "wasi_snapshot_preview1" "environ_get" (func $__imported_wasi_snapshot_preview1_environ_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "environ_sizes_get" (func $__imported_wasi_snapshot_preview1_environ_sizes_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "fd_write" (func $__imported_wasi_snapshot_preview1_fd_write (param i32 i32 i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "proc_exit" (func $__imported_wasi_snapshot_preview1_proc_exit (param i32)))
```

This feels reasonable. I think I can implement this.

I'm going to read the file from `argv[1]`. I'm already thinking about syscalls. I could `open` + `read`, but I wouldn't know how much data to read, so I wouldn't know how much memory to allocate. I guess I could just make a large static allocation and use the return value of `read` as the size (modules are not null-terminated or anything).

Looking at the docs, parsing sections already requires parsing `u32`, which is a LEB128 varint. The good news is that it uses the same format for all data types (at least unsigned ones), so I can use a single `read_uint` function. The bad news is that it's a function call and not just a memory access.

---

Looks like wasm promises that we'll encounter sections in a specific order. That simplifies parsing, so that's cool.

Let me open wast side-by-side:

```
wasm-dis hello-world/target/wasm32-wasip1/release/hello-world.wasm >dis
subl dis
```

The type section looks pretty important. I'm getting a little confused by the recursive type/subtype dichotomy. I don't quite get the recursive part -- the text syntax doesn't seem to allow types to refer to each other.

Side note: I tried to search for `typeidx` because I saw it in the docs but didn't find matches. One Ctrl-C/Ctrl-V later, I got `𝚝𝚢𝚙𝚎𝚒𝚍𝚡`. So they're using Unicode instead of styling. Wow.

https://stackoverflow.com/questions/77472803/how-to-understand-the-recursive-types-in-wasm

Okay, I'm starting to piece it together. Looking at `typeuse`, it's used for heap types, tag types, and external types. Okay. I'm thinking I'll just skip over parsing types for now, simply saving pointers to a global `typeidx`-indexed array. We'll get to it later.

Okay, even skipping types is non-trivial because they're complex. I just got to `heaptype` and I don't know what to do about it. Its production rule is `heaptype ::= absheaptype | s33`, and I don't see how the two can be disambiguated. Oh, wait, now I see -- the `s33` is documented to be `>= 0`, and all the bytes valid for `absheaptype` would correspond to negative integers. I guess, eugh.

But do I even need to implement heap types? Isn't that outside of the original wasm specification?

> Reference types classify values that are first-class references to objects in the runtime store.

This reads like Wasm GC. https://github.com/WebAssembly/reference-types Here's Wasm 1.0: https://www.w3.org/TR/wasm-core-1/ Okay, yeah, that looks SO much simpler. For example, the type section is said to contain a list of `functype`s, not any other types. I don't know if Rust still supports Wasm 1.0, but I only see `functype`s in the wast anyway, so probably?

---

The next section is the import section. Looks valuable. We can skip over the module name, since it should always be `wasi_snapshot_preview1` and it's a long constant to hard-code. I might use the length of the function name as key instead of the function name as well, just to laugh at PHP, but I probably shouldn't do that yet.

I also only want to support `func` imports, not `table`/`mem`/`global`. This is really cool, actually: `importdesc` uses 0x00 for the `func` variant, and it's placed directly after the name, so `func` names are effectively automatically null-terminated. Cool!

I'm not going to actually save anything yet, we'll see how imports are used later.

---

The next section is the function section. The first section we can (probably) skip! Looks like it just describes the function signatures, so it's only useful for type checking.

The next section is the table section. I *think* we can also skip this one? Wasm 1.0 says there's only one table and it's always of `funcref`s, so it's not like there's anything to parse.

The next section is the memory section. There can be only one memory in Wasm 1.0, so the only useful thing to extract here is the minimum and maxium size of the linear memory. We don't need to track the high boundary. Technically we don't need to track the low boundary either, we can just hardcode like 1 MiB and it'll work fine I guess. So we don't need this section either.

The next section is the global section. This one contains values, so it's finally something we need to parse. The issue is that it contains initializers, which is code. Though the verification section says it must be a *constant* expression, which basically just means it needs to be a single `t.const` instruction. That simplifies things to say the least!

---

Time for the export section. Pretty much the only thing I need to do is find the `_start` symbol.

Next goes the... start section? Except it's absent from my file. What?

Okay, so `_start` is part of the WASI ABI, while the start section is effectively autorun. I guess it can also be interpreted as CRT? I'll treat `_start` as the `main` function and call the start function simply `start`.

---

Element section. It's basically an initializer list for tables. When are tables used? Looks like the (sole) function table is only used for `call_indirect`. So function pointers are indices into the table, I guess. It kinda sucks that it doesn't just use `funcidx` directly, but I guess it makes some sense.

---

Finally, code section! I won't parse code itself yet, though, since there's a `size` field to skip over function bodies. I'll just save pointers for now.

Next is the data section. It's basically initializers for memory, as far as I can tell. I'm surprised that `wasm-dis` shows it at the top of the file, rather than at the bottom where it should be... but whatever.

I'm a little surprised that the offsets are as large as 1 MiB, when the memory size is 17 pages. Wait, how large are pages again? It's 64 KiB, larger than I expected. Huh. Well, let's just hard-code 2 MiB for now.

---

This leaves custom sections. My file has 3 custom sections, but wast lists only two. The unlisted one is large, so I guess that's symbols? The remaining two sections are "producers" (probably LLVM version and stuff) and the features section. It's not documented in Wasm 1.0, and nor is it documented in Wasm 3.0, but there's this: https://github.com/WebAssembly/tool-conventions/blob/main/Linking.md#target-features-section.

I don't really need to parse it, I can just crash if an instruction is unimplemented. But it's useful to know which features I need to support in the first place:

```
;; features section: mutable-globals, nontrapping-float-to-int, bulk-memory, sign-ext, reference-types, multivalue, extended-const, bulk-memory-opt, call-indirect-overlong
```

---

So I guess it's time to start interpreting code. Scary!

I don't think we need per-frame stacks. We can just use a single stack. The calls remain a little ugly, since we need to load data from the stack to locals, but e.g. returns don't need to touch anything at all. Looks fine to me.

The stack should probably just be a list of `u64`s, reinterpreted as necessary. I wonder if it's better for the stack to grow upwards or downwards... probably upwards -- as far as I can tell, `(i32.const 1) (i32.const 2) (call $f)` uses `1` as the first argument and `2` as the second one, so it's better if we can just `memcpy` the top of the stack to locals.

Hmm, I just realized: how do Wasm programs invoke external functions? I probably interprted `funcidx`s wrong somewhere.

> The index space for functions, tables, memories and globals includes respective imports declared in the same module. The indices of these imports precede the indices of other definitions in the same index space.

Yeah, it should use continuous numbering.

---

Hopefully now I can interpret code. Shouldn't be too hard.

My basic function is `eval_until(terminator)`, evaluating bytecode from a global pointer until `terminator`. Using a global pointer allows me to just call `read_uint` when necessary, but it does mean I need to be careful to place the pointer carefully at the end of each instruction and before invoking `eval_until` (e.g. in the `loop` instruction).

I think I'll just implement instructions greedily for now and add functionality when necessary.

I should probably enable `-Wall` -- I forget `break` in `switch`..`case` all the time.

Memory accesses are weird:

> The static address offset is added to the dynamic address operand, yielding a 33 bit effective address that is the zero-based index at which the memory is accessed.

So apparently `i32.load` takes an inline static offset, *and* a dynamic argument on the stacak, and sums them together. What I don't get is why I see

```
(i32.load
 (i32.add
  (global.get $GOT.data.internal.__memory_base)
  (i32.const 1055864)
 )
)
```

in wast then. I would've expected `i32.add` to be inlined into `i32.load` in this case. Maybe that's just how `wasm-dis` renders this?.. That'd be odd, the spec says the text format can use `offset=...` for a fixed offset. Is it perhaps due to different behavior when the addition overflows?

https://github.com/emscripten-core/emscripten/issues/7715 (closed by stale bot)

Here's a relevant PR to Emscripten: https://github.com/emscripten-core/emscripten/pull/7860

Is there an open LLVM issue? https://github.com/llvm/llvm-project/issues/187367

Oh, I see `offset=...` in other places, I guess it's just this specific one that's broken due to a large offset. Okay.

---

Encountered `br_if`, this one requires labels. Time to read up on how they work.

Labels are created when entering `block`, `loop`, and `if`, and are removed when exiting. De Bruijn indexing is used, with 0 for the innermost block-like instruction.

Branching to the label of a `block`/`if` jumps to after it, branching to `loop` jumps to its start. A consistent way to interpret this is that we always jump to the end of the body, and that's probably easier to simulate. I think we can make `eval_until` return an integer that denotes how many levels we should break for. And then ignore instructions if the current block should be broken -- but still parse them, since we don't know where the current block ends and what to set `parse_p` to.

Eugh. Ugly! We effectively have to gate each instruction behind an `if`. *And* we have to thread this flag into recursive `eval_until` invocations to skip over blocks. That sucks, but oh well, it'll have to do for now. *Maybe* we could separate parsing and evaluation at some point, but probably not yet.

Okay, thought: use a global integer as a flag for breaking from blocks instead of threading it through every function call. That's becoming bearable, actually, e.g. here's how `block` is implemented:

```c
case 0x02: {
    // block
    parse_p++; // blocktype
    _Bool executed = broken_blocks == 0;
    eval_until(0x0b);
    broken_blocks -= executed && broken_blocks > 0;
    break;
}
```

It's kind of neat. Ugly but neat.

---

Added a few more instrs (instr is short for instruction), stumbled upon `call` and realized my implementation of `br_if` might be wrong? Branching to a loop is supposed to be like `continue`, while I treat it as `break`. Basically, 0 breaks from a loop body and 1 break from a loop body should be treated the same, but not a greater amount.

---

Got to `call` and realized that, since I need to transfer arguments from the stack to locals, I do need to parse the function section, since the code section doesn't contain signatures. Also realized I forgot `-Wextra`.

---

Implementing more instructions. Stumbled across `f64.abs`. I have a feeling some of the floating-point instructions are going to be nasty... Though `fnn.abs` in particular is luckily trivial.

Implemented `if..end` (without `else`), it was pretty weird. Not as weird as `return`, though. I think the best way to implement `return` is to treat it as an infinite `br`, and reset `broken_blocks` to `0` at the end of the function. I think `-1U` works; I don't think I ever need to increment `broken_blocks` if it's already `> 0`, and using `-1` saves on code space.

---

Got to `i32.load8_u`. What does that even mean? I think it's `movzbl`, but otherwise like `i32.load`. That's my first instruction that reuses existing code!

Though the next one is pretty close -- it's `i32.eq`, whereas I already implemented `i32.ne` and, in fact, some other comparisons.

---

Got to `br_table`. It doesn't refer to the function table, it's just an inline jump table. I guess that makes sense...

Implemented `i64.rem_s` and promptly got SIGFPE because I forgot `PARSED`. Fun.

---

Well, that's interesting. I got the `0xec` opcode, and it's not documented in the Wasm 1.0 spec. Maybe later ones? Nope, Wasm 3.0 marks it as reserved too. Do I have a bug? That'd be rather unfortunate.

I couldn't find any obvious bug, so I guess it's time to add some logging.

Nevermind, got it pretty quickly after adding logs -- I forgot to reset `parse_p` after `call_func`. That's much better, now I just get `Segmentation fault`. Sigh.

---

Time to open a debugger. Doesn't help much. Enabled ASAN, figured out `locals` was overflowing. I guess `n_locals` doesn't include arguments!

It didn't help much, though -- now I'm underflowing `stack` when trying to load function arguments from stack. I wonder where that comes from.

I'm confused. The function it's trying to call is `$__wasi_init_tp`, which takes no arguments, but for some reason my code thinks there's 2 arguments.

```
wasm-dis --preserve-type-order hello-world/target/wasm32-wasip1/release/hello-world.wasm >dis
```

I parse the function type as `$5`, which is indeed defined as `(func (param i32 i32) (result i32))`... but as far as I can tell, that's not the function I should be entering by control flow. Are function signatures off by any chance?

> The function section has the id 3. It decodes into a vector of type indices that represent the type fields of the functions in the funcs component of a module.

Right, so the function section actually only touches on defined functions, not imported functions. The function section is parsed after the import section, so I can just use the value of `n_funcs` at that point as the offset.

---

Implemented more instructions and now it just hangs. It's doing *something*, but I don't know what. It's stuck in `$_RNvNtCsTnEDepTwQh_3std2rt19lang_start_internal`. There's a `loop` that does god knows what.

```wast
(loop $label
 (br_if $block1
  (i64.eq
   (local.get $7)
   (i64.const -1)
  )
 )
 (i64.store offset=1055976
  (i32.const 0)
  (select
   (local.tee $6
    (i64.add
     (local.get $7)
     (i64.const 1)
    )
   )
   (local.tee $8
    (i64.load offset=1055976
     (i32.const 0)
    )
   )
   (local.tee $9
    (i64.eq
     (local.get $8)
     (local.get $7)
    )
   )
  )
 )
 (local.set $7
  (local.get $8)
 )
 (br_if $label
  (i32.eqz
   (local.get $9)
  )
 )
)
```

I'll take a look at Rust sources. I see a call to `sys::init`, responsible for parsing `argv`, and initially I thought that's the issue (I don't place anything useful in memory), but I found this in `sys::pal::wasi`:

```rust
pub fn cvt<T: IsMinusOne>(t: T) -> io::Result<T> {
    if t.is_minus_one() { Err(io::Error::last_os_error()) } else { Ok(t) }
}

pub fn cvt_r<T, F>(mut f: F) -> io::Result<T>
where
    T: IsMinusOne,
    F: FnMut() -> T,
{
    loop {
        match cvt(f()) {
            Err(ref e) if e.is_interrupted() => {}
            other => return other,
        }
    }
}
```

There's a loop, a comparison with `-1`, and a `select` based on whether something is `-1`. It's probably this. It's trying to handle `-EINTR`, but for which syscall?.. It's just reading from `1055976`. Maybe it's this line?

```rust
unsafe { main_thread::set(thread::current_id()) };
```

...and it's actually a global that should be initialized to some constant or something? Hmm... does `current_id` access a TLS? Does it try to allocate or something? What?

Okay, I get it, no, the loop comes from `ThreadId::new`, which tries to generate a globally unique ID.

Oh, I see. I misimplemented `select`. That makes a lot of sense, now execution continues further.

---

Execution stopped at `call_indirect`. That's the call to `main`! We're pretty close.

---

Encountered `i32.shr_u`. I wonder how they handle shift values larger than type size... Right, they take it modulo `32`. Good.

---

Got a null pointer dereference. I'm assuming it's a function call to an import, since I don't handle them yet and just use `NULL`. `gdb` seems to agree.

Time to handle imports it is. Here are the imports:

```
(import "wasi_snapshot_preview1" "environ_get" (func $__imported_wasi_snapshot_preview1_environ_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "environ_sizes_get" (func $__imported_wasi_snapshot_preview1_environ_sizes_get (param i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "fd_write" (func $__imported_wasi_snapshot_preview1_fd_write (param i32 i32 i32 i32) (result i32)))
(import "wasi_snapshot_preview1" "proc_exit" (func $__imported_wasi_snapshot_preview1_proc_exit (param i32)))
```

The docs are here: https://github.com/WebAssembly/WASI/blob/wasi-0.1/preview1/docs.md

`environ_sizes_get` returns `Result<(usize, usize), errno>`, but in reality it returns a `i32`. It also isn't supposed to take parameters, but it receives two of them. I guess those are output pointers and the return value is the discriminant? But I can't find the documentation for this.

Found this: https://wasix.org/docs/api-reference/wasi/environ_sizes_get

So not quite -- there are two output pointers, one per `usize`, and the return value is the `errno`.

It's a bit unwieldy, so let's first try implementing the function that's actually being called, i.e. `fd_write`. It takes:

- `fd` (obviously)
- `iovs` -- pointer to an array of `__wasi_ciovec_t`s
- `iovs_len` -- the number of iovecs
- `nwritten` -- output pointer

...and returns `errno`. Not quite Linux-like, but okay.

Can I implement this with `writev`? Surely the ABI is compatible? Okay, no, probably not due to 64-bit vs 32-bit pointers. Unfortunate.

---

Got an error written by `fd_write`:

```
fatal runtime error: rwlock locked for writing, aborting
```

So where does that come from? The trace is

```
$__wasi_fd_write
$_RNvXs9_NtNtCsTnEDepTwQh_3std2io5implsINtNtCsc9qWavMCQxu_5alloc3vec3VechENtB7_5Write9write_allB9_
$_RNvXNvNtCsTnEDepTwQh_3std2io17default_write_fmtINtB2_7AdapterNtNtNtNtB6_3sys5stdio4unix6StderrENtNtCshbByS147RDD_4core3fmt5Write9write_strB6_
$_RNvNtCshbByS147RDD_4core3fmt5write
$_RNvYNtNtNtNtCsTnEDepTwQh_3std3sys5stdio4unix6StderrNtNtBa_2io5Write9write_fmtBa_
$_RNvNtCsTnEDepTwQh_3std9panicking15panic_with_hook
$_RNCNvNtCsTnEDepTwQh_3std9panicking13panic_handler0B5_
$_RNvNtCshbByS147RDD_4core9panicking9panic_fmt
$_RNvNvMNtNtCsTnEDepTwQh_3std6thread2idNtB4_8ThreadId3new9exhausted
$_RNvNtCsTnEDepTwQh_3std2rt19lang_start_internal
```

...so I'm somehow already reaching `exhausted` in the same `ThreadId::new` function. Is it an arithmetic bug this time? I'm confused because I could *swear* I reached `call_indirect`. Or was it from the panic machinery instead of `main`? I guess so. Fun.

Okay, so apparently I was misunderstanding how `loop` works. There's a `br_if` at the end of this `loop`, and apparently that's how the loop is `continue`d, but I think fallthrough should be equivalent to `break`, not `continue`:

> This semantics also applies to the instruction sequence contained in a loop instruction. Therefore, execution of a loop falls off the end, unless a backwards branch is performed explicitly.

---

More instructions. `i32.ctz` is a fun one. Who could possibly need it in a hello-world? Oh, right, an allocator.

---

Another sanitizer failure. This time it's a memory access -- `0x204064` is clearly out of bounds for our 2 MiB memory. There are many other odd accesses before it, generally steadily increasing.

All the accesses come from `$_RNvNtNtCshbByS147RDD_4core5slice6memchr7memrchr`. What could possibly be going on there? I'd expect `memrchr` to access memory in reverse order, but that's not what's happening here.

The addresses are increasing by 127. Why??

Most confusingly, the load instruction is `i32.load8_u`, not the SWARed path.

Wait, I have an idea. Here's one part of the loop:

```wast
(local.set $6
 (i32.add
  (local.get $6)
  (i32.const -1)
 )
)
```

It's trying to add `-1`, but it's supposedly adding 127. Which is exactly how -1 would be represented as a 7-bit signed number. Do I perhaps misunderstood how LEB128 is applied to `iN` types?

> Uninterpreted integers are encoded as signed integers.

I could swear it was unsigned integers. That makes more sense and makes everything so much more confusing. How did it even work until now??

---

Now I get an undocumented `0xfc` opcode. This one's present in the Wasm 3.0 spec, though. Turns out it's a prefix. Must be a `memory.*` opcode -- the features listed `bulk-memory`, that's what it probably is. Yup, it's `memory.copy`, i.e. just `memmove`.
