//! `oneios-tool phone vm-boot --image FILE [--seconds N] [--thp] [--out DIR]` and `phone vm-boot --stop`: the native
//! kernel as runtime/loader's xnuloader-crosvm made it (an arm64 Linux Image) booted in a protected VM of AVF's crosvm
//! under the phone's Gunyah, its console (the ns16550a at 0x3f8, kernel patch 0077) followed live (docs/native.md "A VM
//! under Gunyah"). The VM's RAM and vCPUs come from the Image's own label (`oneios-crosvm ram-mib=N cpus=N` at 0x100):
//! the window was cut for them. Refused during a session and while a VM of ours is up; the RAM checked as vm-probe
//! checks it (MemAvailable plus gh_hugepage_reserve's pool, 1 GiB left to Android). A VM above ~0.5 GB needs the pool
//! (`phone hugepool`) and `--thp` (vm-probe's: shmem_enabled advise and defrag madvise for the run, written back).
//!
//! The run: the Image pushed to /data/local/tmp/oneios-vmboot/kernel.img, its SHA-256 checked on the phone, then a
//! script started there detached runs crosvm (vm-probe's flags, N vCPUs, serial to `serial.txt`), stops it through its
//! control socket after SECONDS or at `stop.request` (`vm-boot --stop`), kills it only when that does not end it, and
//! writes `result`. The tool prints `serial.txt` as it grows, then keeps `serial.txt`, `crosvm.log`, `dmesg.txt`,
//! `run.txt` and `fdt.dtb` in DIR.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{copy, Write};
use std::path::Path;
use std::time::{Duration, Instant};

pub const DIR: &str = "/data/local/tmp/oneios-vmboot";

/// The label xnuloader-crosvm writes at 0x100: (ram MiB, vCPUs).
pub fn label(image: &[u8]) -> Option<(u32, u32)> {
    if image.len() < 0x200 || &image[0x38..0x3c] != b"ARM\x64" {
        return None;
    }
    let end = image[0x100..0x200].iter().position(|b| *b == 0)?;
    let text = std::str::from_utf8(&image[0x100..0x100 + end]).ok()?;
    let rest = text.strip_prefix("oneios-crosvm ")?;
    let mut ram = None;
    let mut cpus = None;
    for word in rest.split_whitespace() {
        if let Some(v) = word.strip_prefix("ram-mib=") {
            ram = v.parse().ok();
        } else if let Some(v) = word.strip_prefix("cpus=") {
            cpus = v.parse().ok();
        }
    }
    Some((ram?, cpus?))
}

/// The run on the phone (detached; its own log is `run.txt`).
pub fn boot_script(mem: u32, cpus: u32, seconds: u32, thp: bool, sha: &str) -> String {
    format!(
        r#"D={DIR}
C=/apex/com.android.virt/bin/crosvm
exec >>$D/run.txt 2>&1 </dev/null
cd $D || exit 1
end() {{ echo "$*" > $D/result; }}
got=$(sha256sum kernel.img | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; end "refused sha256"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; end "refused no /dev/gunyah"; exit 1; }}
rm -f serial.txt crosvm.log crosvm.sock fdt.dtb dmesg.txt stop.request
ulimit -l unlimited 2>/dev/null || echo "ulimit -l unlimited: refused"
T=/sys/kernel/mm/transparent_hugepage
thp_done=1
thp_restore() {{
  [ "$thp_done" = 0 ] || return 0
  thp_done=1
  echo "$old_shmem" > $T/shmem_enabled
  echo "$old_defrag" > $T/defrag
  echo "thp restored: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
}}
if [ {thp} = 1 ]; then
  old_shmem=$(tr ' ' '\n' < $T/shmem_enabled | grep '^\[' | tr -d '\133\135')
  old_defrag=$(tr ' ' '\n' < $T/defrag | grep '^\[' | tr -d '\133\135')
  case "$old_shmem" in always|within_size|advise|never|deny|force) ;; *) echo "thp: shmem_enabled '$old_shmem'?"; end "refused thp"; exit 1 ;; esac
  case "$old_defrag" in always|defer|defer+madvise|madvise|never) ;; *) echo "thp: defrag '$old_defrag'?"; end "refused thp"; exit 1 ;; esac
  thp_done=0
  trap thp_restore EXIT HUP INT TERM
  if ! {{ echo advise > $T/shmem_enabled && echo madvise > $T/defrag; }}; then
    echo "thp: the write was refused"; thp_restore; end "refused thp"; exit 1
  fi
  echo "thp set: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
fi
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/before /'
echo "start $(date +%s) up $(cut -d' ' -f1 /proc/uptime)"
$C --log-level debug run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  --protected-vm-without-firmware --swiotlb 8 --hugepages --mem size={mem} --cpus num-cores={cpus} \
  --socket $D/crosvm.sock --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console \
  --dump-device-tree-blob $D/fdt.dtb $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &
pid=$!
t=0
while [ $t -lt {seconds} ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
how=itself
if kill -0 $pid 2>/dev/null; then
  how=stop; $C stop $D/crosvm.sock </dev/null >/dev/null 2>&1
  t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
fi
if kill -0 $pid 2>/dev/null; then how=kill; kill -9 $pid; fi
wait $pid; rc=$?
thp_restore
echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
dmesg 2>/dev/null | grep -i -E 'gunyah|folio|gh_|crosvm|scm|memextent' | tail -n 80 > $D/dmesg.txt
end "end how $how exit $rc"
"#,
        thp = if thp { 1 } else { 0 }
    )
}

pub struct Options {
    pub seconds: u32,
    pub thp: bool,
}

/// `phone vm-boot --stop`: ask the running VM's script to stop it now (crosvm stop through its socket).
pub fn stop(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    adb.root_ok(&format!("[ -d {DIR} ] && touch {DIR}/stop.request; echo asked\n"), "the stop")?;
    println!("vm-boot: asked the VM's script to stop it (it uses crosvm stop)");
    Ok(())
}

/// `phone vm-boot`.
pub fn vm_boot(adb: &Adb, image: &str, out: &str, o: &Options) -> Result<()> {
    let data = fs::read(image).with_context(|| format!("cannot read {image} (runtime/loader's xnuloader-crosvm makes it)"))?;
    let (mem, cpus) = label(&data).context("no `oneios-crosvm ram-mib=N cpus=N` label at 0x100: not an Image of xnuloader-crosvm")?;
    if !(5..=3600).contains(&o.seconds) || !(1..=8).contains(&cpus) {
        bail!("--seconds 5..3600; the Image's vCPUs 1..8");
    }
    if mem > 480 && !o.thp {
        bail!("a VM of {mem} MiB needs --thp and gh_hugepage_reserve's pool (Gunyah's own memory ends near 0.5 GB of 4 KiB pages)");
    }
    let sha = format!("{:x}", Sha256::digest(&data));
    adb.need_root()?;
    adb.refuse_in_session()?;
    let up = adb.root_ok("ps -A -o PID=,ARGS= | grep -F 'oneios-vm' | grep -F crosvm | grep -v grep || true\n", "the VM check")?;
    if !up.trim().is_empty() {
        bail!("a VM of ours is still up on the phone: {}", up.trim());
    }
    let meminfo = adb.root_ok(
        "grep '^MemAvailable:' /proc/meminfo; grep '^pool_avail=' /sys/module/gh_hugepage_reserve/parameters/refill_stat 2>/dev/null\n",
        "the memory check",
    )?;
    let available = crate::vmprobe::mem_available_mib(&meminfo).with_context(|| format!("no MemAvailable in \"{}\"", meminfo.trim()))?;
    let pool_mib = crate::hugepool::stat(&meminfo, "pool_avail").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0) * 2;
    crate::vmprobe::check_mem(mem, available + pool_mib.min(mem as u64))?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/result {DIR}/run.txt {DIR}/serial.txt\n"), "the VM's folder")?;
    println!("vm-boot: {} MiB Image, sha256 {sha}: pushing", data.len() >> 20);
    adb.push(image, &format!("{DIR}/kernel.img"))?;
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let script = Path::new(out).join("boot.sh");
    fs::write(&script, boot_script(mem, cpus, o.seconds, o.thp, &sha))?;
    adb.push(script.to_str().context("path")?, &format!("{DIR}/boot.sh"))?;
    let r = adb.root(&format!(
        "chmod 755 {DIR}/boot.sh; (command -v setsid >/dev/null && setsid /system/bin/sh {DIR}/boot.sh || /system/bin/sh {DIR}/boot.sh) </dev/null >/dev/null 2>&1 &\necho started\n"
    ))?;
    if !r.out.contains("started") {
        bail!("the VM's script did not start (status {}): {}", r.status, r.err.trim());
    }
    println!("vm-boot: crosvm runs it: {mem} MiB, {cpus} vCPU(s), {} s at most (`phone vm-boot --stop` ends it sooner); its console:", o.seconds);
    let t0 = Instant::now();
    let mut shown = 0u64;
    let result = loop {
        std::thread::sleep(Duration::from_secs(2));
        if t0.elapsed() > Duration::from_secs(o.seconds as u64 + 120) {
            bail!("no end after {} s: read {DIR}/run.txt", t0.elapsed().as_secs());
        }
        let r = match adb.root(&format!(
            "tail -c +{} {DIR}/serial.txt 2>/dev/null | head -c 262144; echo; echo '=== oneios result'; cat {DIR}/result 2>/dev/null\n",
            shown + 1
        )) {
            Ok(r) if r.ok() => r,
            _ => continue,
        };
        let (new, res) = r.out.rsplit_once("\n=== oneios result\n").unwrap_or((r.out.as_str(), ""));
        // the trailing newline the script adds after `head`
        let new = new.strip_suffix('\n').unwrap_or(new);
        if !new.is_empty() {
            print!("{new}");
            std::io::stdout().flush().ok();
            shown += new.len() as u64;
        }
        if !res.trim().is_empty() {
            break res.trim().to_string();
        }
    };
    println!();
    for name in ["serial.txt", "crosvm.log", "dmesg.txt", "run.txt", "fdt.dtb"] {
        let path = Path::new(out).join(name);
        let mut child = adb.exec_out(&format!("su -c 'cat {DIR}/{name}'"))?;
        let mut file = fs::File::create(&path)?;
        copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
        child.wait()?;
    }
    let log = fs::read_to_string(Path::new(out).join("crosvm.log")).unwrap_or_default();
    let lines: Vec<&str> = log.lines().collect();
    println!("--- crosvm.log, last 15 of {} lines", lines.len());
    for l in &lines[lines.len().saturating_sub(15)..] {
        println!("{l}");
    }
    println!("vm-boot: {result} (files in {out})");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_label_gives_the_vm() {
        let mut image = vec![0u8; 0x400];
        image[0x38..0x3c].copy_from_slice(b"ARM\x64");
        let text = b"oneios-crosvm ram-mib=2048 cpus=1";
        image[0x100..0x100 + text.len()].copy_from_slice(text);
        assert_eq!(label(&image), Some((2048, 1)));
        image[0x100] = b'x';
        assert_eq!(label(&image), None);
        assert_eq!(label(&image[..0x80]), None);
    }

    #[test]
    fn the_boot_stops_before_it_kills_and_writes_back_thp() {
        let s = boot_script(2048, 1, 120, true, "ab12");
        assert!(s.find("$C stop $D/crosvm.sock").unwrap() < s.find("kill -9 $pid").unwrap());
        assert!(s.contains("--mem size=2048 --cpus num-cores=1") && s.contains("$D/kernel.img </dev/null"));
        assert!(s.contains("[ ! -e $D/stop.request ]"));
        let wait = s.find("wait $pid; rc=$?").unwrap();
        assert!(s[wait..].starts_with("wait $pid; rc=$?\nthp_restore\n"));
        assert!(s.contains("if [ 1 = 1 ]; then"));
        assert!(boot_script(256, 1, 30, false, "x").contains("if [ 0 = 1 ]; then"));
        assert!(!s.contains("/sys/power"));
    }
}
