// opcode-coverage: which Broadway (Gekko/Broadway) instructions Nebula's translator lowers. For every opcode the
// powerpc crate decodes, one representative word (rD=3, rA=4, rB=5, rC=6) is lowered as `<insn>; blr`.
use nebula_recomp_core::lower_ppc_code_range;
use powerpc::{Extensions, Ins, Opcode};
use std::collections::BTreeMap;

fn main() {
    let ext = Extensions::gekko_broadway();
    let mut seen: BTreeMap<String, (u32, String)> = BTreeMap::new();
    for primary in 0u32..64 {
        for xo in 0u32..1024 {
            for rc in 0u32..2 {
                let fields = (3 << 21) | (4 << 16) | (5 << 11);
                let word = (primary << 26) | fields | (xo << 1) | rc;
                let ins = Ins::new(word, ext);
                if ins.op == Opcode::Illegal { continue; }
                let name = format!("{}{}", ins.op.mnemonic(), if rc == 1 && (primary == 31 || primary == 63 || primary == 59 || primary == 4) { "." } else { "" });
                if seen.contains_key(&name) { continue; }
                // Branches: aim inside the range (to the blr).
                let w = match primary { 16 => (16 << 26) | (20 << 21) | 8, 18 => (18 << 26) | 8, _ => word };
                let mut bytes = w.to_be_bytes().to_vec();
                bytes.extend_from_slice(&0x4E80_0020u32.to_be_bytes());
                let verdict = match lower_ppc_code_range(0x8000_4000, &bytes) {
                    Ok(_) => "ok".to_string(),
                    Err(e) => format!("{e}"),
                };
                seen.insert(name, (w, verdict));
            }
        }
    }
    let (mut ok, mut no) = (0, 0);
    for (name, (w, v)) in &seen {
        if v == "ok" { ok += 1 } else { no += 1 }
        println!("{name},{w:08X},{}", v.replace(',', ";"));
    }
    eprintln!("opcodes={} lowered={} rejected={}", seen.len(), ok, no);
}
