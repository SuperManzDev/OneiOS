//! `oneios-tool phone vm-probe [--payload FILE] [--mem MIB] [--timeout SECONDS] [--out DIR]`: one run of
//! runtime/vmprobe's payload as a protected VM under the phone's Gunyah, with AVF's own crosvm (docs/native.md "A VM
//! under Gunyah"): does Samsung's Gunyah run a VM made from root's shell, and what do its vCPUs say (the ID registers,
//! PSCI, a timed loop). Refused during a session and while a probe VM is up.
//!
//! The run: the payload is pushed to `/data/local/tmp/oneios-vmprobe/probe.bin` (its SHA-256 checked on the phone), then
//! root's shell starts `crosvm run` with one vCPU, MIB of RAM, no device but the 8250 (to a file) and no balloon, RNG or
//! USB, `--protected-vm-without-firmware` (the only kind the phone reports) and a small swiotlb, its stdin /dev/null.
//! The payload powers off by itself in well under a second. A VM still up after SECONDS is stopped through crosvm's
//! control socket (`crosvm stop`, never a signal first: a guest killed from the host once reset a phone, Droid-VM's
//! pacprobe notes); only a crosvm still there SECONDS after that is killed. Then the serial output and crosvm's log come
//! back into DIR (`serial.txt`, `crosvm.log`, `run.txt`). The worst case is the others' (a phone reset).
use crate::adb::Adb;
use crate::hw::split;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

/// The probe's folder on the phone.
pub const DIR: &str = "/data/local/tmp/oneios-vmprobe";
/// AVF's crosvm.
pub const CROSVM: &str = "/apex/com.android.virt/bin/crosvm";

pub struct Options {
    pub mem: u32,
    pub timeout: u32,
    /// During the run only: transparent huge pages for madvised shmem (`shmem_enabled` advise) and direct compaction
    /// for madvised regions (`defrag` madvise), so that crosvm's memfd (`--hugepages`) can come in 2 MiB folios,
    /// which Gunyah's demand paging lends whole; the old values are written back after the run.
    pub thp: bool,
    /// The crosvm to run, its path on the phone: AVF's (CROSVM) when None, else Droid-VM's (`--droidvm`, droidvm.rs)
    pub crosvm: Option<String>,
    /// Droid-VM's pseudo-unprotected VM (`--protected-vm-pseudo-unprotected`): its RAM SHAREd with it after start
    /// through gunyah_host_share, which a shim in the VM accepts before the payload runs; no swiotlb
    pub shared_ram: bool,
    /// With `thp`: what `shmem_enabled` is for the run, `advise` (crosvm's madvised mappings only) or `within_size`
    /// (every mapping of a shmem file within its size). Droid-VM's crosvm pins a shared-RAM VM's window through a
    /// second mapping of its memfd that is not madvised, and with `advise` the window came in 4 KiB pages (run
    /// shared1: 1080 runs for 122 MiB), which Gunyah's own memory cannot map for long
    pub thp_shmem: &'static str,
}

/// The values `--thp-shmem` takes.
pub const THP_SHMEM: [&str; 2] = ["advise", "within_size"];

/// crosvm's protection arguments: the shared-RAM VM, or the protected VM with a small bounce pool.
pub fn protection(o: &Options) -> &'static str {
    if o.shared_ram {
        "--protected-vm-pseudo-unprotected"
    } else {
        "--protected-vm-without-firmware --swiotlb 8"
    }
}

/// The transparent huge page settings `--thp` changes for the run and writes back after it.
pub const THP: &str = "/sys/kernel/mm/transparent_hugepage";

/// The most RAM a probe VM may ask for, and what Android must keep available beside it (MiB).
pub const MAX_MEM: u32 = 6144;
pub const KEEP_FREE: u64 = 1024;

/// MemAvailable in MiB from `/proc/meminfo`'s line ("MemAvailable:  3276800 kB").
pub fn mem_available_mib(meminfo: &str) -> Option<u64> {
    let line = meminfo.lines().find(|l| l.starts_with("MemAvailable:"))?;
    let kb: u64 = line.split_whitespace().nth(1)?.parse().ok()?;
    Some(kb / 1024)
}

/// The VM's size against what Android has: at most MAX_MEM, and KEEP_FREE left to Android.
pub fn check_mem(mem: u32, available_mib: u64) -> Result<()> {
    if mem == 0 || mem > MAX_MEM {
        bail!("--mem {mem}: a probe VM takes 1 to {MAX_MEM} MiB");
    }
    if mem as u64 + KEEP_FREE > available_mib {
        bail!(
            "--mem {mem}: Android has {available_mib} MiB available, and a VM's RAM is pinned; {KEEP_FREE} MiB must stay \
             free beside it. Close apps or ask for less. Nothing was started"
        );
    }
    Ok(())
}

/// The payload's own check: a Linux arm64 Image (magic "ARM\x64" at 0x38) of at most 1 MiB.
pub fn check_payload(data: &[u8]) -> Result<()> {
    if data.len() < 64 || &data[0x38..0x3c] != b"ARM\x64" {
        bail!("not an arm64 Image (no \"ARM\\x64\" at 0x38): build it with runtime/vmprobe/build.sh");
    }
    if data.len() > 1 << 20 {
        bail!("{} bytes: a probe is a few KiB", data.len());
    }
    Ok(())
}

/// The run on the phone, for root's shell: `=== run` (start, end, how it ended, crosvm's status), `=== serial.txt`,
/// `=== crosvm.log`.
pub fn run_script(o: &Options, sha: &str) -> String {
    let t = o.timeout.max(1);
    format!(
        r#"D={DIR}
C={crosvm}
echo '=== run'
cd $D || exit 1
got=$(sha256sum probe.bin | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; exit 1; }}
rm -f serial.txt crosvm.log crosvm.sock fdt.dtb
ulimit -l unlimited 2>/dev/null || echo "ulimit -l unlimited: refused"
T={THP}
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
  [ -n "$old_shmem" ] && [ -n "$old_defrag" ] || {{ echo "thp: cannot read $T"; exit 1; }}
  # only a value the kernel itself offers is ever written back: else nothing is written at all
  case "$old_shmem" in always|within_size|advise|never|deny|force) ;; *) echo "thp: shmem_enabled '$old_shmem'?"; exit 1 ;; esac
  case "$old_defrag" in always|defer|defer+madvise|madvise|never) ;; *) echo "thp: defrag '$old_defrag'?"; exit 1 ;; esac
  thp_done=0
  trap thp_restore EXIT HUP INT TERM
  if ! {{ echo {thp_shmem} > $T/shmem_enabled && echo madvise > $T/defrag; }}; then
    echo "thp: the write was refused"; thp_restore; exit 1
  fi
  echo "thp set: shmem_enabled $(cat $T/shmem_enabled) | defrag $(cat $T/defrag)"
  # one compaction of all zones right before the VM: free 2 MiB blocks for its folios (run 6: none were free, and
  # the 4 KiB pages it got instead filled Gunyah's own 18 MiB). A one-shot action: no setting stays changed
  sed 's/^/uncompacted /' /proc/buddyinfo
  echo 1 > /proc/sys/vm/compact_memory || echo "thp: compact_memory refused"
fi
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/before /'
sed 's/^/before /' /proc/buddyinfo
echo "start $(date +%s) up $(cut -d' ' -f1 /proc/uptime)"
$C --log-level debug run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  {protection} --hugepages --mem size={mem} --cpus num-cores=1 \
  --socket $D/crosvm.sock --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console \
  --dump-device-tree-blob $D/fdt.dtb $D/probe.bin </dev/null >$D/crosvm.log 2>&1 &
pid=$!
t=0; while [ $t -lt {t} ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
how=itself
if kill -0 $pid 2>/dev/null; then
  how=stop; $C stop $D/crosvm.sock </dev/null >/dev/null 2>&1
  t=0; while [ $t -lt {t} ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
fi
if kill -0 $pid 2>/dev/null; then how=kill; kill -9 $pid; fi
wait $pid; rc=$?
thp_restore
echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/after /'
sed 's/^/after /' /proc/buddyinfo
echo '=== serial.txt'
head -c 65536 serial.txt 2>/dev/null
echo '=== crosvm.log'
tail -c 65536 crosvm.log 2>/dev/null
echo '=== dmesg'
dmesg 2>/dev/null | grep -i -E 'gunyah|folio|gh_|crosvm|scm|memextent' | tail -n 80
"#,
        crosvm = o.crosvm.as_deref().unwrap_or(CROSVM),
        protection = protection(o),
        mem = o.mem,
        thp = if o.thp { 1 } else { 0 },
        thp_shmem = o.thp_shmem
    )
}

/// What a run says: how it ended (`itself`, `stop`, `kill`), crosvm's status, and the verdict line.
pub fn verdict(run: &str, serial: &str) -> String {
    let end = run.lines().find(|l| l.starts_with("end ")).unwrap_or("");
    let how = end.split_whitespace().skip_while(|w| *w != "how").nth(1).unwrap_or("?");
    let rc = end.split_whitespace().skip_while(|w| *w != "exit").nth(1).unwrap_or("?");
    let said = if serial.contains("ONEIOS VMPROBE done") {
        "the VM ran the whole probe"
    } else if serial.contains("ONEIOS VMPROBE start") {
        "the VM started the probe but did not finish it"
    } else {
        "no line from the VM (crosvm.log says why)"
    };
    let ended = match how {
        "itself" => "and ended by itself",
        "stop" => "and had to be stopped (crosvm stop)",
        "kill" => "and had to be killed (crosvm stop did not end it)",
        _ => "(no end line: the run script stopped early)",
    };
    format!("{said} {ended}; crosvm exit {rc}")
}

/// The free memory in blocks of 2 MiB or more, from `/proc/buddyinfo` (orders 0 to 10 of 4 KiB pages, every zone):
/// (how many 2 MiB blocks, their MiB).
pub fn huge_free(buddyinfo: &str) -> (u64, u64) {
    let mut blocks = 0u64;
    for line in buddyinfo.lines().filter(|l| l.starts_with("Node ")) {
        // "Node 0, zone   Normal  c0 c1 ... c10": the counts follow the zone's name
        let counts: Vec<u64> = line.split_whitespace().skip(4).filter_map(|w| w.parse().ok()).collect();
        for (order, n) in counts.iter().enumerate().skip(9) {
            blocks += n << (order - 9);
        }
    }
    (blocks, blocks * 2)
}

/// `phone hugeblocks`'s script: `=== before` (MemFree, MemAvailable, Cached, buddyinfo), and with `free` a sync, the
/// file cache and reclaimable slabs dropped (`drop_caches` 3) and one compaction, then `=== after` the same.
pub fn hugeblocks_script(free: bool) -> String {
    let mut s = String::from(
        "echo '=== before'\ngrep -E '^(MemFree|MemAvailable|Cached):' /proc/meminfo\ncat /proc/buddyinfo\n",
    );
    if free {
        s += "t0=$(cut -d' ' -f1 /proc/uptime)\nsync\necho 3 > /proc/sys/vm/drop_caches\necho 1 > /proc/sys/vm/compact_memory\n";
        s += "t1=$(cut -d' ' -f1 /proc/uptime)\necho '=== after'\necho \"took $t0 $t1\"\n";
        s += "grep -E '^(MemFree|MemAvailable|Cached):' /proc/meminfo\ncat /proc/buddyinfo\n";
    }
    s
}

/// `phone hugeblocks [--free]`: how much free memory comes in 2 MiB blocks, which a VM's RAM must (docs/native.md
/// "A VM under Gunyah", runs 5 to 7). With `--free`: first a sync, the file cache dropped and one compaction (one-shot
/// actions: Android reads its files again, nothing is lost, no setting changes). No VM is started.
pub fn hugeblocks(adb: &Adb, free: bool) -> Result<()> {
    adb.need_root()?;
    adb.refuse_in_session()?;
    let out = adb.root_ok(&hugeblocks_script(free), "the memory read")?;
    let parts = split(&out);
    let mut seen = Vec::new();
    for (name, text) in &parts {
        let (blocks, mib) = huge_free(text);
        let kb = |key: &str| text.lines().find(|l| l.starts_with(key)).and_then(|l| l.split_whitespace().nth(1)).unwrap_or("?").to_string();
        let mb = |key: &str| kb(key).parse::<u64>().map(|k| format!("{} MiB", k / 1024)).unwrap_or("?".into());
        if let Some(took) = text.lines().find(|l| l.starts_with("took ")) {
            let t: Vec<f64> = took.split_whitespace().skip(1).filter_map(|w| w.parse().ok()).collect();
            if t.len() == 2 {
                println!("drop_caches and compaction took {:.1} s", t[1] - t[0]);
            }
        }
        println!(
            "{name}: MemFree {}, MemAvailable {}, Cached {}; free in 2 MiB blocks: {blocks} ({mib} MiB)",
            mb("MemFree:"),
            mb("MemAvailable:"),
            mb("Cached:")
        );
        // the free blocks by order (4 KiB << order), as buddyinfo has them
        for line in text.lines().filter(|l| l.starts_with("Node ")) {
            println!("  {}", line.split_whitespace().collect::<Vec<_>>().join(" "));
        }
        seen.push(mib);
    }
    if seen.is_empty() {
        bail!("the read gave nothing: {}", out.trim());
    }
    Ok(())
}

/// `phone vm-probe`.
pub fn vm_probe(adb: &Adb, payload: &str, out: &str, o: &Options) -> Result<()> {
    let data = fs::read(payload).with_context(|| format!("cannot read {payload} (runtime/vmprobe/build.sh makes it)"))?;
    check_payload(&data)?;
    let sha = format!("{:x}", Sha256::digest(&data));
    adb.need_root()?;
    adb.refuse_in_session()?;
    let up = adb.root_ok(&format!("ps -A -o PID=,ARGS= | grep -F '{DIR}/probe.bin' | grep -v grep || true\n"), "the probe check")?;
    if !up.trim().is_empty() {
        bail!("a probe VM is still up on the phone: {}", up.trim());
    }
    // gh_hugepage_reserve's pool (`phone hugepool`) is RAM set aside for a VM that MemAvailable does not count: what
    // the VM takes from it leaves Android's own as it is
    let meminfo = adb.root_ok(
        "grep '^MemAvailable:' /proc/meminfo; grep '^pool_avail=' /sys/module/gh_hugepage_reserve/parameters/refill_stat 2>/dev/null\n",
        "the memory check",
    )?;
    let available = mem_available_mib(&meminfo).with_context(|| format!("no MemAvailable in \"{}\"", meminfo.trim()))?;
    let pool_mib = crate::hugepool::stat(&meminfo, "pool_avail").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0) * 2;
    if pool_mib > 0 {
        println!("vm-probe: gh_hugepage_reserve holds {pool_mib} MiB for VMs, beside Android's {available} MiB available");
    }
    check_mem(o.mem, available + pool_mib.min(o.mem as u64))?;
    if o.shared_ram {
        if o.crosvm.is_none() {
            bail!("--shared-ram is Droid-VM's crosvm's mode: give --droidvm too");
        }
        let dev = adb.root_ok(&format!("[ -c {} ] && echo there || echo absent\n", crate::hostshare::DEV), "gunyah_share")?;
        if !dev.contains("there") {
            bail!("no {} on the phone: `phone hostshare load` first", crate::hostshare::DEV);
        }
    }
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the probe's folder")?;
    adb.push(payload, &format!("{DIR}/probe.bin"))?;
    println!(
        "vm-probe: {} bytes, sha256 {sha}; {} runs it (1 vCPU, {} MiB, {}, at most {} s, then crosvm stop)",
        data.len(),
        o.crosvm.as_deref().unwrap_or(CROSVM),
        o.mem,
        protection(o),
        o.timeout
    );
    let r = adb.root(&run_script(o, &sha))?;
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let parts = split(&r.out);
    let get = |name: &str| parts.iter().find(|(n, _)| n == name).map(|(_, t)| t.as_str()).unwrap_or("");
    let (run, serial, log, kmsg) = (get("run"), get("serial.txt"), get("crosvm.log"), get("dmesg"));
    fs::write(Path::new(out).join("run.txt"), format!("{run}{}", r.err))?;
    fs::write(Path::new(out).join("serial.txt"), serial)?;
    fs::write(Path::new(out).join("crosvm.log"), log)?;
    // the kernel's own lines of the run (Gunyah's demand paging logs why it could not give a page): the phone's log
    // keeps only minutes, so they are read right after the run
    fs::write(Path::new(out).join("dmesg.txt"), kmsg)?;
    // the VM's device tree as crosvm made it (--dump-device-tree-blob): the machine an XNU guest is given
    let tree = Path::new(out).join("fdt.dtb");
    let mut child = adb.exec_out(&format!("su -c 'cat {DIR}/fdt.dtb'"))?;
    let mut file = fs::File::create(&tree).with_context(|| format!("cannot write {}", tree.display()))?;
    let n = std::io::copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
    child.wait()?;
    let good = fs::read(&tree).map(|d| d.len() >= 40 && d[..4] == [0xd0, 0x0d, 0xfe, 0xed]).unwrap_or(false);
    if !good {
        fs::remove_file(&tree).ok();
    }
    print!("{run}");
    println!("fdt.dtb: {}", if good { format!("{n} bytes (crosvm's tree for this VM)") } else { "none".to_string() });
    if !r.err.trim().is_empty() {
        println!("stderr: {}", r.err.trim());
    }
    println!("--- serial ({} lines)", serial.lines().count());
    print!("{serial}");
    if !serial.contains("ONEIOS VMPROBE done") {
        let lines: Vec<&str> = log.lines().collect();
        println!("--- crosvm.log, last 25 of {} lines", lines.len());
        for l in &lines[lines.len().saturating_sub(25)..] {
            println!("{l}");
        }
        let klines: Vec<&str> = kmsg.lines().collect();
        println!("--- the kernel's lines (dmesg: gunyah, folio, gh_, crosvm, scm), last 25 of {}", klines.len());
        for l in &klines[klines.len().saturating_sub(25)..] {
            println!("{l}");
        }
    }
    println!("vm-probe: {} (files in {out})", verdict(run, serial));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_run_stops_before_it_kills_and_reads_nothing_from_the_script() {
        let s = run_script(&Options { mem: 128, timeout: 20, thp: false, crosvm: None, shared_ram: false, thp_shmem: "advise" }, "ab12");
        assert!(s.contains("C=/apex/com.android.virt/bin/crosvm\n"));
        assert!(s.contains("--protected-vm-without-firmware --swiotlb 8 --hugepages --mem size=128 "));
        // Droid-VM's crosvm and its shared-RAM VM: no bounce pool
        let d = Options { mem: 128, timeout: 20, thp: false, crosvm: Some("/data/local/tmp/oneios-droidvm/crosvm".into()), shared_ram: true, thp_shmem: "within_size" };
        let shared = run_script(&d, "ab12");
        assert!(shared.contains("C=/data/local/tmp/oneios-droidvm/crosvm\n"));
        assert!(shared.contains("--hypervisor gunyah \\\n  --protected-vm-pseudo-unprotected --hugepages --mem size=128 "));
        assert!(!shared.contains("--swiotlb") && !shared.contains("without-firmware"));
        assert!(!shared.contains("if [ 1 = 1 ]") && shared.contains("echo within_size > $T/shmem_enabled"));
        let stop = s.find("$C stop $D/crosvm.sock").expect("crosvm stop");
        let kill = s.find("kill -9 $pid").expect("the last resort");
        assert!(stop < kill, "stop comes first");
        assert_eq!(s.matches("kill -9").count(), 1);
        assert!(s.contains("$D/probe.bin </dev/null"), "crosvm's stdin is /dev/null");
        assert!(s.contains("--protected-vm-without-firmware") && s.contains("--hypervisor gunyah"));
        assert!(s.contains("--no-balloon --no-rng --no-usb") && s.contains("--cpus num-cores=1") && s.contains("--mem size=128"));
        assert!(s.contains("[ \"$got\" = \"ab12\" ]"), "the payload's sha256 is checked before the run");
        assert!(s.contains("while [ $t -lt 20 ]"));
        assert!(s.contains("--dump-device-tree-blob $D/fdt.dtb $D/probe.bin"));
        assert!(s.contains("echo '=== dmesg'"), "the kernel's lines right after the run");
        // no glob: the only stars are the defaults of the two case statements that check the old THP values
        assert!(!s.replace(" *) echo \"thp: ", "").contains('*'), "no glob");
        assert!(s.contains("if [ 0 = 1 ]; then"), "without --thp the settings are not touched");
    }

    #[test]
    fn huge_free_counts_the_2_mib_blocks() {
        // run 7's "before": 23 of order 9 and 52 of order 10
        let b = "Node 0, zone   Normal  17518  18192   9932   3171    197     30      9      5      3     23     52 \n";
        assert_eq!(huge_free(b), (23 + 2 * 52, (23 + 2 * 52) * 2));
        assert_eq!(huge_free("Node 0, zone   Normal 1 2 3 4 5 6 7 8 9 0 0\n"), (0, 0));
        let two = format!("{b}Node 0, zone    DMA32 0 0 0 0 0 0 0 0 0 1 1\n");
        assert_eq!(huge_free(&two).0, 23 + 2 * 52 + 3);
    }

    #[test]
    fn hugeblocks_only_reads_unless_asked_to_free() {
        let read = hugeblocks_script(false);
        assert!(!read.contains('>') && !read.contains("sync"), "a read only");
        let free = hugeblocks_script(true);
        let writes: Vec<&str> = free.lines().filter(|l| l.contains('>')).collect();
        assert_eq!(writes, vec!["echo 3 > /proc/sys/vm/drop_caches", "echo 1 > /proc/sys/vm/compact_memory"]);
        assert!(free.find("sync").unwrap() < free.find("drop_caches").unwrap());
    }

    #[test]
    fn thp_is_set_for_the_run_only_and_always_written_back() {
        let s = run_script(&Options { mem: 1024, timeout: 60, thp: true, crosvm: None, shared_ram: false, thp_shmem: "advise" }, "ab12");
        assert!(s.contains("if [ 1 = 1 ]; then"));
        assert!(s.contains("T=/sys/kernel/mm/transparent_hugepage"));
        // the old values are read before anything is written, and the trap is set before the write
        let read = s.find("old_shmem=$(tr").unwrap();
        let trap = s.find("trap thp_restore EXIT HUP INT TERM").unwrap();
        let write = s.find("echo advise > $T/shmem_enabled && echo madvise > $T/defrag").unwrap();
        assert!(read < trap && trap < write);
        // written back right after crosvm ends, before the end line
        let wait = s.find("wait $pid; rc=$?").unwrap();
        let back = s[wait..].find("\nthp_restore\n").map(|i| i + wait).unwrap();
        let end = s.find("echo \"end $(date").unwrap();
        assert!(wait < back && back < end);
        // the only writes under /sys are those two and their restore
        let writes: Vec<&str> = s.lines().filter(|l| l.contains("> $T/")).collect();
        assert_eq!(writes.len(), 3, "{writes:?}");
        assert!(s.contains("echo \"$old_shmem\" > $T/shmem_enabled") && s.contains("echo \"$old_defrag\" > $T/defrag"));
        // a refused write is undone and nothing starts
        assert!(s.contains("echo \"thp: the write was refused\"; thp_restore; exit 1"));
        // one compaction, inside the --thp block, before crosvm starts; the only write under /proc
        let compact = s.find("echo 1 > /proc/sys/vm/compact_memory").unwrap();
        assert!(compact > s.find("if [ 1 = 1 ]; then").unwrap() && compact < s.find("$C --log-level").unwrap());
        assert_eq!(s.lines().filter(|l| l.contains("> /proc/")).count(), 1);
    }

    #[test]
    fn the_vm_leaves_android_a_gib() {
        assert_eq!(mem_available_mib("MemAvailable:    3276800 kB\n"), Some(3200));
        assert_eq!(mem_available_mib("MemFree: 1 kB\n"), None);
        assert!(check_mem(128, 3200).is_ok());
        assert!(check_mem(2048, 3200).is_ok());
        assert!(check_mem(2304, 3200).is_err(), "2304 + 1024 > 3200");
        assert!(check_mem(0, 9999).is_err() && check_mem(MAX_MEM + 1, 99999).is_err());
    }

    #[test]
    fn a_payload_is_an_arm64_image() {
        let mut image = vec![0u8; 4096];
        image[0x38..0x3c].copy_from_slice(b"ARM\x64");
        assert!(check_payload(&image).is_ok());
        assert!(check_payload(&image[..32]).is_err());
        image[0x38] = b'X';
        assert!(check_payload(&image).is_err());
    }

    #[test]
    fn the_verdict_reads_the_run_and_the_serial() {
        let run = "start 100\nend 101 how itself exit 0\n";
        assert_eq!(verdict(run, "ONEIOS VMPROBE start\nONEIOS VMPROBE done\n"), "the VM ran the whole probe and ended by itself; crosvm exit 0");
        assert_eq!(
            verdict("start 1\nend 41 how stop exit 0\n", "ONEIOS VMPROBE start\n"),
            "the VM started the probe but did not finish it and had to be stopped (crosvm stop); crosvm exit 0"
        );
        assert!(verdict("sha256 x, not y\n", "").starts_with("no line from the VM"));
        assert!(verdict("sha256 x, not y\n", "").contains("no end line"));
    }
}
