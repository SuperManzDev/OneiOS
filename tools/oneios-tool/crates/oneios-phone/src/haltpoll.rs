//! `oneios-tool phone haltpoll status | load | unload`: Droid-VM's gunyah_halt_poll module (GPL; their prebuilt for GKI 6.6,
//! DroidVM-Prebuilt-Root f7c6ce8 manual-build/arm64-v8a/usr/lib/modules/android15-6.6/gunyah-halt-poll-gki-6.6.ko, built
//! from gunyah_host_mod e33efae; its SHA-256 is pinned below, the file fetched by hand to /mnt/tkg-build/oneios/dl/halt-poll).
//! When a vCPU idles, the
//! built-in gunyah_vcpu driver puts its thread to sleep at once, and the next interrupt for it (a GPU or disk completion,
//! a timer) wakes it the long way: the hypervisor, an interrupt on the phone's first core, the scheduler. The module
//! waits a moment first (at most 200 us), as KVM's halt polling does, growing and shrinking the wait per vCPU so an idle
//! VM costs next to nothing. The built-in driver is not changed. Its switch: halt_poll_ns (0 = off) at run time; it
//! cannot be unloaded while a VM runs (docs/native.md "The GPU in the shared-RAM VM").
//! - `status`: its parameters (named files), read only.
//! - `load`: refused during a session or when it is loaded; the .ko pushed, its SHA-256 checked, a `sync` and `insmod`.
//! - `unload`: rmmod (the kernel refuses while a VM runs).
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

/// The module's folder on the phone, the .ko there and its parameters (the module is gunyah_halt_poll_gki_6.6).
pub const DIR: &str = "/data/local/tmp/oneios-haltpoll";
pub const KO: &str = "/data/local/tmp/oneios-haltpoll/gunyah-halt-poll-gki-6.6.ko";
pub const PARAMS: &str = "/sys/module/gunyah_halt_poll_gki_6.6/parameters";
/// The prebuilt's SHA-256 (its git blob is 6e77dd1fafd17336ff7bbd6611c867f4c1c9d923 in DroidVM-Prebuilt-Root f7c6ce8).
pub const SHA256: &str = "c3adc55e4260282f060224bed3d6a8d2e8042d3afc23f61158be3d3e142848ea";

/// `status`'s read on the phone.
pub fn status_script() -> String {
    format!(
        "if [ -d {PARAMS} ]; then echo '=== loaded'; for p in halt_poll_ns halt_poll_grow halt_poll_grow_start halt_poll_shrink; do \
         echo \"$p: $(cat {PARAMS}/$p 2>/dev/null)\"; done\n\
         else echo '=== not loaded'; fi\n\
         echo '=== kernel lines'\ndmesg 2>/dev/null | grep -i 'halt_poll' | tail -n 8\n"
    )
}

/// `phone haltpoll status`.
pub fn status(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    print!("{}", adb.root_ok(&status_script(), "the module's status")?);
    Ok(())
}

/// `phone haltpoll load`.
pub fn load(adb: &Adb, ko: &str, out: &str) -> Result<()> {
    let data = fs::read(ko).with_context(|| format!("cannot read {ko} (scripts/droidvm/fetch-crosvm.sh DIR hackintosh)"))?;
    let sha = format!("{:x}", Sha256::digest(&data));
    if sha != SHA256 {
        bail!("{ko}: sha256 {sha}, not Droid-VM's prebuilt ({SHA256})");
    }
    adb.need_root()?;
    adb.refuse_in_session()?;
    if adb.root_ok(&format!("[ -d {PARAMS} ] && echo loaded || echo absent\n"), "the module check")?.trim() == "loaded" {
        bail!("gunyah_halt_poll_gki_6.6 is loaded already: `phone haltpoll status`");
    }
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the module's folder")?;
    adb.push(ko, KO)?;
    println!("load: {} bytes, sha256 {sha}; insmod {KO}", data.len());
    let script = format!(
        "got=$(sha256sum {KO} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"sha256 $got, not {sha}\"; exit 1; }}\n\
         chown root:root {KO} && chmod 644 {KO} && chown root:root {DIR} && chmod 755 {DIR}\n\
         sync\ninsmod {KO}; echo \"insmod exit $?\"\necho \"halt_poll_ns: $(cat {PARAMS}/halt_poll_ns 2>/dev/null)\"\n\
         dmesg 2>/dev/null | grep -i 'halt_poll' | tail -n 8\n"
    );
    let r = adb.root(&script)?;
    print!("{}{}", r.out, r.err);
    fs::write(Path::new(out).join("load.txt"), format!("{}{}", r.out, r.err))?;
    if !r.out.contains("insmod exit 0") {
        bail!("the module did not load");
    }
    Ok(())
}

/// `phone haltpoll unload`.
pub fn unload(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    let r = adb.root(&format!("[ -d {PARAMS} ] || {{ echo 'not loaded'; exit 0; }}\nrmmod gunyah_halt_poll_gki_6.6; echo \"rmmod exit $?\"\n"))?;
    print!("{}{}", r.out, r.err);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_status_reads_the_named_parameters() {
        let s = status_script();
        assert!(s.contains("/sys/module/gunyah_halt_poll_gki_6.6/parameters"));
        assert!(s.contains("for p in halt_poll_ns halt_poll_grow halt_poll_grow_start halt_poll_shrink; do"));
    }
}
