use std::collections::HashMap;

#[derive(Clone)]
struct Counter {
    c0: usize,
    c1: usize,
}

impl Default for Counter {
    fn default() -> Self {
        Self { c0: 0, c1: 0 }
    }
}

impl Counter {
    #[inline]
    fn adjust(cyes: &mut usize, cno: &mut usize) {
        *cyes += 1;
        if *cno > 1 {
            *cno /= 2;
        }
    }
    pub fn add_zero(&mut self) {
        Self::adjust(&mut self.c0, &mut self.c1)
    }
    pub fn add_one(&mut self) {
        Self::adjust(&mut self.c1, &mut self.c0)
    }
    pub fn learn(&mut self, bit: u8) {
        if bit == 0 {
            self.add_zero();
        } else {
            self.add_one();
        }
    }
}

#[derive(Clone, Default)]
struct ModelCounter {
    // masked past bytes, current in-progress byte
    map: HashMap<(u64, u8), Counter>,
}

impl ModelCounter {
    fn get_mut(&mut self, past_bytes: u64, in_progress: u8) -> &mut Counter {
        self.map.entry((past_bytes, in_progress)).or_default()
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
fn compress_with_models(bytes: &[u8], models: &[bool; 256]) -> f64 {
    let mut size = 0.;
    let mut stats = core::array::from_fn::<_, 256, _>(|_| ModelCounter::default());
    let mut prev_bytes = 0;
    for byte in bytes {
        let mut next_byte = 1;
        for bit_index in (0..8).rev() {
            let bit = (byte >> bit_index) & 1;

            let mut c0 = 1;
            let mut c1 = 1;
            for model in models
                .iter()
                .enumerate()
                .filter(|(_i, v)| **v)
                .map(|(i, _v)| i)
            {
                let c = stats[model].get_mut(apply_model_mask(prev_bytes, model as u8), next_byte);
                let w = model.count_ones() as usize; // + (c.c0 == 0 || c.c1 == 0) as usize * 2;
                c0 += c.c0 << w;
                c1 += c.c1 << w;
                c.learn(bit);
            }

            let p = (if bit == 0 { c0 } else { c1 }) as f64 / (c0 + c1) as f64;
            // eprintln!("{p}");
            size -= p.log2();

            next_byte = next_byte * 2 + bit;
        }
        prev_bytes = prev_bytes << 8 | *byte as u64;
    }

    size / 8. + models.iter().filter(|v| **v).count() as f64
}

fn main() {
    let bytes = std::fs::read("../interp-small").unwrap();
    let mut models = [false; 256];
    let mut size = f64::INFINITY;
    dbg!(bytes.len());
    for model in 0..=255 {
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
    let model_list = (0..=255).filter(|i| models[*i]).collect::<Vec<_>>();
    dbg!(model_list);
}
