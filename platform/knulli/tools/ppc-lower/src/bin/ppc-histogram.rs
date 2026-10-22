// ppc-histogram <main.dol | StaticR.rel>...: opcode histogram of a game's executable code, and which of those
// opcodes Nebula's translator does not lower yet. Runs on the PC on the user's own extracted files (Mario Kart
// Wii: sys/main.dol and files/rel/StaticR.rel); nothing of the game leaves the machine.
use nebula_recomp_core::{lower_ppc_code_range, DolImage};
use powerpc::{Extensions, Ins, Opcode};
use std::collections::BTreeMap;

fn be32(d: &[u8], o: usize) -> u32 { u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) }

/// Executable section byte ranges of a REL module (header: section count at 0x0C, table offset at 0x10; each
/// entry is {offset | exec flag in bit 0, size}).
fn rel_code(d: &[u8]) -> Vec<(usize, usize)> {
    let count = be32(d, 0x0C) as usize;
    let table = be32(d, 0x10) as usize;
    (0..count)
        .filter_map(|i| {
            let off = be32(d, table + 8 * i);
            let size = be32(d, table + 8 * i + 4) as usize;
            (off & 1 == 1 && size > 0).then(|| ((off & !3) as usize, size))
        })
        .filter(|(o, s)| o + s <= d.len())
        .collect()
}

fn main() {
    let ext = Extensions::gekko_broadway();
    let mut histogram: BTreeMap<&'static str, (u64, u32)> = BTreeMap::new();
    let (mut words, mut illegal) = (0u64, 0u64);
    for path in std::env::args().skip(1) {
        let data = std::fs::read(&path).expect("read input");
        let ranges: Vec<(usize, usize)> = if path.to_lowercase().ends_with(".rel") {
            rel_code(&data)
        } else {
            let dol = DolImage::parse(&data).expect("parse DOL");
            dol.text_sections().map(|s| { let r = s.file_range(); (r.start, r.end - r.start) }).collect()
        };
        let mut file_words = 0u64;
        for (start, size) in ranges {
            for o in (start..start + size).step_by(4) {
                if o + 4 > data.len() { break; }
                let w = be32(&data, o);
                let ins = Ins::new(w, ext);
                words += 1; file_words += 1;
                if ins.op == Opcode::Illegal { illegal += 1; continue; }
                let e = histogram.entry(ins.op.mnemonic()).or_insert((0, w));
                e.0 += 1;
            }
        }
        eprintln!("{path}: {file_words} palabras de codigo");
    }
    let mut rows: Vec<_> = histogram.into_iter().collect();
    rows.sort_by(|a, b| b.1 .0.cmp(&a.1 .0));
    let mut unsupported = Vec::new();
    for (name, (count, sample)) in &rows {
        let first = *sample;
        let primary = first >> 26;
        let w = match primary { 16 => (first & !0xFFFC) | 8, 18 => (18 << 26) | 8 | (first & 1), _ => first };
        let mut bytes = w.to_be_bytes().to_vec();
        bytes.extend_from_slice(&0x4E80_0020u32.to_be_bytes());
        let ok = match lower_ppc_code_range(0x8000_4000, &bytes) {
            Ok(_) => true,
            Err(e) => { let m = e.to_string(); !m.contains("unsupported instruction") }
        };
        println!("{name},{count},{}", if ok { "ok" } else { "NO SOPORTADA" });
        if !ok { unsupported.push((*name, *count)); }
    }
    eprintln!("palabras={words} ilegales/datos={illegal} opcodes={} sin soporte={:?}", rows.len(), unsupported);
}
