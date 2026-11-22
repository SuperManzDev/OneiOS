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
}

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
C={CROSVM}
echo '=== run'
cd $D || exit 1
got=$(sha256sum probe.bin | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; exit 1; }}
rm -f serial.txt crosvm.log crosvm.sock fdt.dtb
ulimit -l unlimited 2>/dev/null || echo "ulimit -l unlimited: refused"
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/before /'
echo "start $(date +%s) up $(cut -d' ' -f1 /proc/uptime)"
$C --log-level debug run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  --protected-vm-without-firmware --swiotlb 8 --hugepages --mem size={mem} --cpus num-cores=1 \
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
echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
grep -E '^(MemAvailable|SwapFree):' /proc/meminfo | tr -s ' ' | sed 's/^/after /'
echo '=== serial.txt'
head -c 65536 serial.txt 2>/dev/null
echo '=== crosvm.log'
tail -c 65536 crosvm.log 2>/dev/null
"#,
        mem = o.mem
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
    let meminfo = adb.root_ok("grep '^MemAvailable:' /proc/meminfo\n", "the memory check")?;
    let available = mem_available_mib(&meminfo).with_context(|| format!("no MemAvailable in \"{}\"", meminfo.trim()))?;
    check_mem(o.mem, available)?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the probe's folder")?;
    adb.push(payload, &format!("{DIR}/probe.bin"))?;
    println!("vm-probe: {} bytes, sha256 {sha}; crosvm runs it (1 vCPU, {} MiB, at most {} s, then crosvm stop)", data.len(), o.mem, o.timeout);
    let r = adb.root(&run_script(o, &sha))?;
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let parts = split(&r.out);
    let get = |name: &str| parts.iter().find(|(n, _)| n == name).map(|(_, t)| t.as_str()).unwrap_or("");
    let (run, serial, log) = (get("run"), get("serial.txt"), get("crosvm.log"));
    fs::write(Path::new(out).join("run.txt"), format!("{run}{}", r.err))?;
    fs::write(Path::new(out).join("serial.txt"), serial)?;
    fs::write(Path::new(out).join("crosvm.log"), log)?;
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
    }
    println!("vm-probe: {} (files in {out})", verdict(run, serial));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_run_stops_before_it_kills_and_reads_nothing_from_the_script() {
        let s = run_script(&Options { mem: 128, timeout: 20 }, "ab12");
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
        assert!(!s.contains('*'), "no glob");
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
