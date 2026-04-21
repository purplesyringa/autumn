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

fn model_mask(model: u8) -> u64 {
    (0..8)
        .map(|i| (((model >> i) & 1) as u64 * 0xFF) << (8 * i))
        .sum()
}

impl ModelCounter {
    fn get_mut(&mut self, model: u8, prev_bytes: u64, next_byte: u8) -> &mut Counter {
        let x86_model = ((model << 1) | 1).reverse_bits();
        let mask = model_mask(x86_model);
        let arg = ((prev_bytes << 8) | next_byte as u64).swap_bytes() & mask;
        let hash = unsafe { core::arch::x86_64::_mm_crc32_u64(x86_model as u64, arg) } ^ arg;
        let key = (hash >> 1) as usize % self.map.len();
        &mut self.map[key]
    }
}

struct Encoder {
    bits: Vec<bool>,
    left: u32,
    range: u32,
}

impl Encoder {
    pub fn new() -> Self {
        Self {
            bits: vec![],
            left: 0,
            range: u32::MAX,
        }
    }

    fn increment(&mut self) {
        for bit in self.bits.iter_mut().rev() {
            if !*bit {
                *bit = true;
                break;
            }
            *bit = false;
        }
    }

    pub fn encode_bit(&mut self, bit: u8, numerator: usize, denominator: usize) {
        let mid = ((self.range as u64 * numerator as u64) / denominator as u64) as u32;
        if bit == 1 {
            self.range = mid;
        } else {
            let carry;
            (self.left, carry) = self.left.overflowing_add(mid);
            if carry {
                self.increment();
            }
            self.range -= mid;
        }
        while self.range < (1 << 31) {
            self.bits.push((self.left >> 31) == 1);
            self.left <<= 1;
            self.range <<= 1;
        }
    }

    fn finalize(&mut self) {
        if self.left.overflowing_add(self.range - 1).1 {
            self.increment();
        } else {
            self.bits.push(true);
        }
    }

    fn pad_to_eight(&mut self) {
        let pad = (8 - self.bits.len()) % 8;
        for _ in 0..pad {
            self.bits.push(false);
        }
    }

    pub fn finish(mut self) -> (u32, Vec<u8>) {
        self.finalize();
        let initial = self
            .bits
            .drain(..32)
            .enumerate()
            .filter(|(_, b)| *b)
            .map(|(i, _)| 1 << (31 - i))
            .sum();
        self.pad_to_eight();

        let (bytes_bits, remainder) = self.bits.as_chunks::<8>();
        assert!(remainder.is_empty());

        let mut out = vec![];
        for &[b0, b1, b2, b3, b4, b5, b6, b7] in bytes_bits {
            out.push(
                (b0 as u8)
                    + ((b1 as u8) << 1)
                    + ((b2 as u8) << 2)
                    + ((b3 as u8) << 3)
                    + ((b4 as u8) << 4)
                    + ((b5 as u8) << 5)
                    + ((b6 as u8) << 6)
                    + ((b7 as u8) << 7),
            );
        }

        (initial, out)
    }
}

// TODO: weights (as popcnt)?
fn compress_with_models(
    bytes: &[u8],
    models: &[bool; 128],
    size_only: bool,
) -> (f64, (u32, Vec<u8>)) {
    let mut encoder = Encoder::new();
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
                let c = stats.get_mut(*model, prev_bytes, next_byte);
                c0 += c.c0 as usize;
                c1 += c.c1 as usize;
            }

            let p = ((if bit == 0 { c0 } else { c1 }) << 16) / (c0 + c1);
            if !size_only {
                encoder.encode_bit(bit, c1, c0 + c1);
            }
            size -= (p as f64 / 65536.).log2();

            for model in models.iter().rev() {
                stats.get_mut(*model, prev_bytes, next_byte).learn(bit);
            }

            next_byte = next_byte * 2 + bit;
        }
        prev_bytes = prev_bytes << 8 | *byte as u64;
    }

    let out = if !size_only {
        encoder.finish()
    } else {
        (0, vec![])
    };

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
        .rev()
        .filter(|i| models[*i as usize])
        .map(|i| ((i << 1) | 1).reverse_bits())
        .collect::<Vec<_>>();
    dbg!(model_list.len());
    println!("{model_list:02x?}");
    std::fs::write("../models.bin", model_list).unwrap();
    let (_, (initial, out)) = compress_with_models(&bytes, &models, false);
    dbg!(out.len());
    std::fs::write("../initial.txt", format!("{initial}")).unwrap();
    std::fs::write("../compressed.bin", out).unwrap();
}
