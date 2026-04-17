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

The next section is the memory section. There can be only one memory in Wasm 1.0, so the only useful thing to extract here is the minimum and maximum size of the linear memory. We don't need to track the high boundary. Technically we don't need to track the low boundary either, we can just hardcode like 1 MiB and it'll work fine I guess. So we don't need this section either.

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

---

Yay, sanitizer errors again. I missed you. This time it's a stack underflow while evaluating `i64.div_u`. But there's only one `i64.div_u` in the code, and it's not immediately after a `return`, so I must've broken something about function calls.

The function this happened in is never called directly. Maybe `call_indirect` is parsed wrong? `$_RNvNtCshbByS147RDD_4core3fmt5write` does use `call_indirect`. I genuinely feel like an LLM writing this.

I'm confused. There are genuinely `0x80` (`i64.div_u`) bytes after `call_indirect`:

```
11 81 80 80 80 00 80 80 80 80 00 0b 02 40 20 05 28 ...
```

This is `call_indirect` followed an overlong encoding of the number `1` for `typeidx`, followed by `0x80` as the table index, etc. This is clearly malformed. What's going on? Did Wasm 3.0 change something important? Doesn't seem so.

Is it due to relocations? I could imagine `call_indirect` being used to link different modules. And since modules might have different tables, you might want to change the tables and the memory... so it leaves fixed space for that? Wow.

So apparently we're simply not allowed to skip uints that are documented to be exactly `0x00` in the Wasm 1.0 spec, because the Wasm 3.0 spec makes them variable and thus possibly overlong. It's a little too late to make this change everywhere, but I can at least fix it in `call_indirect`.

---

Great, more instructions outside of Wasm 1.0. `i32.extend8_s` this time. A few instructions later we finally have a complete "Hello, world!". It's beautiful.

---

Okay, now for optimization. I'd love to simulate the Wasm stack with the native one, but to free the native stack, I need to make the core interpreter non-recursive. It currently mainly uses recursion to handle blocks.

Here's the plan: save the block/function frame info to a separate manual stack that is accessed much more rarely (and thus requires less code), and then make `0x0b` (`end`) a real instruction that jumps somewhere or updates globals depending on that stack, instead of just a terminator. Perhaps I should look at how labels are explained in the Wasm spec for inspiration.

Sidenote: my parse/eval mechanism based on a global `broken_blocks` flag reminds me very much of Forth.

Say `eval_until` tries to becomes a tail call. What do I usually do after it, which prevents this?

- In `block` and `if`, I do `break_level -= executed && break_level > 0` after it and then continue evaluating intrs as usual.
- In `loop`, I check if `executed && break_level > 0 && --break_level == 0` and reset `p` if so, then continue as usual.
- In function invocation, I reset `break_level` and some other properties and then continue as usual.

So what I'm thinking is that this small tail (a variant ID, together with the saved parameters) should just be stored on the manual stack and evaluated by hand as necessary.

...except: function calls will have to reverse arguments, since x86's stack grows downwards. Hopefully that's fine and doesn't end up being too long?

I just found another issue: I can't easily allocate locals anymore. I used to use the native stack for that with what is effectively just `alloca`. Now I use the native stack for the Wasm stack, and I want the Wasm stack to be shared between functions invocations without allocations in-between (due to return values), so I need to allocate locals elsewhere.

Segfaults... Segfaults as always. Now it's a stack underflow immediately after `$malloc`. For some reason `$malloc` didn't return enough data or something? I don't get it.

Okay, so the issue was that I didn't handle `loop` correctly. If there was a `br` to the `loop`, I removed it from the stack and didn't add it back when reentering. That fixed it. The code has become quite ugly, but it's not too bad and hopefully it'll let me implement `if`..`else` later.

---

Here's another idea: I currently have two invocations of `eval_loop`, one for the start (initialization) function and one for `_start`. I could instead invoke `_start`, then before I evaluate it invoke the start function, and call `eval_loop` once. This effectively inserts a fake `(call <start function>)` at the beginning of `_start`. The reason to do this is because it allows me to unconditionally call `exit` when the caller stack empties (i.e. `p` is reset to `NULL`).

---

I should probably start working on making `stack` grow downward. I'm a little sad about it -- I really loved the `memcpy` from stack to locals -- but `pop` and `push` are just too compact to lose on this opportunity.

That was easy.

---

Okay, now to actually start working on getting the size down. Turns out I forgot to make local functions `static`, so that increased the binary size a bit.

```
$ size interp-small
   text	   data	    bss	    dec	    hex	filename
   7545	    672	3227808	3236025	 3160b9	interp-small

$ nm -S --size-sort interp-small | grep -i ' t ' | cut -d' ' -f2-
0000000000000002 t main.cold
000000000000000d T _fini
000000000000001b T _init
0000000000000020 t frame_dummy
0000000000000026 T _start
0000000000000030 t deregister_tm_clones
0000000000000040 t register_tm_clones
0000000000000050 t __do_global_dtors_aux
0000000000000062 t impl_read_int
00000000000000df t fd_write
0000000000000137 t call_func
0000000000000ffe T main
```

There's a suspicious discrepancy -- the sizes reported by `nm` sum up to 5000, significantly below 7545. I'd assume that's due to alignment, but `-fno-align-functions` doesn't help. There must be unaccounted data in `.text`. Maybe it's PLT? We don't really need to link to anything, and I think this time is as good as any to get rid of the libc dependency.

---

Man, that's a lot of stuff to reimplement. It's not *that* much, it's really just `memcpy`, `memmove`, `syscall`, and `_start`. But `syscall` has to be a macro, and that gets ugly. Just take a look at `musl`'s `syscall`. I guess I can just implement `syscall` for specific sizes.

Implementing `_start` correctly requires reading the Linux ABI stack: https://articles.manugarg.com/aboutelfauxiliaryvectors (though this post assumes a 32-bit architecture).

---

Now `size interp-small` reports 5203 bytes, which is close enough, especially since I reimplemented a small part of libc. What I don't get is why the ELF itself is still so large. Section headers, I guess?

```
strip --strip-section-headers $@
```

Also added `-fno-asynchronous-unwind-tables`. The file is now 12 KiB down from 16 KiB.

I remember using some ELF explorer to see what takes space -- can't find it now, unfortunately.

What I can do is use `xxd`. There's a whole lot of zero bytes. I think I can just ask the linker to reduce section alignment. Added `-Wl,-n`, now it's 9.5 KiB -- much better. There's still some zeroes, though...

I don't get it. Here are the sections:

```
  [ 0]                   NULL             0000000000000000  00000000
       0000000000000000  0000000000000000           0     0     0
  [ 1] .note.gnu.bu[...] NOTE             0000000000400158  00000158
       0000000000000024  0000000000000000   A       0     0     4
  [ 2] .text             PROGBITS         0000000000400180  00000180
       000000000000118f  0000000000000000  AX       0     0     32
  [ 3] .rodata           PROGBITS         0000000000401310  00001310
       00000000000001b8  0000000000000000   A       0     0     16
  [ 4] .note.gnu.pr[...] NOTE             00000000004014c8  000014c8
       0000000000000030  0000000000000000   A       0     0     8
  [ 5] .data             PROGBITS         00000000004024f8  000024f8
       0000000000000020  0000000000000000  WA       0     0     8
  [ 6] .bss              NOBITS           0000000000402520  00002518
       0000000000314080  0000000000000000  WA       0     0     32
  [ 7] .comment          PROGBITS         0000000000000000  00002518
       000000000000001b  0000000000000001  MS       0     0     1
  [ 8] .symtab           SYMTAB           0000000000000000  00002538
       00000000000002d0  0000000000000018           9     8     8
  [ 9] .strtab           STRTAB           0000000000000000  00002808
       0000000000000102  0000000000000000           0     0     1
  [10] .shstrtab         STRTAB           0000000000000000  0000290a
       0000000000000063  0000000000000000           0     0     1
```

There's a lot of empty space between `.note.gnu.pr[...]` and `.data`, but the note section itself is small.

Maybe this is the explorer? https://github.com/rbakbashev/elfcat Yup, that's it. But that doesn't explain why there are zeros. It's not even aligned... it's just exactly 4096 zero bytes. Where could they possibly come from?

Okay, let's try something different. Let's just write a linker script. I'm sure I have a template lying around.

Found one in sunwalker-box. Tried to write it myself by hand for a minute but got SIGSEGV when trying to run the ELF. I always forget the `. = 0x400000` line -- the kernel treats that as an absolute address and refuses to map anything at address 0, and without this line the default is `. = 0`.

5200 bytes exactly. `elfcat` is much more pretty now -- it's just a single ELF segment. And I can still use the normal `interp` build for debugging and `nm`/`size`.

---

Before optimizing stuff by hand, I wanted to see if I could improve code size by switching targets. I don't think it makes sense to switch to i686 -- we do need a lot of 64-bit arithmetic, so we'd likely lose on space. I wanted to try the x32 ABI, but it looks like it's getting less and less support day by day... unfortunate.

I tried aarch64 just in case, but it was slightly larger, at 5384 bytes. (See `arm` branch.) I think ARM is just a bad choice for code golf.

---

Looked at the disassembly again and found more `nop`s. Realized I didn't enable `-fno-align-loops` and similar options. 4976 bytes now. Some `nop`s are still present; not sure what the deal is there.

---

There's an issue with `memcpy`/`memmove`. Since they use inline assembly, GCC cannot infer that `memcpy(dst, src, 4)` is equivalent to a manual 4-byte copy. But if I use `__builtin_memcpy` everywhere, it generates `call memcpy` in codegen, breaking inlining, which defeats the purpose of using `rep movsb`. So I have to use `__builtin_*` for fixed amounts and the normal version for variable amounts manually. 4928 bytes.

---

Simplified `impl_read_int` a little, but it didn't improve file size.

I'm thinking about trying out something a bit more cursed. We use the `p` pointer all across the program, and it'd be awful useful for it to be in a register. IIRC, GCC has some option to do that.

Here: https://gcc.gnu.org/onlinedocs/gcc/Global-Register-Variables.html

4720 bytes after adding

```c
register unsigned char *p asm ("r12");
```

---

I think I can't optimize this further with C alone. But before rewriting at least some parts of the code in assembly, I need to make sure it's feature-complete enough. Here's the missing opcodes from Wasm 3.0:

- Floating-point memory accesses: `0x2a`, `0x2b`, `0x38`, `0x39`, `0x43`, `0x44`.
- Size conversions: `0x2c`, `0x2e`, `0x30` to `0x35`, `0x3c` to `0x3e`, `0xa7` to `0xb1`, `0xc1` to `0xc4`.
- Memory: `0x3f`, `0x40`.
- Various comparisons: `0x4c`, `0x4e`, `0x50`, `0x53` to `0x66`.
- Integer arithmetic: `0x69`, `0x6d` to `0x70`, `0x75`, `0x78` to `0x7b`, `0x7d` to `0x7f`, `0x82`, `0x85`, `0x87` to `0x8a`.
- Floating-point arithmetic: `0x8b` to `0x98`, `0x9a` to `0xa6`.
- Floating-point conversions: `0xb2` to `0xbf`, `0xfc 0x00` to `0xfc 0x07`.

Most of them are present in Wasm 1.0 as well, so we can't really cheat that way. We can just refuse to support the various WasmGC features, vector extensions, and dynamic allocations, though.

---

So I woke up and, unsurprisingly, forgot what I was doing. I think my idea was to finish implementing some set of instructions to get it to orthogonality, and then use bit flags or similar ways to remove the exponential blow-up. Hopefully this can let me increase feature support without increasing size much.

Implemented all integer comparisons, code size is now 4800. But now that they work, I can think of a couple optimizations:

- I can sign-extend `int` to `long` when performing signed `i32` comparison, so that I don't have to worry about data size anymore. This brings the size down to 4784.
- Now that all comparisons are 64-bit, the result if a comparison can be determined from the flags set by a single `cmp` instruction. I'm confused on how to parse the flags without tasting a ton of space, though.

Now that I think about it, what I really need is a `setcc` with a dynamic parameter. I don't think x86 supports dynamic comparison like that. Hmm. I could just write self-modifying code, it's not like I need W^X, and I'm writing in x86-64 so I don't need to reset code cache. It's going to be slow, but who cares?

https://www.felixcloutier.com/x86/setcc

So the instruction looks like `0f <...> r/m`, and we need to patch the second byte.

Man, I can never remember the AT&T syntax. Right, `[rel a]` is written as `a(%rip)`. https://stackoverflow.com/questions/54745872/how-do-rip-relative-variable-references-like-rip-a-in-x86-64-gas-intel-sy Now I just need to make code pages rwx. They already are in `interp-small`, but not in `interp-debug`, which I'd like to use. Hmm.

Eugh. This is not as trivial as I expected it to be. `ld -N` disables dynamic linking, but I need it for ASAN. `objcopy --set-section-flags` can make the `.text` section writeable post-build, but it doesn't mark the corresponding segment as writeable. Can I maybe emit an assembler directive?

Tried

```
.section .text, \"awx\", @progbits;
```

but got "Warning: ignoring changed section attributes for .text" followed by assembler errors. I can't even use a linker script for this... I mean, I can, but it's likely not going to look simple. And I need to support both `-static` and shared builds. Eugh!

Okay, so I just patched the bit by hand with a small C script. `<elf.h>` is cool. Now I just need to deal with the `ud2`... right, I swapped the parameters to `cmp` and also made a typo.

4624 bytes after replacing the ternary with an array literal. That's smaller than original! Hopefully this trend continues.

This leaves:

- Various comparisons: `0x5b` to `0x66`.

---

I considered patching the `cmp` between 32-bit and 64-bit variants as well, instead of sign-extending values, but I think that'll have to wait. It's not impossible to do, but the savings will likely be minimal.

I should just switch to other instrs, like integer arithmetic.

Found a bug in `i32.rotl`/`i32.rotr`, I forgot a conversion.

Now I get an undefined reference to `__popcountdi2`. What's the status of the `popcnt` extension, anyway? Surely everyone supports it by now?

It's part of SSE 4.2. I think I can enable it. 4976 bytes. Not much to optimize yet, I think I'll skip over that for now. That's integer arithmetic done.

---

I think I need a framework for short jump tables. I currently use ternaries to choose between values, and that generates code with `<do thing>; jmp out`, where "do thing" takes less space than `jmp out`. The least I can do is turn `jmp` into `ret`.

That's not exactly trivial, though, due to calling conventions. I can't set up a custom one, so I probably need to just generate asm directly.

Got down to 4968 bytes. That's not a big improvement, of course, but since I control the assembly now, I can replace a jump table with a linear scan, using `ret` as seaprators -- the pointers in the jump table take more space than the code. And that brings it down to 4816 bytes -- much better!

---

I can't help but notice that for the most part, the only difference between 32-bit and 64-bit handlers is a REX prefix. I think this time it's more useful -- `cmp` was just one instructions, but here the instructions are *everywhere*. And since the exact registers are fixed (`a` needs to be in `rax` for `div` and `b` needs to be in `rcx` for `shl`), I don't have to worry about the REX prefix being trickier to specify. This brings it down to 4720 bytes.

---

Floating-point operations excluded, this leaves just size conversions. It also gives me a chance to optimize out the `memcpy` call in the variable-length loads and fix an unportable cast to `(char)` instead of `(signed char)`.

I'll start with loads and stores. Loads are messy more than tricky, since opcodes don't straightforwardly correspond to sizes. Its kind of hard to hand all the cases the same way: `i32.load` and `i64.load` don't need any zero-extension, while `i64.load32_u` seemingly does, but there's no `movzx r64, r/m32` because it's just a `mov`.

Here's the list of parameters we need to handle:

- Read size: 8, 16, 32, 64.
- For small reads, sign or zero extension.
- Sign-extension into `i32` vs into `i64`.

Parametric `shl` + `shr`/`sar` + conditional `mov r32, r32` might do the trick, I guess? That's 4784 bytes.

---

I do wonder, though. Currently, my ABI is that 32-bit integers are always represented in 64-bit storage with the top bits zeroed. Allowing garbage in the top bits would simplify loading. Do I actually rely on this anywhere?

Oh, of course I do -- in places like `br_if`, which receive `i32`. Nevermind then.

But I can still do something cool by placing in the top 2 bits of the shift, which `shl`/`shr` ignore. I'm thinking about placing the info about the bitness there. If I can carefully sign-extend it, maybe I can use it as a bitmask of sorts to simplify this condition:

```c
if (opcode < 0x30 && opcode != 0x29) { // 32-bit destination
    value &= -1U;
}
```

Didn't work out, unfortunately. Maybe GCC is just bad. I tried using Clang, but it said

> error: register 'r12' unsuitable for global register variables on this target

...and it seems like Clang simply doesn't support this feature: https://clang.llvm.org/docs/UsersManual.html#gcc-extensions-not-implemented-yet. I tried removing the `register` annotation, but Clang still produced a significantly larger binary than GCC, even with `-falign-loops=1`.

I'd also like to try `-Os`, but it wants me to provide `memcmp`, even with `-ffreestanding` and `__builtin_memcmp`. I use `memcmp` to compare with `_start` and `fd_write`, but I can just emit an 8-byte write and compare constants, I guess. Somehow that's an improvement even without `-Os` (4752 bytes). And with `-Os`, it's 3952 bytes! Wow. `-Oz` is slightly better (3896 bytes), but I'll keep using `-Os` for now to keep the disassembly readable.

---

Back on track, I should really optimize stores -- they're quite bulky. The `rep movsb` is pretty much optimal, I think -- it's about short as it gets. The tricky part is optimizing `len` calculation. I need to map it like this:

```
0x36 => 4
0x37 => 8
0x38 => 4
0x39 => 8
0x3a => 1
0x3b => 2
0x3c => 1
0x3d => 2
0x3e => 4
```

I tried to use a table, but it's worse than just conditional jumps. So I did a different cool thing. If I put the bytes in a single constant, like `0x040201020108040804`, I can use the constant itself as a LUT, by shifting it to the right by `index * 8` and then taking the low byte with `movzx r32, r8`. There are 9 entries, which doesn't fit in a 64-bit constant, but I can use a rotate instead of a shift because the first and last elements match. And I can further pre-rotate the constant to avoid having to subtract `0x36` from `opcode`. That gives 3912 bytes. Applying the same trick to loads, I got 3904 bytes.

This leaves `0xa7` to `0xb1`, `0xc1` to `0xc4`. I also implemented `f32.store` and `f64.store`.

---

I just realized I could optimize stores a bit further. Since stores are actually copies from `stack` to `memory`, it can be a direct `memcpy`, without going through a stack local. 3888 bytes.

---

Back to arithmetic. `0xa7`... are truncation/extension instructions that don't interact with memory. Some of them are floating-point-related, I'll skip those for now.

This is funny: `i64.extend_i32_s` and `i64.extend32_s` do the same thing and have very similar names, but have different signatures (`i32 -> i64` vs `i64 -> i64`).

This looks similar to loads, in fact I think I can reuse some code. But the opcodes are not consecutive. But we got *ridiculously* lucky. Look at this:

```c
case 0xa7: // i32.wrap_i64
case 0xac: // i64.extend_i32_s
case 0xad: // i64.extend_i32_u
case 0xc0: // i32.extend8_s
case 0xc1: // i32.extend16_s
case 0xc2: // i64.extend8_s
case 0xc3: // i64.extend16_s
case 0xc4: // i64.extend32_s
```

Not only are the last 4 nibbles different among all opcodes -- the last *3* bits almost don't collide, and when they do collide (`0xac` and `0xc4`) it's between `i64.extend_i32_s` and `i64.extend32_s`, which have the same semantics!

```
i32.extend8_s
i32.extend16_s
i64.extend8_s
i64.extend16_s
i64.extend32_s i64.extend_i32_s
i64.extend_i32_u
nop
i32.wrap_i64
```

So we can use an 8-byte LUT, and that means an inline 64-bit constant! And it gets better: signed operations are not only consecutive, but form a prefix, so we can use a single comparison for that. And same for `i32` operations! I refuse to believe this is an accident.

```c
unsigned long shift_const = 0x2000202030383038UL;
asm ("shr %b1, %0" : "+r"(shift_const) : "c"(opcode * 8) : "flags");
unsigned char shift = shift_const & 0xff;

value <<= shift;
if (opcode % 8 < 5) { // signed
    value = (long)value >> shift;
    if (opcode % 8 < 2) { // 32-bit destination
        value &= -1U;
    }
} else {
    value >>= shift;
}
```

For whatever reason, though, this is less optimal than a ternary... Maybe because there are just 6 different operations? The code looks quite optimal, even though there are many jumps. I guess it'll stay a ternary then. Unfortunate!

Left unimplemented: `0xa8` to `0xab`, `0xae` to `0xb1`.

---

This leaves memory and floating-point. Let's start with memory, that sounds simpler. We need to implement `memory.size` and `memory.grow`. I guess we can just simulate a fixed-sized array? Just need to remember not to allow the machine to go out out-of-bounds due to the fixed-size access in `*.load*` by adding a few guard bytes at the end of `memory`. 4016 bytes, very simple.

---

Floating-point is really scary. That's a large instruction space... we barely fit in 4 KiB (that's amazing btw), but floating-point will likely make this feat impossible. We can try to touch on some basics, though.

- Memory accesses: `0x2a`, `0x2b`, `0x43`, `0x44`.
- Comparisons: `0x5b` to `0x66`.
- Arithmetic: `0x8b` to `0x98`, `0x9a` to `0xa6`.
- Conversions: `0xa8` to `0xab`, `0xae` to `0xbf`, `0xfc 0x00` to `0xfc 0x07`.

Let's start with loads. I played around with it and realized that loads were apparently broken?.. The constant is wrong, but hello-world didn't catch that. Huh. The conditionals were also off, though it didn't matter. Basically, with our current semantics, `i32.load` and `f32.load` are interpreted as the non-existent `i32.load64_s`, so we sign-extend the 64-bit value to a 64-bit value and then truncate the top 32 bits. This is cursed, but it's optimal. `i64.load` and `f64.load` are treated as `i64.load64_u`, which is fine.

This didn't increase the code because I only needed to change the constant.

---

Implemented `f32.const` and `f64.const`. 4008 bytes. I guess that makes some sense?

---

Comparisons look like the easiest thing to start with, since it should just be a `cmpss`, and I already have similar code with `cmp`. Though now that I think about it, maybe I can optimize integer comparisons with masks, too. Let's see... Nope, tried it and it didn't help. Let's get to work on `cmpss` then.

Anyway.

```
cmpss xmm1, xmm2/m32, imm8
```

So we need to patch an `imm8`, as usual. Wait, there's no `cmpgtss`?

> The greater-than relations that the processor does not implement require more than one instruction to emulate in software and therefore should not be implemented as pseudo-ops. (For these, the programmer should reverse the operands of the corresponding less than relations and use move instructions to ensure that the mask is moved to the correct destination register and that the source operand is left intact.)

Oh, but the AVX version does support it with `vcmpgtss`. That's fun...

I'm confused though. SSE has `nle`, which supposedly differs from `gt` in NaN handling. But then does that mean `neq` is also different from `ne` in some fashion?.. Probably not, but I'll check how LLVM lowers this on godbolt. Nevermind, it does a `ucomiss` for comparisons, but it does use `cmp[n]eqss` for `==` and `!=`. And `ucomiss` stays even with `-mavx`. Huh.

Oh, that's because `cmpss` doesn't have a flag output. Then why not use `ucomiss` even for normal comparison?

- On `a > b`, `ucomiss` sets `Z = 0, C = 0`, compatible with an unsigned integer comparison.
- On `a < b`, `ucomiss` sets `Z = 0, C = 1`, compatible with an unsigned integer comparison.
- On `a = b`, `ucomiss` sets `Z = 1, C = 0`, compatible with an unsigned integer comparison.
- Oh, if `a` and `b` are unordered, it sets `Z = 1, C = 1`, which is typically impossible.

Sigh. So, which `setcc` specifically is this incompatible with? `sete` and `setne` are obviously broken. `setb` is broken, `setae` is fine. `seta` is fine, `setbe` is broken. Yea... I don't think this'll work out. Back to `cmpss` then.

Now I just need to switch between `cmpss` and `cmpsd` in runtime. That's another byte to patch.

4200 bytes after optimization. That's... surprisingly much. As far as I can tell, only 96 bytes were added to the relevant code section, but `size` does report 192 bytes added to `.text`. Is it because jumps got longer due to distances increasing, perhaps?

---

Or maybe jump tables are stored as part of `.text`? I see that the jump table is there despite `-Os`, and `-Oz` doesn't change that either. How do I check its size?

The relevant jump table starts at `binop_handlers+0x2f1`, which is `0x40202c`, which is part of `.rodata`. Why does `size` report the data section as having only 32 bytes? Does it count `.rodata` as text by any chance? `man size`

> The Berkeley style output counts read only data in the "text" column

Right. Who could've guessed. Anyway, `.rodata` has size `482`, and as far as I can tell, it's almost entirely a jump tables. Let's try something simple first -- remove jump tables entirely. One `-fno-jump-tables` later we get 4040 bytes. Let me commit that before I try other things.

---

So my idea is that maybe jump tables are large, but `cmp` + `je` is also large because not all jump offsets fit in 8 bits. And even if we account for intervals, almost every generic handler has to contain code for parsing per-opcode information from a LUT. So maybe instead of removing jump tables, we can just significantly simplify them: leave 1 byte for the handler index and 1 byte for arbitrary per-opcode data. Maybe that might work better? It should also increase code locality, so maybe that'll play a role.

But let's start with something simpler: just move every handler out to a separate function. Somehow that reduced code by 8 bytes, funny.

Next I made a 1-byte jump table. That's 4056 bytes, but now I can try to add 1-byte parameters to instructions, hopefully simplifying plenty code.

So I extracted almost everything I could to single-byte args, and now I'm at 4216 bytes. So I saved 96 bytes on code, but wasted 256 bytes on a table. I now doubt that was a good idea. Apparently tables are ridiculously expensive. That's unfortunate. Rolling this back, the code is on the `optable` branch.

---

While working with the optable, I found some other avenues for optimization due to similarity between some control opcodes. This brings the size down from 4040 bytes to 3864 bytes.

---

Thought to make `stack_head` a register while I'm at it. I still think it should be `rsp`, but I'm pretty sure I'll have to work on the C version for quite a while, so let's make it more tenable while it lasts. 3632 bytes.

---

Time for floating-point arithmetic -- hopefully there's enough space for that due to recent changes.

I can probably start with unary operations. I already implement `f64.abs` because it can be done on integers, but not others, like:

- `f64.neg`
- `f64.ceil`
- `f64.floor`
- `f64.trunc`
- `f64.nearest`
- `f64.sqrt`

Rounding can be done with `roundsd`.

> The immediate operand specifies control fields for the rounding operation, three bit fields are defined and shown in Figure 1-24. Bit 3 of the immediate byte controls processor behavior for a precision exception, bit 2 selects the source of rounding mode control. Bits 1:0 specify a non-sticky rounding-mode value (Table 1-18 lists the encoded values for rounding-mode field).

...where:

- Round to even is 00 (`f64.nearest`)
- Round downward is 01 (`f64.floor`)
- Round upward is 10 (`f64.ceil`)
- Round toward zero is 11 (`f64.trunc`)

We have a choice:

- Either bit 2 is `0`, denoting a rounding mode taken from the instruction, and we patch the instruction as usual.
- ...or it's `1` and the rounding mode is taken from the FP context.

If we use the second way, we need to set MXCSR to `0x1f80 | (rounding_mode << 13)`. That's a write to stack and `ldmxcsr`. If we use the first way, we need to patch only one byte and don't need any operation afterwards. I think patching code is better, especially since we'd have to return the values back before normal floating-point arithmetic.

On that note, though... how is `sNaN` handled? Wasm says there's no difference between `sNaN` and `qNaN`. Does x86 behave the same by default? I think it does -- the default value of MXCSR, `0x1f80`, has all the mask bits set. So that shouldn't be an issue. Cool.

Implemented rounding for both `f32` and `f64`, 3712 bytes. This leaves:

- `f64.abs`, `f64.neg`, `f64.sqrt`
- `f32.abs`, `f32.neg`, `f32.sqrt`

---

`f64.sqrt` is just `sqrtsd`. I don't know how to implement this well -- it's very similar to `roundsd` in spirit. When implemented as a separate instruction set, it bumps size to 3816 bytes. Most of that increase seems to be due to routing execution to the right place, rather than the implementation itself.

It is kind of close to rounding, though... I can try merging them. That results in 3760 bytes. I guess that'll have to do, though I really don't like having to do a ton of branching.

---

Does SSE simply not support negation and absolutes? Clang lowers both to bit operations. I guess this simplifies things a little -- the choice between implementing these ops on integers vs floats is already done for me. 3816 bytes.

While we're at it, I might as well merge the two groups of unary operators together -- misc integer arithmetic (like `i32.popcnt`) and size extension (like `i32.extend16_s`). That's 3776 bytes.

---

The unary op handling has grown in size significantly. I'd love to use the same approach as with binary operators (`call`ing thunks, counting `ret`s instead of using a jump table), but the opcodes here are very far from each other, so that doesn't work. But I can at least move the ternary out to a separate function so that it can use `ret`.

That worsened it. Due to the calling convention, I'm assuming? This isn't Clang, though, so there's no `preserve_none`. Inline `asm` it is then. And I think I can kinda use a flat key-value map for opcode implementations.

It's a little cursed, but it's now 3664 bytes.

---

I'm getting the feeling I might just be able to fit floating-point arithmetic in here.

I'll focus on simple binary operators for now. I have a feeling `min`/`max` might be complex due to `NaN` handling. I was wondering where `rem` was, but I guess it has a valid userland implementation based on floored dividion and FMA... but I don't think either is present in Wasm, at least in Wasm 1.0. Anyway.

Implemented `add`, `sub`, `mul`, `div`, got 3784 bytes.

---

I don't think SSE has `copysign`. And it's easy to implement as a binary operation, so that's the likely reason. Clang lowers `copysign` in a somewhat confusing manner; I don't get why it doesn't just use `blend`.

Oh, right, `blend` is not bit-granular. https://stackoverflow.com/questions/57870896/writing-a-portable-sse-avx-version-of-stdcopysign#comment102167961_57870896 Not that it'd work with integers anyway.

I initially wanted to use the integer binop infrastructure for `copysign`, but it doesn't quite work, since `copysign` requires multiple instructions and can't be patched for different sizes easily. Its opcode is also far from other instructions. So it needs to be a separate instr handler.

Yuki designed a clever implementation that doesn't need patching for different sizes and just receives a flag in `cl` (1 vs 33):

```
shl rax, cl
shl rdx, cl
rcr rax, cl
```

3888 bytes.

---

Thought of another optimization while walking. Instead of patching out `rex.w`, we can just jump past it, so the binop code doesn't need to be self-modifying. 3880 bytes.

---

Applied a similar trick to unops, 3856 bytes.

---

Replaced a manual search loop with `repne scasb`, 3848 bytes.

---

Found a bug in `impl_read_int` -- varints containing more than 64 bits were parsed wrong, even if all bits above the 64th one were zero. Luckily the fix didn't increase code usage, since I optimized `impl_read_int` a bit prior to the fix.

---

Optimized `impl_read_int` a bit more -- I forgot that C supports multiple return values. 3824 bytes.

---

So I was playing around with making `eval_instr` non-inlineable, so that opcodes could use a short `ret` to signify an exit instead of using a long `jmp`, and realized there was a prologue with `push rbp`, despite frame pointers being disabled. I was confused for a while before realizing that `rsp` is saved because it's then manually aligned to 16 bytes. I initially assumed that's due to `call` instructions and tries to set `-mpreferred-stack-boundary=3`, but it didn't help, and in fact no other functions had alignment, despite some containing nested `call`s. It turns out that it was due to `__m128i` locals -- even though they were never spilled, apparently the minimal stack alignment was still set to 16 just because they're present (even using `register` didn't help). So I had to replace them with `double`s and some such. It didn't lead to any size improvement yet, but it'll help later on.

---

The second reason `eval_instr` still has `push rbp` is the VLA in `br_table`. I've been looking at it for a while now, and I guess it's finally time to simplify that code. Reimplemented it without stack allocations, now it takes 3784 bytes.

Stack alignment is still present... this time due to an indirect call to native code in `call_func`. `-mpreferred-stack-boundary=3` didn't help, I give up. `asm volatile` it is. 3776 bytes.

---

I can't make GCC emit conditional returns as jumps over `ret`. It wants to emit a conditional `jmp` to a single far-away `ret` instead. What do you even do about it? And it's not like `asm volatile ("ret")` works -- first, it SIGSEGVs under ASAN for obvious reasons, and second, it generates worse code when there *is* a `ret` nearby. Sounds like something better left to an assembly version, I guess.

For now, the best I can do is add `__attribute__((noinline))` and call it a day. 3696 bytes.

---

Apparently my `if` inverted the condition and still worked?? Wow.

---

Optimized `struct caller_info` population. 3680 bytes.

---

I'm realizing there's a dichotomy. If I use `rsp` for call stack, I can't use it for data, and accesses are long. If I use `rsp` for data, I can't use `ret`, and handler exit paths are long. `leave; ret` can at least safely return, but loses `rsp`. Hmm, `jmp rbp` is just two bytes though. I wish there was a 1-byte instruction. There's plenty of ways to trigger various exceptions, but setting up their handlers will likely take more code.

This will have to wait for assembly anyway. For now, I can reduce the cost of `PARSED` a little by making `break_level` a register as well. It's the third such variable -- and hopefully the last one. With a few other minor optimizations I get 3552 bytes.

---

I think I should stop chasing optimizations for now and see how many features I can add. I've been sleeping terribly the last few days, so a lighter workload might be better.

I haven't implemented `fnn.min/max` yet. What are their semantics? If either value is a `NaN`, they return a `NaN`, otherwise the behavior is trivial. Are `minsd`/`maxsd` similar?

> If only one value is a NaN (SNaN or QNaN) for this instruction, the second source operand, either a NaN or a valid floating-point value, is written to the result. If instead of this behavior, it is required that the NaN source operand (from either the first or second source) be returned, the action of MINSD can be emulated using a sequence of instructions, such as, a comparison followed by AND, ANDN, and OR.

Nope, that's wrong. Unfortunate. How does Rust handle this? I thought it had several different `min`-like functions. `f32::minimum` seems to have the right behavior, and it handles `-0 < +0` too. https://doc.rust-lang.org/stable/std/primitive.f32.html#method.minimum How does LLVM lower it? Oh it's so nasty. That's garbage. Slightly better with `-C target-feature=+avx`, though seemingly just due to ABI differences. The lowering is still confusing though. It compares `a < b`, `a > b`, and if neither is true, it *also* compares both `a unord b` and `a != b`. I think it can be made simpler and compilers are just stupid about float comparison.

e.g. here's a valid implementation of `f32.min`:

```
	ucomiss a, b
	jb .a_lt_b
	je .a_eq_b
	jnp .a_gt_b
	return a + b
.a_lt_b:
	return a
.a_eq_b:
	return max(bits(a), bits(b)) -- -0 considered less than +0
.a_gt_b:
	return b
```

Unfortunately I don't see many possibilities for code reuse. Both `ucomiss` and `a + b` are sized and will need to be patched between `ss` and `sd`, and there's going to be duplication between `min` and `max`. I can have two return values, both for `min` and `max`, I guess... or I could patch some comparisons, but that's likely more expensive.

Wait, I'm stupid. `minss` behaves correctly if the inputs are neither unordered nor equal, so I don't need two paths for `<` and `>`.

Man, patching all these `ss`s is gonna suck. Brainstorming: I tried using `min(signed(bits(a)), signed(bits(b)))` instead of `max(bits(a), bits(b))` because it seemed closer in meaning, but only `max` is size-independent.

For now, let's just see if it's better to patch `min`/`max` or use two outputs. Two outputs immediately increase the code size by ~30 bytes due to duplication and a late `cmov`, and they'll also require more `ss`/`sd` patching. How difficult is it to patch `min`/`max`?

Looks like it's more optimal than `cmov`, if slightly. The codegen sucks a little, but that's due to GCC rather than intrinsic inefficiency. Time to think about `ss`/`sd`.

I don't think I need `addss` to propagate `NaN`s. Can't I just use `or`? `NaN` is represented as the maximum exponent and a non-zero mantissa. ORing this with anything can only change the sign or mantissa, but can't make it non-`NaN`. The Wasm spec says that:

> When the result of a floating-point operator other than fneg, fabs, or fcopysign is a NaN, then its sign is non-deterministic and the payload is computed as follows:
>
> - If the payload of all NaN inputs to the operator is canonical (including the case that there are no NaN inputs), then the payload of the output is canonical as well.
> - Otherwise the payload is picked non-determinsitically among all arithmetic NaNs; that is, its most significant bit is 1 and all others are unspecified.

Wait, no, that's broken then. A canonical NaN is a qNaN (so the highest bit of mantissa set, which is also preserved) with other mantissa bits reset. But ORing with a non-`NaN` can set them.

Technically, I can instead reset the highest bit of both inputs and do an unsigned `max`. That forces the sign to positive and chooses the value with the largest exponent and then mantissa. Since `NaN` has the largest exponent, it always wins, except against infinities, but infinities have a mantissa of zero, so it's not a problem either.

I thought I can get away with unconditionally resetting both bits 63 and 31, but that's actually broken -- for a `double` sNaN with a mantissa of `2^31`, this will turn it into `Infinity`. I guess I can still pass a constant of `31` or `63` in `cl`, through, or something... but it's likely to take much longer than the four bytes of `addss`, even taking `ss`/`sd` patching into account.

Omfg `pmaxuq` doesn't exist until AVX-512. I don't like this. Surely there's another way to choose between `-0` and `+0`, though? I think this time `or` works for `min` (preferring the sign bit set), and `and` works for `max` (preferring the sign bit reset). And hey, that's one less place to patch for `ss`/`sd`! (Sidenote: "one fewer" sounds wrong despite "place" being a countable noun. Apparently there's a special rule for "one"? I love intuition.)

I *think* I implemented it? 3664 bytes. That's over 100 bytes just for this operation!

---

I think at least some of the patching is more expensive than plain code duplication would be. I don't think I've *ever* used patching where duplication would also work yet, except perhaps in `fnn.sqrt`, but there it worked because it coincided with the various unary rounding ops, so I could reuse code.

But then again, `cmov`s are expensive because they're not really a thing for vector registers... and GCC refuses to generate conditional jumps.

I could replace `minss` back with a jump and two `mov`s. That'd remove one place that needs `ss`/`sd` patching, but then I'd need to patch `min`/`max` in two places. Or I could patch between `jb` and `jnb`, that also works. That removes 6 bytes from patching and adds 2 bytes from `jb` -- that's a win in my book. The file size didn't change because it always seems to be a multiple of 8.

---

I think it should still be possible to improve further. For example, I think I don't actually need to patch between `jb` and `jnb` -- I just need to invert the carry flag if the operation is `max`, so really I just need a primitive that allows conditionally flipping CF.

I know `adc 0, 0xff` copies CF, but `adc` can never invert it because it's monotonous over C: if addition overflows, adding larger numbers will also overflow. But I don't actually need the output in CF specifically -- something like ZF will also work. And that's the primitive: `adc 0, 0` sets `ZF = !CF`, while `adc -1, 0` sets `ZF = CF`.

I thought I could combine this `0`/`-1` constant with the `0xdb`/`0xeb` opcode in `pand`/`por`, but that doesn't quite work. With `adc`, I need addition to typically yield `0` or `-1`, and use CF to nudge it to `1` or `0` respectively. but the difference between `0xdb` and `0xeb` is not exactly `1`, so that doesn't work. Maybe there's another instruction that inputs carry? `rcl`/`rcr` come to mind, but it won't ever emit a zero output.

Hmm... but ZF is not the only cool flag -- there's also PF! And parity is really similar to the XOR we're looking for. Let's see: `0xdb` and `0xeb` have equal parities, but `0xdb + 0x5` and `0xeb + 0x5` have distinct ones. And adding `1` flips the parity. Intel in shambles, first use for PF found in ages.

3648 bytes. I feel oddly proud.

---

Here's my thoughts on `NaN` handling. We don't actually need `addss` or anything like that, because we aren't required to propagate `NaN` payloads. We can just unconditionally emit a canonical `NaN`.

We need to choose between `0x7fc00000 = (0x1ff << 22)` and `0x7ff8000000000000 = (0xfff << 51)` based on size. It's an interval of bits -- I thought there was some instruction with packed operands for that? Well... `bextr` is that, but for reading. Then scratch that, it sounds like too much code.

Though now I wonder: why do `ucomiss` and `addss` use different formats for encoding precision? `ucomiss` uses a precision override prefix, while `addss` uses `0xf2` vs `0xf3` as a required prefix. I see that `addps`/`addpd` use `0x66` though... I think I can just use the packed version then to reuse the same byte for both. Also fixed a typo. And I'm now using `0x67` as the 32-bit fallback instead of `0x90`, since it's easier to generate and should be an ignored prefix for these instructions.

3640 bytes now.

---

I think that's comparisons done! Next up are conversions.

Let's start with `inn.trunc_fnn_*`. It takes `fnn` as an input and outputs `inn`, where:

- If the input is finite and `trunc(input)` is a valid integer, that's the output.
- Otherwise, the output is undefined.

The spec says this on partial operators (i.e. those that can have an undefined output):

> Where the underlying operators are partial, the corresponding instruction will trap when the result is not defined.

I'm not sure if we want to handle traps -- it's not like we do any validation elsewhere. Though maybe it makes sense to draw a distinction between *runtime* properties and *static* properties. For instance, we can assume that the Wasm file passes validation, but still handle runtime OOB accesses. But let's forget about that for a second and focus on conversions.

Without AVX-512, x86 has the following truncating conversions:

- `cvttsd2si` -- `double` to `int`/`long` in GPRs
- `cvttss2si` -- `float` to `int`/`long` in GPRs

Notably, this only handles signed conversions. AVX-512 also has `vcvttsd2usi` and `vcvttss2usi` for unsigned conversions, but I'd much prefer not to rely on it. Compilers generate rather ugly code for conversion to `unsigned long` -- they convert both `x` and `x - 2^63` to `long` and then merge the two results depending on the value of `x` (either via comparisons or by taking the high bit of `cvt(x)`). When converting to `unsigned`, they just convert to `long` instead and use wrap-around as `poison`.

Let's forget about trapping then, and just rely on compiler-generated code for now. 3856 bytes, a more than 200-byte increase.

---

So, first of all, `f32` -> `f64` conversion is precise, so we can convert to `double`s first and then run the same code regardless of input type.

The opcode space here is frankly quite ridiculous because floating-point conversions are intermingled with pure-integer conversions, but oh well.

3784 bytes.

---

Here comes the fun part... The least we can do is replace the GCC `unsigned long` lowering with the LLVM lowering. GCC uses `comisd` to check if the value fits in 63 bits, LLVM reads the output to determine if it fits, relying on the fact that failed conversion always returns `2^63`.

It would be cool if we could replace `x - 2^63` with some other calculation, so that we don't have to load `2^63` from memory (increase both `.text` and `.rodata`). It *feels* possible: in this case, all valid values are from `2^63` to `2^64` exclusive, so the exponent is always `63`. We *just* need to extract the mantissa, shift it to the left by 11, and set the highest bit. In fact, I think just `shl x, 11` + `bts x, 63` works, since we're effectively just setting the hidden bit.

This might become an issue later if we try to add traps, but for now this reduces size. In fact, if we don't care about traps, we can use the same code for `i64.trunc_fnn_u` and `i32.trunc_fnn_u`. And same for the `_s` variants -- we can convert to `long` first and conditionally truncate the top 32 bits second.

That's 3728 bytes, and it doesn't even need much assembly.

---

The next large group is int-to-float conversions. This one should be easier.

Is it safe to implement `(float)i` as `(float)(double)i`? It *looks* safe, but I'm worried about edge cases.

Is it possible for `(float)i` to return +inf, but for `(float)(double)i` to return a finite number? `(float)i` is +inf if `i` is close to `f32`'s limit, i.e. at least halfway between the maximum finite value and +inf. `(double)i` should round this to a power of two that `(float)` will then interpret as the limit and return +inf. And LLVM seems to agree that `(float)(double)i == (float)i` as well. That's good enough for me.

I have a feeling that x86 can't handle unsigned integers here as well, or something. Yup, it needs similar special-casing in this direction, too.

GCC codegens from `unsigned long` conversion by checking the high bit, and if it's set, it shifts the value to the right by one (making sure to round correctly), converts it, and then doubles the result. LLVM uses something trickier.

Okay, that's clever. It reinterprets `0x43300000<low half>` and `0x45300000<high half>` as doubles. The former has an exponent of 52, so it's equal to `2^52 + low`. The latter is `2^84 + high * 2^32`. It then subtracts `2^52` and `2^84` respectively from both values and sums the results (with `vhaddpd` under `-Os`). Clever! These guys are cooking.

There are three steps to this approach:

- Interleaving halves and constants, done with `vpunpckldq`.
- Subtracting the constants, done with `vsubpd`.
- Summing up results, done with `vhaddpd`.

The issue with `vpunpckldq`/`vsubpd` is that it can't reuse a constant, because `vpunpckldq` requires it to be in form `<low const><high const><0><0>`, whereas `vsubpd` requires `<low const><0><high const><0>`. We could save 16 bytes on reusing constants if there was a way to use the same format with both. I'm thinking `vpshufd` and then reuse the constant.

Does `vpshufd` support reading from unaligned memory, i.e. our `stack`? https://github.com/StanfordPL/stoke/issues/381#issuecomment-68233729 says it does, and from my memory that checks out. Then maybe:

```asm
convert:
    vpshufd xmm0, [rdi], 0b11011000
    vorpd xmm0, xmm0, [rel const]
    vsubpd xmm0, xmm0, [rel const]
    vhaddpd xmm0, xmm0, xmm0
    ret

const:
    dq 0x4330000000000000, 0x4530000000000000
```

Having a 16-byte constant is suboptimal. Alternatively, it can be generated on the fly from an 8-byte constant:

```
convert:
    vpshufd xmm1, [rel const], 0b01110010
    vpshufd xmm0, [rdi], 0b11011000
    vorpd xmm0, xmm0, xmm1
    vsubpd xmm0, xmm0, xmm1
    vhaddpd xmm0, xmm0, xmm0
    ret

const:
    dq 0x4530000043300000
```

I guess this will have to do.

```c
static unsigned long magic_c = 0x4530000043300000;
double magic;
asm (
    "vpshufd $0b01110010, %[magic_c], %[magic];"
    "vpshufd $0b11011000, %[x], %[out];"
    "vorpd %[magic], %[out], %[out];"
    "vsubpd %[magic], %[out], %[out];"
    "vhaddpd %[out], %[out], %[out];"
    : [magic]"=&x"(magic), [out]"=&x"(out)
    : [magic_c]"m"(magic_c), [x]"x"(x)
);
```

It takes more code than it used to. *facepalm* Fine, let's copy the GCC lowering then.

After fixing a few bugs and applying optimizations assuming lack of traps, like in the previous section, I get 3848 bytes. I don't know why this is a larger increase than the previous time, even though the code looks similar. Probably the `switch`.

---

Let's try a simpler one for now -- the `reinterpret` family. That's just bitcasts, and so they don't need *any* lowering -- they're just `nop`s! That's a first. Still 3848 bytes.

---

This leaves demotions and promotions from the main instruction set. Technically those are "just" unary ops, so it's probably reasonable to treat them as integer unops and just inserts casts.

`cvtsd2ss` and `cvtss2sd` differ by the first byte (`0xf2` vs `0xf3`), but I have already been in this situation before -- `cvtpd2ps` and `cvtps2pd` differ by the presence of `0x66`, so I can reuse the REX-skipping mechanism to skip the precision override prefix if I use the packed version.

3880 bytes -- refreshingly small.

---

While working on these ops, I realized that there's one more place where I can use `0x66` instead of `0xf2`/`0xf3`. I can replace `addsd` with `addpd`, and similarly with other FP binops. 3872 bytes.

---

Also found a bug in `fnn.const`. 3936 bytes.

---

Optimized and fixed `fnn` comparisons. Patching was buggy, and I separately found a way to use `0x66`. 3928 bytes.

Separately from that, found another avenue for optimization in `fnn.min/max`: instead of patching the `0x66` prefix in or out, I can jump over it based on a flag. I need to insert two conditional jumps: over the prefix of `ucomisd` and then afterwards, over the prefix of `addpd`. With luck, I can use a flag that `ucomisd` doesn't reset so that I need to perform the test only once. Unfortunately, `ucomisd` affects all arithmetic flags, so I can't actually do that. But it's not too expensive, so I guess that's fine 3920 bytes.

---

I still can't believe there's no way to generate `NaN` safely in a size-independent manner. Surely there's some 64-bit arithmetic operation that produces something that looks a 32-bit `NaN`? `f32` reinterpreted as `f64` is always a subnormal. So, for example, `addsd` applied to 32-bit `f32`s actually adds them by bitwise value (overflowing into bit 32). This is not the correct output, but still. I don't know if I can apply this anywhere.

For now, I think I can optimize `round` and `sqrt` -- they currently use the same code, but their handling is very different, and I think I can optimize this. Both currently contain `xorps %0, %0` because `roundss`/`sqrtss` don't override the second 32-bit word, and I thought that was a problem; but since I'm reading from `stack` rather than `memory`, that word should be zero anyway.

It's 3936 bytes now, i.e. an increase of 16 bytes. I *think* this is due to `switch`, `PARSED`, and maybe `movq`. It's something that a jump table might help with, and the assembly version won't have problems with this.
