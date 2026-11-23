//! `oneios-tool phone vm-boot --image FILE [--seconds N] [--thp] [--out DIR]` and `phone vm-boot --stop`: the native
//! kernel as runtime/loader's xnuloader-crosvm made it (an arm64 Linux Image) booted in a protected VM of AVF's crosvm
//! under the phone's Gunyah, its console (the ns16550a at 0x3f8, kernel patch 0077) followed live (docs/native.md "A VM
//! under Gunyah"). The VM's RAM, vCPUs and restricted DMA pool come from the Image's own label (`oneios-crosvm ram-mib=N
//! cpus=N swiotlb-mib=N` at 0x100; an older label without swiotlb-mib means 8): the window was cut for them. Refused
//! during a session and while a VM of ours is up; the RAM checked as vm-probe checks it (MemAvailable plus
//! gh_hugepage_reserve's pool, 1 GiB left to Android). A VM above ~0.5 GB needs the pool (`phone hugepool`) and `--thp`
//! (vm-probe's: shmem_enabled advise and defrag madvise for the run, written back).
//!
//! The image set's disks: FILE.disks beside the Image (xnuloader-crosvm --image) names them, crosvm's `--block` values
//! with the PCI address each is pinned to. Before crosvm starts, the script checks each read-only file's size on the
//! phone against the manifest's, makes the VM's own Data store (sparse, at the manifest's size) when it is missing, and
//! wants 4 GiB free on /data. A protected VM's host can touch only crosvm's restricted DMA pool, where the kernel's disk
//! driver keeps every buffer: the moment crosvm has written its tree (`fdt.dtb`, before the kernel runs more than a few
//! instructions; its disks start seconds later), the script checks that the pool is where the Image's windows are
//! (`restricted_dma_reserved@ADDR`) and stops the VM if not.
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
/// crosvm's DRAM on aarch64 (xnuloader-crosvm RAM_BASE).
const RAM_BASE: u64 = 0x8000_0000;
/// What /data must have free before a VM with a Data store starts: its first start writes the seed's ~2 GB into it.
const DATA_FREE_KIB: u64 = 4 << 20;

/// The label xnuloader-crosvm writes at 0x100.
#[derive(Debug, PartialEq)]
pub struct Label {
    pub ram: u32,
    pub cpus: u32,
    pub swiotlb: u32,
}

pub fn label(image: &[u8]) -> Option<Label> {
    if image.len() < 0x200 || &image[0x38..0x3c] != b"ARM\x64" {
        return None;
    }
    let end = image[0x100..0x200].iter().position(|b| *b == 0)?;
    let text = std::str::from_utf8(&image[0x100..0x100 + end]).ok()?;
    let rest = text.strip_prefix("oneios-crosvm ")?;
    let (mut ram, mut cpus, mut swiotlb) = (None, None, Some(8));
    for word in rest.split_whitespace() {
        if let Some(v) = word.strip_prefix("ram-mib=") {
            ram = v.parse().ok();
        } else if let Some(v) = word.strip_prefix("cpus=") {
            cpus = v.parse().ok();
        } else if let Some(v) = word.strip_prefix("swiotlb-mib=") {
            swiotlb = v.parse().ok();
        }
    }
    Some(Label { ram: ram?, cpus: cpus?, swiotlb: swiotlb? })
}

/// FILE.disks: the pool crosvm's tree must name, the Data store and crosvm's `--block` values.
#[derive(Debug, PartialEq)]
pub struct Disks {
    pub swiotlb: u32,
    pub pool: u64,
    pub store: String,
    pub store_gib: u32,
    /// The size the phone's file must have (None: the writable store) and crosvm's --block value.
    pub blocks: Vec<(Option<u64>, String)>,
}

/// A value the phone's shell and crosvm's key=value parser take as it is: no blank, quote or shell character.
fn plain(text: &str) -> bool {
    !text.is_empty() && text.chars().all(|c| c.is_ascii_alphanumeric() || "/._-=:,".contains(c))
}

pub fn parse_disks(text: &str) -> Result<Disks> {
    let (mut swiotlb, mut pool, mut store) = (None, None, None);
    let mut blocks = Vec::new();
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let mut words = line.split_whitespace();
        match (words.next(), words.next(), words.next(), words.next()) {
            (Some("swiotlb-mib"), Some(v), None, None) => swiotlb = Some(v.parse::<u32>().context("swiotlb-mib")?),
            (Some("pool"), Some(v), None, None) => {
                pool = Some(u64::from_str_radix(v.strip_prefix("0x").context("pool 0x...")?, 16).context("pool")?)
            }
            (Some("store"), Some(path), Some(gib), None) => store = Some((path.to_string(), gib.parse::<u32>().context("store GIB")?)),
            (Some("block"), Some(size), Some(value), None) => {
                let size = if size == "-" { None } else { Some(size.parse::<u64>().context("block BYTES")?) };
                blocks.push((size, value.to_string()));
            }
            _ => bail!("a line of the disks list that is not one of swiotlb-mib, pool, store, block: {line:?}"),
        }
    }
    let (store, store_gib) = store.context("no store line")?;
    let d = Disks { swiotlb: swiotlb.context("no swiotlb-mib line")?, pool: pool.context("no pool line")?, store, store_gib, blocks };
    if d.blocks.is_empty() || d.blocks.len() > 5 || !(1..=256).contains(&d.store_gib) {
        bail!("1 to 5 disks and a store of 1 to 256 GiB");
    }
    if !d.store.starts_with("/data/") || !plain(&d.store) || d.store.contains(',') {
        bail!("the store {:?} is not a plain path under /data", d.store);
    }
    let mut writable = 0;
    for (size, value) in &d.blocks {
        let path = value.strip_prefix("path=").and_then(|v| v.split(',').next()).unwrap_or("");
        if !plain(value) || !path.starts_with("/data/") || !value.contains(",pci-address=00:") {
            bail!("the --block value {value:?} is not a plain path under /data with a pinned PCI address");
        }
        match size {
            None if path == d.store && value.contains(",ro=false") => writable += 1,
            Some(_) if value.contains(",ro=true") => {}
            _ => bail!("{value:?}: the store is the one writable disk, every other disk read-only with its size"),
        }
    }
    if writable != 1 {
        bail!("the disks list names the store {:?} {writable} times", d.store);
    }
    Ok(d)
}

/// The run on the phone (detached; its own log is `run.txt`).
pub fn boot_script(l: &Label, seconds: u32, thp: bool, sha: &str, disks: Option<&Disks>) -> String {
    let (mem, cpus, swiotlb) = (l.ram, l.cpus, l.swiotlb);
    let mut checks = String::new();
    let mut blocks = String::new();
    let mut pool_check = String::new();
    if let Some(d) = disks {
        for (size, value) in &d.blocks {
            blocks += &format!(" \\\n  --block {value}");
            let path = value.strip_prefix("path=").and_then(|v| v.split(',').next()).unwrap_or("");
            if let Some(size) = size {
                checks += &format!(
                    "s=$(stat -c %s {path} 2>/dev/null); [ \"$s\" = {size} ] || {{ echo \"disk {path}: '$s' bytes, not {size}\"; end \"refused disk\"; exit 1; }}\n"
                );
            }
        }
        let store = &d.store;
        let parent = store.rsplit_once('/').map(|(p, _)| p).unwrap_or("/data");
        checks += &format!(
            "if [ ! -f {store} ]; then\n  mkdir -p {parent} && truncate -s {gib}G {store} || {{ echo \"store: cannot make {store}\"; end \"refused store\"; exit 1; }}\n  \
             echo \"store: a new {store} ({gib} GiB, sparse): the kernel formats it and unpacks the seed\"\nfi\n\
             avail=$(df -k /data | awk 'NR==2 {{print $4}}')\n\
             [ \"${{avail:-0}}\" -ge {DATA_FREE_KIB} ] || {{ echo \"/data: '$avail' KiB free, {DATA_FREE_KIB} needed\"; end \"refused space\"; exit 1; }}\n\
             echo \"disks: {n}, the store {store} ($(du -k {store} | cut -f1) KiB used); /data $avail KiB free\"\n",
            gib = d.store_gib,
            n = d.blocks.len()
        );
        // The pool's node, as crosvm names it (aarch64/src/fdt.rs: the address in lowercase hex, no 0x)
        pool_check = format!(
            "t=0\nwhile [ $t -lt 100 ] && [ ! -s $D/fdt.dtb ] && kill -0 $pid 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done\n\
             sleep 0.1 2>/dev/null\n\
             if kill -0 $pid 2>/dev/null; then\n  \
             if grep -q -F restricted_dma_reserved@{pool:x} $D/fdt.dtb; then echo \"pool: crosvm's restricted DMA pool is at 0x{pool:x}, as the disks' windows\"; \
             else echo \"pool: crosvm's tree has no restricted_dma_reserved@{pool:x}: the VM is stopped before the kernel's disks start\"; pool=bad; touch $D/stop.request; fi\n\
             fi\n",
            pool = d.pool
        );
    }
    format!(
        r#"D={DIR}
C=/apex/com.android.virt/bin/crosvm
exec >>$D/run.txt 2>&1 </dev/null
cd $D || exit 1
end() {{ echo "$*" > $D/result; }}
got=$(sha256sum kernel.img | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; end "refused sha256"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; end "refused no /dev/gunyah"; exit 1; }}
{checks}rm -f serial.txt crosvm.log crosvm.sock fdt.dtb dmesg.txt stop.request
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
pool=
$C --log-level debug run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  --protected-vm-without-firmware --swiotlb {swiotlb} --hugepages --mem size={mem} --cpus num-cores={cpus} \
  --socket $D/crosvm.sock --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console \
  --dump-device-tree-blob $D/fdt.dtb{blocks} $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &
pid=$!
{pool_check}t=0
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
if [ "$pool" = bad ]; then end "refused pool: end how $how exit $rc"; else end "end how $how exit $rc"; fi
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
    let l = label(&data).context("no `oneios-crosvm ram-mib=N cpus=N` label at 0x100: not an Image of xnuloader-crosvm")?;
    if !(5..=3600).contains(&o.seconds) || !(1..=8).contains(&l.cpus) || !(2..=64).contains(&l.swiotlb) {
        bail!("--seconds 5..3600; the Image's vCPUs 1..8, its pool 2..64 MiB");
    }
    if l.ram > 480 && !o.thp {
        bail!("a VM of {} MiB needs --thp and gh_hugepage_reserve's pool (Gunyah's own memory ends near 0.5 GB of 4 KiB pages)", l.ram);
    }
    let list = format!("{image}.disks");
    let disks = match fs::read_to_string(&list) {
        Ok(text) => Some(parse_disks(&text).with_context(|| format!("{list}"))?),
        Err(_) => None,
    };
    if let Some(d) = &disks {
        let pool = RAM_BASE + ((l.ram as u64) << 20) - ((l.swiotlb as u64) << 20);
        if d.swiotlb != l.swiotlb || d.pool != pool {
            bail!("{list} names a pool of {} MiB at 0x{:x}, the Image's label {} MiB at 0x{pool:x}: make both again", d.swiotlb, d.pool, l.swiotlb);
        }
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
    crate::vmprobe::check_mem(l.ram, available + pool_mib.min(l.ram as u64))?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/result {DIR}/run.txt {DIR}/serial.txt\n"), "the VM's folder")?;
    println!("vm-boot: {} MiB Image, sha256 {sha}: pushing", data.len() >> 20);
    adb.push(image, &format!("{DIR}/kernel.img"))?;
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let script = Path::new(out).join("boot.sh");
    fs::write(&script, boot_script(&l, o.seconds, o.thp, &sha, disks.as_ref()))?;
    adb.push(script.to_str().context("path")?, &format!("{DIR}/boot.sh"))?;
    let r = adb.root(&crate::adb::detached_start(&format!("{DIR}/boot.sh")))?;
    if !r.out.contains("started") {
        bail!("the VM's script did not start (status {}): {}", r.status, r.err.trim());
    }
    match &disks {
        Some(d) => println!(
            "vm-boot: crosvm runs it: {} MiB, {} vCPU(s), {} disk(s) (the store {}), {} s at most (`phone vm-boot --stop` ends it sooner); its console:",
            l.ram, l.cpus, d.blocks.len(), d.store, o.seconds
        ),
        None => println!(
            "vm-boot: crosvm runs it: {} MiB, {} vCPU(s), no disk, {} s at most (`phone vm-boot --stop` ends it sooner); its console:",
            l.ram, l.cpus, o.seconds
        ),
    }
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
    let run = fs::read_to_string(Path::new(out).join("run.txt")).unwrap_or_default();
    for line in run.lines().filter(|l| l.starts_with("disk") || l.starts_with("store:") || l.starts_with("pool:") || l.starts_with("/data:")) {
        println!("vm-boot: {line}");
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

    const DISKS: &str = "swiotlb-mib 8\npool 0x13f800000\nstore /data/local/tmp/oneios-vmboot/data.img 16\n\
        block 10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true,id=os,pci-address=00:02.0\n\
        block 1267978240 path=/data/local/tmp/oneios/image/data.seed,ro=true,id=seed,pci-address=00:05.0\n\
        block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0\n";

    fn image(text: &[u8]) -> Vec<u8> {
        let mut image = vec![0u8; 0x400];
        image[0x38..0x3c].copy_from_slice(b"ARM\x64");
        image[0x100..0x100 + text.len()].copy_from_slice(text);
        image
    }

    #[test]
    fn the_label_gives_the_vm() {
        assert_eq!(label(&image(b"oneios-crosvm ram-mib=2048 cpus=1")), Some(Label { ram: 2048, cpus: 1, swiotlb: 8 }));
        assert_eq!(label(&image(b"oneios-crosvm ram-mib=3072 cpus=2 swiotlb-mib=16")), Some(Label { ram: 3072, cpus: 2, swiotlb: 16 }));
        let mut bad = image(b"oneios-crosvm ram-mib=2048 cpus=1");
        bad[0x100] = b'x';
        assert_eq!(label(&bad), None);
        assert_eq!(label(&bad[..0x80]), None);
    }

    #[test]
    fn the_disks_list_is_checked() {
        let d = parse_disks(DISKS).unwrap();
        assert_eq!((d.swiotlb, d.pool, d.store_gib, d.blocks.len()), (8, 0x13f800000, 16, 3));
        assert_eq!(d.blocks[0].0, Some(10359930880));
        assert_eq!(d.blocks[2].0, None);
        // a shell character, a store that is not the writable disk, a disk outside /data, two stores, no PCI address
        assert!(parse_disks(&DISKS.replace("os.img", "os.img;reboot")).is_err());
        assert!(parse_disks(&DISKS.replace("block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false", "block - path=/data/other.img,ro=false")).is_err());
        assert!(parse_disks(&DISKS.replace("path=/data/local/tmp/oneios/image/os.img", "path=/sdcard/os.img")).is_err());
        assert!(parse_disks(&DISKS.replace("block 1267978240 path=/data/local/tmp/oneios/image/data.seed,ro=true", "block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false")).is_err());
        assert!(parse_disks(&DISKS.replace(",pci-address=00:02.0", "")).is_err());
        assert!(parse_disks(&DISKS.replace("10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true", "10359930880 path=/data/local/tmp/oneios/image/os.img,ro=false")).is_err());
        assert!(parse_disks("swiotlb-mib 8\npool 0x13f800000\n").is_err());
    }

    #[test]
    fn the_boot_stops_before_it_kills_and_writes_back_thp() {
        let l = Label { ram: 2048, cpus: 1, swiotlb: 8 };
        let s = boot_script(&l, 120, true, "ab12", None);
        assert!(s.find("$C stop $D/crosvm.sock").unwrap() < s.find("kill -9 $pid").unwrap());
        assert!(s.contains("--swiotlb 8 --hugepages --mem size=2048 --cpus num-cores=1"));
        assert!(s.contains("$D/fdt.dtb $D/kernel.img </dev/null"));
        assert!(s.contains("[ ! -e $D/stop.request ]"));
        let wait = s.find("wait $pid; rc=$?").unwrap();
        assert!(s[wait..].starts_with("wait $pid; rc=$?\nthp_restore\n"));
        assert!(s.contains("if [ 1 = 1 ]; then"));
        assert!(boot_script(&l, 30, false, "x", None).contains("if [ 0 = 1 ]; then"));
        assert!(!s.contains("/sys/power") && !s.contains("--block") && !s.contains("restricted_dma_reserved"));
    }

    #[test]
    fn the_disks_are_checked_before_crosvm_and_the_pool_right_after() {
        let d = parse_disks(DISKS).unwrap();
        let l = Label { ram: 3072, cpus: 1, swiotlb: 8 };
        let s = boot_script(&l, 600, true, "ab12", Some(&d));
        let crosvm = s.find("$C --log-level debug run").unwrap();
        // the sizes, the store and the free space before crosvm; the store made only when missing
        let size = s.find("s=$(stat -c %s /data/local/tmp/oneios/image/os.img 2>/dev/null); [ \"$s\" = 10359930880 ]").unwrap();
        let store = s.find("if [ ! -f /data/local/tmp/oneios-vmboot/data.img ]; then").unwrap();
        let space = s.find("[ \"${avail:-0}\" -ge 4194304 ]").unwrap();
        assert!(size < crosvm && store < crosvm && space < crosvm);
        assert!(s.contains("truncate -s 16G /data/local/tmp/oneios-vmboot/data.img"));
        assert!(!s.contains("stat -c %s /data/local/tmp/oneios-vmboot/data.img"));
        // every disk on crosvm's line, pinned
        assert_eq!(s.matches("  --block path=").count(), 3);
        assert!(s.contains("--block path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0 $D/kernel.img"));
        // the pool: right after crosvm starts, before the main wait; a wrong one stops the VM
        let pool = s.find("grep -q -F restricted_dma_reserved@13f800000 $D/fdt.dtb").unwrap();
        let wait = s.find("while [ $t -lt 600 ]").unwrap();
        assert!(crosvm < pool && pool < wait);
        assert!(s.contains("pool=bad; touch $D/stop.request"));
        assert!(s.contains("end \"refused pool: end how $how exit $rc\""));
    }

    #[test]
    fn the_boot_parses_as_posix_sh() {
        let d = parse_disks(DISKS).unwrap();
        let l = Label { ram: 3072, cpus: 1, swiotlb: 8 };
        for (n, script) in [boot_script(&l, 600, true, "ab12", Some(&d)), boot_script(&l, 60, false, "ab12", None)].iter().enumerate() {
            let path = std::env::temp_dir().join(format!("oneios-vmboot-{}-{n}.sh", std::process::id()));
            fs::write(&path, script).unwrap();
            for shell in ["bash", "sh", "dash"] {
                let mut c = std::process::Command::new(shell);
                if shell == "bash" {
                    c.arg("--posix");
                }
                if let Ok(o) = c.arg("-n").arg(&path).output() {
                    assert!(o.status.success(), "{shell} -n: {}", String::from_utf8_lossy(&o.stderr));
                }
            }
            fs::remove_file(&path).ok();
        }
    }
}
