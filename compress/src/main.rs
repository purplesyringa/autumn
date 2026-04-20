#[derive(Clone, Copy)]
struct Counter {
    c0: u8,
    c1: u8,
}

impl Counter {
    pub fn learn(&mut self, bit: u8) {
        self.c0 += 1;
        self.c1 += 1;
        if self.c0 == 0 {
            self.c1 += 1;
        }
        if bit == 0 {
            self.c1 /= 2;
        } else {
            self.c0 /= 2;
        }
    }
}

#[derive(Clone)]
struct ModelCounter {
    // masked past bytes, current in-progress byte
    map: Box<[Counter; const { 1 << 24 }]>,
}

impl Default for ModelCounter {
    fn default() -> Self {
        Self {
            map: Box::new([Counter { c0: 0, c1: 0 }; _]),
        }
    }
}

impl ModelCounter {
    fn get_mut(&mut self, model: u8, past_bytes: u64, next_byte: u8) -> &mut Counter {
        let hash =
            unsafe { core::arch::x86_64::_mm_crc32_u64(0, past_bytes << 8 | next_byte as u64) }
                ^ model as u64;
        let key = hash as usize % self.map.len();
        &mut self.map[key]
    }
}

struct Encoder<W: FnMut(u8)> {
    write_byte: W,
    l: u32,
    r: u32,
}

impl<W: FnMut(u8)> Encoder<W> {
    fn new(write_byte: W) -> Self {
        Self {
            write_byte,
            l: 0,
            r: u32::MAX,
        }
    }
    fn encode_bit(&mut self, bit: u8, prob0: u16) {
        let mid = (self.l as u64 + (((self.r - self.l) as u64 * prob0 as u64) >> 16)) as u32;
        if bit == 0 {
            self.r = mid;
        } else {
            self.l = mid + 1;
        }
        while (self.l >> 24) == (self.r >> 24) {
            (self.write_byte)((self.l >> 24) as u8);
            self.l <<= 8;
            self.r <<= 8;
            self.r += 0xff;
        }
    }
    fn finish(mut self) {
        self.l
            .to_be_bytes()
            .iter()
            .for_each(|b| (self.write_byte)(*b))
    }
}

fn model_mask(model: u8) -> u64 {
    (0..8)
        .map(|i| (((model >> i) & 1) as u64 * 0xFF) << (8 * i))
        .sum()
}

fn apply_model_mask(prev_bytes: u64, model: u8) -> u64 {
    prev_bytes & model_mask(model)
}

// TODO: weights (as popcnt)?
fn compress_with_models(bytes: &[u8], models: &[bool; 128], size_only: bool) -> (f64, Vec<u8>) {
    let mut out = vec![];
    let mut encoder = Encoder::new(|b| out.push(b));
    let mut size = 0.;
    let mut stats = ModelCounter::default();
    let mut prev_bytes = 0;
    let models = models
        .iter()
        .enumerate()
        .filter(|(_i, v)| **v)
        .map(|(i, _v)| i as u8)
        .collect::<Vec<_>>();
    for byte in bytes {
        let mut next_byte = 1;
        for bit_index in (0..8).rev() {
            let bit = (byte >> bit_index) & 1;

            let mut c0: usize = 1;
            let mut c1: usize = 1;
            for model in &models {
                let c = stats.get_mut(*model, apply_model_mask(prev_bytes, *model), next_byte);
                c0 += c.c0 as usize;
                c1 += c.c1 as usize;
            }

            let p = ((if bit == 0 { c0 } else { c1 }) << 16) / (c0 + c1);
            if !size_only {
                let p0 = (c0 << 16) / (c0 + c1);
                encoder.encode_bit(bit, p0 as u16);
            }
            size -= (p as f64 / 65536.).log2();

            for model in &models {
                stats
                    .get_mut(*model, apply_model_mask(prev_bytes, *model), next_byte)
                    .learn(bit);
            }

            next_byte = next_byte * 2 + bit;
        }
        prev_bytes = prev_bytes << 8 | *byte as u64;
    }

    if !size_only {
        encoder.finish();
    }

    (size / 8. + models.len() as f64, out)
}

fn main() {
    let bytes = std::fs::read("../interp-small.bin").unwrap();
    let mut models = [false; 128];
    let mut size = f64::INFINITY;
    dbg!(bytes.len());
    for model in 0..=127 {
        models[model] = true;
        let (next_size, _) = compress_with_models(&bytes, &models, true);
        if next_size >= size {
            models[model] = false;
        } else {
            size = next_size;
            println!("A{model:02x}/{model:08b}: {size}");

            // try remove
            for i in 0..model {
                if !models[i] {
                    continue;
                }
                models[i] = false;
                let (next_size, _) = compress_with_models(&bytes, &models, true);
                if next_size >= size {
                    models[i] = true;
                } else {
                    size = next_size;
                    println!("R{i:02x}/{i:08b}: {size}");
                }
            }
        }
    }
    let model_list = (0..=127u8)
        .filter(|i| models[*i as usize])
        .collect::<Vec<_>>();
    dbg!(model_list.len());
    println!("{model_list:02x?}");
    std::fs::write("../models.bin", model_list).unwrap();
    let (_, compressed) = compress_with_models(&bytes, &models, false);
    dbg!(compressed.len());
    std::fs::write("../compressed.bin", compressed).unwrap();
}
