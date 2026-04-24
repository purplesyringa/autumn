use anyhow::{bail, ensure, Context, Result};
use bumpalo::Bump;
use std::collections::HashMap;
use std::ffi::OsStr;
use std::path::Path;
use std::process::Command;
use wast::{
    core::{
        BlockType, Expression, Func, FuncKind, FunctionType, ImportItems, Imports, InlineExport,
        Instruction, ItemKind, ItemSig, ModuleField, ModuleKind, NanPattern, TypeUse, WastArgCore,
        WastRetCore,
    },
    lexer::Lexer,
    parser::{parse, ParseBuffer},
    token::{Id, Index, Span},
    QuoteWat, Wast, WastArg, WastDirective, WastExecute, WastInvoke, WastRet, Wat,
};

fn main() -> Result<()> {
    let in_root = Path::new("../spec/test/core");
    let out_root = Path::new("../testsuite");

    // The only extension we support is `bulk-memory`.
    for test_dir in [in_root, &in_root.join("bulk-memory")] {
        for entry in test_dir.read_dir()? {
            let wast_path = entry?.path();
            if wast_path.extension().and_then(OsStr::to_str) != Some("wast") {
                continue;
            }
            match build_test(&wast_path) {
                Ok(bytes) => {
                    let rel_path = wast_path.strip_prefix(test_dir).context("strip_prefix")?;
                    let out_path = out_root.join(rel_path).with_extension("wasm");
                    std::fs::write(out_path, bytes).context("write")?;
                }
                Err(err) => println!("{:?}\n\n", err.context(format!("in file {:?}", wast_path))),
            }
        }
    }
    Ok(())
}

#[derive(Debug)]
struct Module<'a> {
    name: Option<&'a str>,
    fields: Vec<ModuleField<'a>>,
    exports: HashMap<&'a str, Index<'a>>,
}

fn load_module<'a>(
    quote_wat: QuoteWat<'a>,
    arena: &'a Bump,
) -> Result<(Option<Id<'a>>, Module<'a>)> {
    let (id, mut fields) = match quote_wat {
        QuoteWat::Wat(Wat::Module(module)) => match module.kind {
            ModuleKind::Text(fields) => (module.id, fields),
            ModuleKind::Binary(_) => bail!("binary modules are not supported"),
        },
        QuoteWat::QuoteModule(..) => bail!("quoted modules are not supported"),
        QuoteWat::Wat(Wat::Component(_)) | QuoteWat::QuoteComponent(..) => {
            bail!("components are not supported");
        }
    };

    let mut last_ad_hoc_id = 0;
    let mut inline_export =
        |id: &mut Option<Id<'a>>, span: Span, exports: &InlineExport<'a>| -> (Vec<&str>, Index) {
            // Assign an ID to this item if it's missing.
            let id = *id.get_or_insert_with(|| {
                last_ad_hoc_id += 1;
                Id::new(arena.alloc(format!("adhoc{last_ad_hoc_id}")), span)
            });
            (exports.names.clone(), Index::Id(id))
        };

    let mut exports = HashMap::new();
    for field in &mut fields {
        let (names, index) = match field {
            ModuleField::Func(func) if !func.exports.names.is_empty() => {
                inline_export(&mut func.id, func.span, &func.exports)
            }
            ModuleField::Global(global) if !global.exports.names.is_empty() => {
                inline_export(&mut global.id, global.span, &global.exports)
            }
            ModuleField::Export(export) => (vec![export.name], export.item),
            _ => continue,
        };
        for name in names {
            exports.insert(name, index);
        }
    }

    Ok((
        id,
        Module {
            name: None,
            fields,
            exports,
        },
    ))
}

impl<'a> Module<'a> {
    fn get_export(&self, name: &str) -> Result<Index<'a>> {
        self.exports
            .get(name)
            .copied()
            .with_context(|| format!("undefined import {name}"))
    }
}

fn do_invoke<'a>(
    module: &Module<'a>,
    invoke: &WastInvoke<'a>,
    instrs: &mut Vec<Instruction<'a>>,
) -> Result<()> {
    for arg in &invoke.args {
        let WastArg::Core(arg) = arg else {
            bail!("components are not supported");
        };
        let insn = match arg {
            WastArgCore::I32(x) => Instruction::I32Const(*x),
            WastArgCore::I64(x) => Instruction::I64Const(*x),
            WastArgCore::F32(x) => Instruction::F32Const(*x),
            WastArgCore::F64(x) => Instruction::F64Const(*x),
            _ => bail!("unsupported arg {arg:?}"),
        };
        instrs.push(insn);
    }
    instrs.push(Instruction::Call(module.get_export(invoke.name)?));
    Ok(())
}

// Pushes `1i32` on success, `0i32` on failure.
fn do_check_single_result<'a>(
    result: &WastRetCore<'a>,
    instrs: &mut Vec<Instruction<'a>>,
) -> Result<()> {
    match result {
        WastRetCore::I32(x) => {
            instrs.push(Instruction::I32Const(*x));
            instrs.push(Instruction::I32Eq);
        }
        WastRetCore::I64(x) => {
            instrs.push(Instruction::I64Const(*x));
            instrs.push(Instruction::I64Eq);
        }
        WastRetCore::F32(pat) => {
            instrs.push(Instruction::I32ReinterpretF32);
            match pat {
                NanPattern::CanonicalNan | NanPattern::ArithmeticNan => {
                    instrs.push(Instruction::I32Const(i32::MAX));
                    instrs.push(Instruction::I32And);
                    instrs.push(Instruction::I32Const(0x7fc00000));
                    if let NanPattern::CanonicalNan = pat {
                        instrs.push(Instruction::I32Eq);
                    } else {
                        instrs.push(Instruction::I32GeU);
                    }
                }
                NanPattern::Value(x) => {
                    instrs.push(Instruction::I32Const(x.bits as i32));
                    instrs.push(Instruction::I32Eq);
                }
            }
        }
        WastRetCore::F64(pat) => {
            instrs.push(Instruction::I64ReinterpretF64);
            match pat {
                NanPattern::CanonicalNan | NanPattern::ArithmeticNan => {
                    instrs.push(Instruction::I64Const(i64::MAX));
                    instrs.push(Instruction::I64And);
                    instrs.push(Instruction::I64Const(0x7ff8000000000000));
                    if let NanPattern::CanonicalNan = pat {
                        instrs.push(Instruction::I64Eq);
                    } else {
                        instrs.push(Instruction::I64GeU);
                    }
                }
                NanPattern::Value(x) => {
                    instrs.push(Instruction::I64Const(x.bits as i64));
                    instrs.push(Instruction::I64Eq);
                }
            }
        }
        WastRetCore::Either(_) => bail!("assert_return with either is unsupported"),
        _ => bail!("unsupported result {result:?}"),
    }
    Ok(())
}

fn empty_block_type<'a>() -> Box<BlockType<'a>> {
    Box::new(BlockType {
        label: None,
        label_name: None,
        ty: TypeUse {
            index: None,
            inline: None,
        },
    })
}

fn do_assert_return<'a>(results: &[WastRet<'a>], instrs: &mut Vec<Instruction<'a>>) -> Result<()> {
    for result in results.iter().rev() {
        let WastRet::Core(result) = result else {
            bail!("components are not supported");
        };
        do_check_single_result(result, instrs)?;
        instrs.push(Instruction::If(empty_block_type()));
        instrs.push(Instruction::Else(None));
        instrs.push(Instruction::Unreachable);
        instrs.push(Instruction::End(None));
    }
    Ok(())
}

fn make_func<'a>(span: Span, name: &'a str, instrs: Vec<Instruction<'a>>) -> ModuleField<'a> {
    ModuleField::Func(Func {
        span,
        id: Some(Id::new(name, span)),
        name: None,
        exports: InlineExport { names: vec![name] },
        kind: FuncKind::Inline {
            locals: Box::new([]),
            expression: Expression {
                instrs: instrs.into(),
                branch_hints: Box::new([]),
                instr_spans: None,
            },
        },
        ty: TypeUse {
            index: None,
            inline: None,
        },
    })
}

struct TestSet<'a> {
    text: &'a str,
    arena: &'a Bump,
    modules: Vec<Module<'a>>,
    module_ids: HashMap<Option<Id<'a>>, usize>,
    tests: Vec<(usize, &'a str)>,
}

impl<'a> TestSet<'a> {
    fn new(text: &'a str, arena: &'a Bump) -> Self {
        Self {
            text,
            arena,
            modules: Vec::new(),
            module_ids: HashMap::new(),
            tests: Vec::new(),
        }
    }

    fn try_load_module(&mut self, quote_wat: QuoteWat<'a>) {
        if let Ok((id, module)) = load_module(quote_wat, self.arena) {
            self.module_ids.insert(id, self.modules.len());
            self.modules.push(module);
        }
    }

    fn register(&mut self, name: &'a str, id: Option<Id<'a>>) {
        if let Some(module_index) = self.module_ids.get_mut(&id) {
            self.modules[*module_index].name = Some(name);
        }
    }

    fn add_test(
        &mut self,
        module_id: Option<Id<'a>>,
        span: Span,
        f: impl FnOnce(&Module<'a>, &mut Vec<Instruction<'a>>) -> Result<()>,
    ) -> Result<()> {
        let Some(module_index) = self.module_ids.get(&module_id) else {
            return Ok(()); // skip
        };
        let module = &mut self.modules[*module_index];

        // Use line and column number as part of the function name for easier debugging. This also
        // enforces uniqueness.
        let (line, col) = span.linecol_in(self.text);
        let name = self.arena.alloc(format!("test {}:{}", line + 1, col + 1));

        let mut instrs = Vec::new();
        f(module, &mut instrs).with_context(|| format!("in {name}"))?;

        module.fields.push(make_func(span, name, instrs));

        self.tests.push((*module_index, name));

        Ok(())
    }

    fn finish_tests(&mut self) {
        let mut last_ad_hoc_id = 0;
        let mut fields: Vec<ModuleField<'a>> = Vec::new();
        let mut instrs: Vec<Instruction<'a>> = Vec::new();

        for (module_index, func_name) in self.tests.drain(..) {
            let module_name = self.modules[module_index].name.get_or_insert_with(|| {
                last_ad_hoc_id += 1;
                self.arena.alloc(format!("adhoc{last_ad_hoc_id}"))
            });
            let id = Id::new(func_name, Span::from_offset(0));
            fields.push(ModuleField::Import(Imports {
                span: Span::from_offset(0),
                items: ImportItems::Single {
                    module: module_name,
                    name: func_name,
                    sig: ItemSig {
                        span: Span::from_offset(0),
                        id: Some(id),
                        name: None,
                        kind: ItemKind::Func(TypeUse {
                            index: None,
                            inline: Some(FunctionType {
                                params: Box::new([]),
                                results: Box::new([]),
                            }),
                        }),
                    },
                },
            }));
            instrs.push(Instruction::Call(Index::Id(id)));
        }
        fields.push(make_func(Span::from_offset(0), "_start", instrs));

        self.modules.push(Module {
            name: Some("test"),
            fields,
            exports: HashMap::new(),
        });
    }
}

fn build_test(wast_path: &Path) -> Result<Vec<u8>> {
    let text = std::fs::read_to_string(wast_path).context("read")?;
    let mut lexer = Lexer::new(&text);
    lexer.allow_confusing_unicode(true);
    let buf = ParseBuffer::new_with_lexer(lexer).context("lex")?;
    let ast: Wast = parse(&buf).context("parse")?;

    let arena = Bump::new();
    let mut test_set = TestSet::new(&text, &arena);

    for directive in ast.directives {
        match directive {
            WastDirective::Module(quote_wat) => test_set.try_load_module(quote_wat),
            WastDirective::Register { name, module, .. } => test_set.register(name, module),

            // Skip unsupported tests
            WastDirective::Invoke(invoke) => {
                let _ = test_set.add_test(invoke.module, invoke.span, |module, instrs| {
                    do_invoke(module, &invoke, instrs)
                });
            }
            WastDirective::AssertReturn {
                span,
                exec,
                results,
            } => match exec {
                WastExecute::Invoke(invoke) => {
                    let _ = test_set.add_test(invoke.module, span, |module, instrs| {
                        do_invoke(module, &invoke, instrs)?;
                        do_assert_return(&results, instrs)
                    });
                }
                WastExecute::Get { module, global, .. } => {
                    let _ = test_set.add_test(module, span, |module, instrs| {
                        instrs.push(Instruction::GlobalGet(module.get_export(global)?));
                        do_assert_return(&results, instrs)
                    });
                }
                WastExecute::Wat(_) => bail!("assert_return with WAT is not supported"),
            },

            WastDirective::ModuleDefinition(_)
            | WastDirective::ModuleInstance { .. }
            | WastDirective::AssertMalformed { .. }
            | WastDirective::AssertInvalid { .. }
            | WastDirective::AssertExhaustion { .. }
            | WastDirective::AssertUnlinkable { .. }
            | WastDirective::AssertTrap { .. } => {}

            _ => bail!("unsupported directive {directive:?}"),
        }
    }

    ensure!(!test_set.tests.is_empty(), "no tests found");

    test_set.finish_tests();

    let mut modules = Vec::new();

    for module in test_set.modules {
        let Some(module_name) = module.name else {
            continue; // must be only used for unsupported assertions
        };

        let encoded_module = wast::core::Module {
            span: Span::from_offset(0),
            id: None,
            name: None,
            kind: ModuleKind::Text(module.fields),
        }
        .encode()
        .with_context(|| format!("encoding failed in {:?}", module_name))?;

        modules.push((module_name, encoded_module));
    }

    let tmpdir = tempfile::tempdir().context("create tempdir")?;
    let mut command = Command::new("wasm-merge");
    for (name, contents) in modules {
        let file_path = tmpdir.path().join(format!("{name}.wasm"));
        std::fs::write(&file_path, contents).context("write file")?;
        command.arg(file_path).arg(name);
    }
    let output_path = tmpdir.path().join("output.wasm");
    let output = command
        .arg("-o")
        .arg(&output_path)
        .arg("--rename-export-conflicts")
        .arg("--mvp-features")
        .arg("--enable-mutable-globals")
        .arg("--enable-multivalue")
        .arg("--enable-sign-ext")
        .arg("--enable-nontrapping-float-to-int")
        .arg("--enable-bulk-memory-opt")
        .arg("--enable-extended-const")
        .arg("--enable-call-indirect-overlong")
        .arg("-g")
        .output()
        .context("wasm-merge")?;
    if !output.status.success() {
        bail!(
            "wasm-merge returned error: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    Ok(std::fs::read(output_path).context("read merged wasm")?)
}
