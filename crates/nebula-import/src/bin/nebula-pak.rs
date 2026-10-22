//! nebula-pak <disc image> <game.pak>: the native game's data from the player's own Super Mario Galaxy disc image
//! (ISO, RVZ, WIA, WBFS…), as Android's setup makes it, on any system with Rust (Linux, Termux, macOS, Windows).
use std::fs::File;
use std::io::Write;
use std::path::Path;

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("uso: nebula-pak <imagen del disco> <game.pak>");
        std::process::exit(2);
    }
    let open = || -> anyhow::Result<nebula_import::DiscFile> { Ok(nebula_import::DiscFile::new(File::open(&args[1])?)) };
    let identity = nebula_import::identify(open()?)?;
    println!("{}", identity.message);
    if !identity.supported {
        std::process::exit(1);
    }
    let mut last = 0u64;
    let written = nebula_import::build_pak(open()?, Path::new(&args[2]), &mut |done, total| {
        let percent = if total > 0 { done * 100 / total } else { 0 };
        if percent != last {
            last = percent;
            print!("\r{percent:3}% ({} / {} MiB)", done >> 20, total >> 20);
            let _ = std::io::stdout().flush();
        }
        true
    })?;
    println!("\ngame.pak listo: {} MiB de datos", written >> 20);
    Ok(())
}
