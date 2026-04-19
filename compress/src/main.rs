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

fn model_mask(model: u8) -> u64 {
    (0..8)
        .map(|i| (((model >> i) & 1) as u64 * 0xFF) << (8 * i))
        .sum()
}

fn apply_model_mask(prev_bytes: u64, model: u8) -> u64 {
    prev_bytes & model_mask(model)
}

// TODO: weights (as popcnt)?
fn compress_with_models(bytes: &[u8], models: &[bool; 128]) -> f64 {
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

    size / 8. + models.len() as f64
}

fn main() {
    let bytes = std::fs::read("../interp-small").unwrap();
    let mut models = [false; 128];
    let mut size = f64::INFINITY;
    dbg!(bytes.len());
    for model in 0..=127 {
        models[model] = true;
        let next_size = compress_with_models(&bytes, &models);
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
                let next_size = compress_with_models(&bytes, &models);
                if next_size >= size {
                    models[i] = true;
                } else {
                    size = next_size;
                    println!("R{i:02x}/{i:08b}: {size}");
                }
            }
        }
    }
    let model_list = (0..=127).filter(|i| models[*i]).collect::<Vec<_>>();
    dbg!(model_list);
}
