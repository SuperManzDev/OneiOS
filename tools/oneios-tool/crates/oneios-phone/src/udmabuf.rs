//! `oneios-tool phone udmabuf status | load | unload`: Droid-VM's udmabuf module (GPL-2.0-or-later, built by
//! scripts/hugepage/build-udmabuf.sh) on the phone. GKI 6.6's own udmabuf keeps a buffer's page array in one
//! kmalloc_array, which a fragmented phone cannot give for a large buffer: crosvm's virtio-gpu then failed the guest's
//! large GPU blobs ("GUEST-ALLOC: create_udmabuf failed ... Out of memory", vm-boot gpu6) and the screen stayed black.
//! The module picks its mode at insmod and says which: "override" redirects the built-in udmabuf_create (a kprobe) to a
//! copy that uses kvmalloc, "paramonly" only raises the built-in's limits, "native" registers /dev/udmabuf where there
//! is none (docs/native.md "The GPU in the shared-RAM VM").
//! - `status`: its parameters (named files), read only.
//! - `load`: refused during a session or when it is loaded; the built-in's symbols checked in /proc/kallsyms (a named
//!   file), the .ko pushed, its SHA-256 checked, a `sync` and `insmod`, then its mode.
//! - `unload`: rmmod (the kernel refuses while a buffer of the module's lives).
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

/// The module's folder on the phone, the .ko there and its parameters (the module is udmabuf_gki_6.6).
pub const DIR: &str = "/data/local/tmp/oneios-udmabuf";
pub const KO: &str = "/data/local/tmp/oneios-udmabuf/udmabuf_gki.ko";
pub const PARAMS: &str = "/sys/module/udmabuf_gki_6.6/parameters";

/// `status`'s read on the phone.
pub fn status_script() -> String {
    format!(
        "if [ -d {PARAMS} ]; then echo '=== loaded'; for p in mode list_limit size_limit_mb stat_created stat_failed; do echo \"$p: $(cat {PARAMS}/$p)\"; done\n\
         else echo '=== not loaded'; fi\n\
         echo \"built-in: list_limit $(cat /sys/module/udmabuf/parameters/list_limit 2>/dev/null) size_limit_mb $(cat /sys/module/udmabuf/parameters/size_limit_mb 2>/dev/null)\"\n\
         echo '=== kernel lines'\ndmesg 2>/dev/null | grep -i 'udmabuf' | tail -n 12\n"
    )
}

/// `phone udmabuf status`.
pub fn status(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    print!("{}", adb.root_ok(&status_script(), "the module's status")?);
    Ok(())
}

/// The built-in's symbols the module hooks, in /proc/kallsyms's lines (` t udmabuf_create`): both or nothing to override.
pub fn has_builtin(kallsyms: &str) -> (bool, bool) {
    let has = |name: &str| kallsyms.lines().any(|l| l.split_whitespace().nth(2) == Some(name));
    (has("udmabuf_create"), has("udmabuf_ioctl"))
}

/// `phone udmabuf load`.
pub fn load(adb: &Adb, ko: &str, out: &str) -> Result<()> {
    let data = fs::read(ko).with_context(|| format!("cannot read {ko} (scripts/hugepage/build-udmabuf.sh makes it)"))?;
    let sha = format!("{:x}", Sha256::digest(&data));
    adb.need_root()?;
    adb.refuse_in_session()?;
    let r = adb.root_ok(
        &format!(
            "[ -d {PARAMS} ] && echo loaded || echo absent\n\
             grep -E ' [Tt] (udmabuf_create|udmabuf_ioctl)$' /proc/kallsyms | while read -r a t n; do echo \"x $t $n\"; done\n"
        ),
        "the module check",
    )?;
    if r.lines().next() == Some("loaded") {
        bail!("udmabuf_gki_6.6 is loaded already: `phone udmabuf status`");
    }
    let (create, ioctl) = has_builtin(&r);
    println!("preflight: the built-in's udmabuf_create {}, udmabuf_ioctl {}", if create { "found" } else { "absent" }, if ioctl { "found" } else { "absent" });
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the module's folder")?;
    adb.push(ko, KO)?;
    println!("load: {} bytes, sha256 {sha}; insmod {KO}", data.len());
    let script = format!(
        "got=$(sha256sum {KO} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"sha256 $got, not {sha}\"; exit 1; }}\n\
         sync\ninsmod {KO}; echo \"insmod exit $?\"\necho \"mode: $(cat {PARAMS}/mode 2>/dev/null)\"\n\
         dmesg 2>/dev/null | grep -i 'udmabuf' | tail -n 8\n"
    );
    let r = adb.root(&script)?;
    print!("{}{}", r.out, r.err);
    fs::write(Path::new(out).join("load.txt"), format!("{}{}", r.out, r.err))?;
    if !r.out.contains("insmod exit 0") {
        bail!("the module did not load");
    }
    Ok(())
}

/// `phone udmabuf unload`.
pub fn unload(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    let r = adb.root(&format!("[ -d {PARAMS} ] || {{ echo 'not loaded'; exit 0; }}\nrmmod udmabuf_gki_6.6; echo \"rmmod exit $?\"\n"))?;
    print!("{}{}", r.out, r.err);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_builtins_symbols_are_read_by_name() {
        assert_eq!(has_builtin("x t udmabuf_create\nx t udmabuf_ioctl\n"), (true, true));
        assert_eq!(has_builtin("x t udmabuf_create_list\n"), (false, false));
        assert_eq!(has_builtin(""), (false, false));
    }
}
