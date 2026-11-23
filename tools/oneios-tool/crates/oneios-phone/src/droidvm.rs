//! Droid-VM's own crosvm on the phone (scripts/droidvm/fetch-crosvm.sh: their prebuilt, BSD-3-Clause with Droid-VM's
//! GPL-2.0-or-later additions; the owner chose it over a build from source, 2026-11-23). Its package, crosvm and the
//! three libraries of it the phone has not, goes to `/data/local/tmp/oneios-droidvm`, each file's SHA-256 checked
//! there; crosvm finds those libraries next to itself (RUNPATH `$ORIGIN`) and the rest (libbase, libcutils,
//! libminijail, libcap, libbinder_ndk, ...) in /system/lib64. It runs the pseudo-unprotected VM
//! (`--protected-vm-pseudo-unprotected`: `vm-probe --shared-ram`), which needs gunyah_host_share (`phone hostshare`).
//! `oneios-tool phone droidvm-crosvm [--dir DIR] [--grep REGEX]`: the push, then the lines of `crosvm run --help` that
//! match REGEX.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;

/// The package's folder on the phone, and its files.
pub const DIR: &str = "/data/local/tmp/oneios-droidvm";
pub const FILES: [&str; 4] = ["crosvm", "libvirglrenderer.so", "libgfxstream_backend.so", "libvncserver.so"];

/// The package's files in DIR: (name, SHA-256, bytes), in FILES' order.
pub fn package(dir: &str) -> Result<Vec<(String, String, usize)>> {
    FILES
        .iter()
        .map(|name| {
            let path = format!("{dir}/{name}");
            let data = fs::read(&path).with_context(|| format!("cannot read {path} (scripts/droidvm/fetch-crosvm.sh)"))?;
            Ok((name.to_string(), format!("{:x}", Sha256::digest(&data)), data.len()))
        })
        .collect()
}

/// The names whose SHA-256 on the phone (`sha256sum` lines) is not the package's: what to push.
pub fn to_push<'a>(files: &'a [(String, String, usize)], sums: &str) -> Vec<&'a (String, String, usize)> {
    files
        .iter()
        .filter(|(name, sha, _)| {
            !sums.lines().any(|l| {
                let mut w = l.split_whitespace();
                w.next() == Some(sha.as_str()) && w.next() == Some(name.as_str())
            })
        })
        .collect()
}

/// The phone's check of the package after the push: every file's SHA-256, crosvm executable.
pub fn check_script(files: &[(String, String, usize)]) -> String {
    let mut s = format!("cd {DIR} || exit 1\n");
    for (name, sha, _) in files {
        s += &format!("got=$(sha256sum {name} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"{name}: sha256 $got, not {sha}\"; exit 1; }}\n");
    }
    s += "chown root:root crosvm *.so && chmod 755 crosvm && chmod 644 *.so && echo package ok\n";
    s
}

/// The package on the phone, pushing only what differs; crosvm's path there.
pub fn push(adb: &Adb, dir: &str) -> Result<String> {
    let files = package(dir)?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the package's folder")?;
    let sums = adb.root_ok(&format!("cd {DIR} && sha256sum {} 2>/dev/null; true\n", FILES.join(" ")), "the package's sums")?;
    for (name, _, len) in to_push(&files, &sums) {
        println!("droidvm: {name} ({len} bytes) to {DIR}");
        // a root-owned file of an earlier push: adb's shell cannot overwrite it
        adb.root_ok(&format!("rm -f {DIR}/{name}\n"), "the old file")?;
        adb.push(&format!("{dir}/{name}"), &format!("{DIR}/{name}"))?;
    }
    let r = adb.root(&check_script(&files))?;
    if !r.out.contains("package ok") {
        bail!("Droid-VM's package on the phone: {}{}", r.out.trim(), r.err.trim());
    }
    Ok(format!("{DIR}/crosvm"))
}

/// `phone droidvm-crosvm`.
pub fn crosvm(adb: &Adb, dir: &str, grep: &str) -> Result<()> {
    if grep.contains('\'') {
        bail!("--grep: no single quotes");
    }
    adb.need_root()?;
    let c = push(adb, dir)?;
    let r = adb.root(&format!("{c} run --help > {DIR}/help.txt 2>&1; echo \"crosvm exit $?\"; grep -i -E -A3 '{grep}' {DIR}/help.txt\n"))?;
    print!("{}{}", r.out, r.err);
    if r.out.contains("CANNOT LINK") || r.out.contains("not found") {
        bail!("crosvm does not start on the phone");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_what_differs_is_pushed() {
        let files = vec![("crosvm".to_string(), "aa".to_string(), 3), ("libvncserver.so".to_string(), "bb".to_string(), 2)];
        assert_eq!(to_push(&files, "").len(), 2);
        assert_eq!(to_push(&files, "aa  crosvm\ncc  libvncserver.so\n").iter().map(|f| f.0.as_str()).collect::<Vec<_>>(), ["libvncserver.so"]);
        assert!(to_push(&files, "aa  crosvm\nbb  libvncserver.so\n").is_empty());
        let s = check_script(&files);
        assert!(s.contains("got=$(sha256sum crosvm | cut -d' ' -f1); [ \"$got\" = \"aa\" ]"));
        assert!(s.ends_with("echo package ok\n"));
    }
}
