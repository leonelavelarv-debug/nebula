// ppc-lower [--fast|--native] <address-hex> <file.bin>: lowers a big-endian PowerPC code range with Nebula's
// translator. Default: the plain exact lowering. --native: native float + flat RAM + register residency
// (bit-exact fallbacks kept). --fast: also fast float (Dolphin-like accuracy), as Nebula's fastest module.
use nebula_recomp_core::{audit_lower_ppc_code_range_with_module_options, lower_ppc_code_range, ModuleTranslationOptions};
fn main() {
    let mut a: Vec<String> = std::env::args().skip(1).collect();
    let mode = if a[0].starts_with("--") { a.remove(0) } else { String::new() };
    let addr = u32::from_str_radix(a[0].trim_start_matches("0x"), 16).unwrap();
    let bytes = std::fs::read(&a[1]).unwrap();
    let result = if mode.is_empty() {
        lower_ppc_code_range(addr, &bytes)
    } else {
        let mut o = ModuleTranslationOptions::default();
        o.native_float = true;
        o.flat_ram_reads = true;
        o.flat_ram_writes = true;
        o.guest_resident_integer = true;
        if mode == "--fast" { o.fast_float = true; o.guest_resident_float = true; o.hw_float_conversions = true; }
        audit_lower_ppc_code_range_with_module_options(addr, &bytes, &o)
    };
    match result {
        Ok(s) => print!("{s}"),
        Err(e) => { eprintln!("error: {e}"); std::process::exit(1) }
    }
}
