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
//!
//! The screen: an Image made with xnuloader-crosvm --display has the boot video at the start of the pool and the
//! guest's frame count after it (FILE.disks: `fb OFFSET BYTES WIDTH HEIGHT STRIDE SEQ`, offsets in the pool). The tool
//! then pushes runtime/display's displayctl (`--bridge`) beside the Image, and the script starts it the moment the pool
//! check passed: `displayctl pool` maps only that part of crosvm's pool and serves it to the staged display helper
//! (runtime/bin/oneios-display), a layer over One UI that goes away when the VM ends. Touches still go to One UI under
//! it. Its log is `display.txt`.
//!
//! The touch: such an Image also has the input bus (`input touch` in FILE.disks). The script finds the phone's touchscreen
//! by its name in /proc/bus/input/devices (`sec_touchscreen`, with INPUT_PROP_DIRECT; refused should it be a side key's
//! device) and gives it to the VM through runtime/display's `displayctl touch` (pushed as the bridge is): it listens on
//! `touch.sock`, and when crosvm connects there (`--input multi-touch[path=...]`) it takes the touchscreen for the VM's
//! life (One UI under the layer gets no touches) and passes its frames on. crosvm's own `--evdev` cannot take it: it wants
//! the device's serial name, which sec_touchscreen has none of (boot 4's first start). The kernel reads crosvm's device as
//! a virtio-input PCI function, its queues in the pool. The side keys stay with Android; volume down held 3 s stops the
//! VM (getevent reads the key and does not take it): the way back without the PC. Their logs are `touch.txt` and run.txt.
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
    /// The boot video and the frame count in the pool (xnuloader-crosvm --display).
    pub fb: Option<Fb>,
    /// The kernel has the input bus (`input touch`): the phone's touchscreen goes to crosvm.
    pub input: bool,
}

/// `fb OFFSET BYTES WIDTH HEIGHT STRIDE SEQ`: offsets in the pool, the frame first, the count's page after it.
#[derive(Debug, PartialEq)]
pub struct Fb {
    pub offset: u64,
    pub bytes: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub seq: u64,
}

/// A value the phone's shell and crosvm's key=value parser take as it is: no blank, quote or shell character.
fn plain(text: &str) -> bool {
    !text.is_empty() && text.chars().all(|c| c.is_ascii_alphanumeric() || "/._-=:,".contains(c))
}

pub fn parse_disks(text: &str) -> Result<Disks> {
    let (mut swiotlb, mut pool, mut store, mut fb, mut input) = (None, None, None, None, false);
    let mut blocks = Vec::new();
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let words: Vec<&str> = line.split_whitespace().collect();
        match words.as_slice() {
            ["swiotlb-mib", v] => swiotlb = Some(v.parse::<u32>().context("swiotlb-mib")?),
            ["pool", v] => pool = Some(u64::from_str_radix(v.strip_prefix("0x").context("pool 0x...")?, 16).context("pool")?),
            ["store", path, gib] => store = Some((path.to_string(), gib.parse::<u32>().context("store GIB")?)),
            ["block", size, value] => {
                let size = if *size == "-" { None } else { Some(size.parse::<u64>().context("block BYTES")?) };
                blocks.push((size, value.to_string()));
            }
            ["fb", offset, bytes, width, height, stride, seq] => {
                fb = Some(Fb {
                    offset: offset.parse().context("fb OFFSET")?,
                    bytes: bytes.parse().context("fb BYTES")?,
                    width: width.parse().context("fb WIDTH")?,
                    height: height.parse().context("fb HEIGHT")?,
                    stride: stride.parse().context("fb STRIDE")?,
                    seq: seq.parse().context("fb SEQ")?,
                })
            }
            ["input", "touch"] => input = true,
            _ => bail!("a line of the disks list that is not one of swiotlb-mib, pool, store, block, fb, input: {line:?}"),
        }
    }
    let (store, store_gib) = store.context("no store line")?;
    let d = Disks { swiotlb: swiotlb.context("no swiotlb-mib line")?, pool: pool.context("no pool line")?, store, store_gib, blocks, fb, input };
    if d.input && d.fb.is_none() {
        bail!("the input bus without a screen: xnuloader-crosvm gives both with --display");
    }
    if let Some(f) = &d.fb {
        // as displayctl pool takes them: the frame page aligned at the pool's start or after, the count after it
        let pool_bytes = (d.swiotlb as u64) << 20;
        let ok = (16..=16384).contains(&f.width) && (16..=16384).contains(&f.height) && f.stride as u64 >= f.width as u64 * 4
            && f.bytes == f.stride as u64 * f.height as u64 && f.offset % 16384 == 0 && f.seq % 16384 == 0
            && f.seq >= f.offset + f.bytes && f.seq + 16384 <= pool_bytes;
        if !ok {
            bail!("the fb line {f:?} does not lie in the pool of {} MiB as the frame and then its count", d.swiotlb);
        }
    }
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
    boot_script_with(l, seconds, thp, sha, disks, false)
}

/// The same, with `swipe`: the touch forwarder's test swipes (`--swipe-test`: 200 s after crosvm connected, then every
/// 60 s), a test of the VM's touch without a finger.
pub fn boot_script_with(l: &Label, seconds: u32, thp: bool, sha: &str, disks: Option<&Disks>, swipe: bool) -> String {
    let (mem, cpus, swiotlb) = (l.ram, l.cpus, l.swiotlb);
    let mut checks = String::new();
    let mut blocks = String::new();
    let mut pool_check = String::new();
    let (mut bridge_start, mut bridge_end) = (String::new(), String::new());
    let (mut touch_find, mut escape_start, mut escape_end) = (String::new(), String::new(), String::new());
    if disks.is_some_and(|d| d.input) {
        // The touchscreen by its name (and INPUT_PROP_DIRECT), never a side key's device (rule: the way back to One UI)
        touch_find = concat!(
            "dev_of() {\n",
            "  awk -v want=\"N: Name=\\\"$1\\\"\" 'BEGIN { RS = \"\" } index($0, want \"\\n\") && (p == \"\" || index($0, \"B: PROP=\" p \"\\n\")) {\n",
            "    if (match($0, /Handlers=[^\\n]*/)) { h = substr($0, RSTART, RLENGTH); if (match(h, /event[0-9]+/)) print substr(h, RSTART, RLENGTH) } }' p=\"$2\" /proc/bus/input/devices\n",
            "}\n",
            "tdev=$(dev_of sec_touchscreen 2)\n",
            "keys=\" $(dev_of gpio-keys | tr '\\n' ' ')$(dev_of pmic_pwrkey | tr '\\n' ' ')$(dev_of pmic_resin | tr '\\n' ' ')\"\n",
            "case \"$tdev\" in event[0-9]|event[0-9][0-9]) ;; *) tdev= ;; esac\n",
            "case \"$keys\" in *\" $tdev \"*) [ -n \"$tdev\" ] && echo \"touch: $tdev is a side key's device: not given to the VM\"; tdev= ;; esac\n",
            "if [ -n \"$tdev\" ] && [ -c /dev/input/$tdev ]; then\n",
            "  echo \"touch: /dev/input/$tdev (sec_touchscreen) goes to the VM while it runs; the side keys stay with Android\"\n",
            "else\n",
            "  tdev=; echo \"touch: no sec_touchscreen with INPUT_PROP_DIRECT: the VM runs without touch\"\n",
            "fi\n",
            // The forwarder listens before crosvm starts: crosvm connects to its socket while it makes its devices
            "tsock=\nforward=\n",
            "if [ -n \"$tdev\" ] && [ -x $D/displayctl ]; then\n",
            "  rm -f $D/touch.sock\n",
            "  $D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock </dev/null >>$D/touch.txt 2>&1 &\n",
            "  forward=$!\n",
            "  t=0; while [ $t -lt 50 ] && [ ! -S $D/touch.sock ] && kill -0 $forward 2>/dev/null; do sleep 0.1 2>/dev/null || sleep 1; t=$((t + 1)); done\n",
            "  if [ -S $D/touch.sock ]; then\n",
            "    tsock=$D/touch.sock; echo \"touch: the forwarder ($forward) takes it when crosvm connects to $tsock: touch.txt\"\n",
            "  else\n",
            "    echo \"touch: the forwarder did not start (touch.txt): the VM runs without touch\"; kill $forward 2>/dev/null; forward=\n",
            "  fi\n",
            "fi\n"
        )
        .to_string();
        // Volume down held 3 s: stop.request (getevent reads pmic_resin's events; Android still gets the key)
        escape_start = concat!(
            "escape=\n",
            "vd=$(dev_of pmic_resin)\n",
            "if [ -n \"$tsock\" ] && [ -n \"$vd\" ] && [ -c /dev/input/$vd ]; then\n",
            "  ( getevent -l /dev/input/$vd 2>/dev/null | while read -r a b c e; do\n",
            "      case \"$a $b $c $e\" in\n",
            "        *KEY_VOLUMEDOWN*DOWN*) down=$(cut -d. -f1 /proc/uptime) ;;\n",
            "        *KEY_VOLUMEDOWN*UP*) now=$(cut -d. -f1 /proc/uptime)\n",
            "          if [ -n \"$down\" ] && [ $((now - down)) -ge 3 ]; then echo \"touch: volume down held $((now - down)) s: the VM stops\"; touch $D/stop.request; fi\n",
            "          down= ;;\n",
            "      esac\n",
            "    done ) &\n",
            "  escape=$!\n",
            "  echo \"touch: volume down held 3 s stops the VM (the way back without the PC)\"\n",
            "fi\n"
        )
        .to_string();
        // After the VM: the forwarder sees crosvm gone and gives the touchscreen back; one still there 3 s on is ended
        escape_end = concat!(
            "if [ -n \"$escape\" ]; then kill $escape 2>/dev/null; pkill -f \"getevent -l /dev/input/$vd\" 2>/dev/null; wait $escape 2>/dev/null; fi\n",
            "if [ -n \"$forward\" ]; then\n",
            "  t=0; while [ $t -lt 3 ] && kill -0 $forward 2>/dev/null; do sleep 1; t=$((t + 1)); done\n",
            "  kill $forward 2>/dev/null; wait $forward 2>/dev/null\n",
            "  echo \"touch: $(grep '^displayctl touch:' $D/touch.txt | tail -n 1)\"\n",
            "fi\n"
        )
        .to_string();
    }
    if let Some((d, f)) = disks.and_then(|d| d.fb.as_ref().map(|f| (d, f))) {
        // The screen, once crosvm's tree shows the pool where the Image put it: the bridge maps only the frame and its
        // count of the pool (displayctl pool), the helper is the staged runtime's
        bridge_start = format!(
            "bridge=\nif [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then\n  \
             if [ -x $D/displayctl ] && [ -x $H ]; then\n    \
             TMPDIR=$D $D/displayctl pool --pid $pid --pool-offset {po} --pool-bytes {pb} --fb-offset {fo} --size {w}x{h} --stride {s} \
             --seq-offset {sq} --helper $H </dev/null >>$D/display.txt 2>&1 &\n    \
             bridge=$!\n    echo \"display: the bridge ($bridge) shows the VM's screen on a layer over One UI: display.txt\"\n  \
             else\n    echo \"display: no $D/displayctl or no $H: the VM runs without its screen\"\n  fi\nfi\n",
            po = d.pool - RAM_BASE,
            pb = (d.swiotlb as u64) << 20,
            fo = f.offset,
            w = f.width,
            h = f.height,
            s = f.stride,
            sq = f.seq
        );
        // After the VM: the bridge sees crosvm gone and closes its session (the layer goes); a bridge still there after
        // 3 s is asked to end, and after 8 s ended
        bridge_end = "if [ -n \"$bridge\" ]; then\n  \
             t=0; while [ $t -lt 8 ] && kill -0 $bridge 2>/dev/null; do [ $t = 3 ] && kill -TERM $bridge 2>/dev/null; sleep 1; t=$((t + 1)); done\n  \
             kill -0 $bridge 2>/dev/null && kill -9 $bridge 2>/dev/null\n  wait $bridge 2>/dev/null\n  \
             echo \"display: $(grep -E '^displayctl pool: (the VM ended|time is up|stopped|the helper left)' $D/display.txt | tail -n 1)\"\nfi\n"
            .to_string();
    }
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
        if swipe {
            touch_find = touch_find.replace("--socket $D/touch.sock </dev/null", "--socket $D/touch.sock --swipe-at 200 --swipe-every 60 </dev/null");
        }
    }
    format!(
        r#"D={DIR}
C=/apex/com.android.virt/bin/crosvm
H=$D/oneios-display
exec >>$D/run.txt 2>&1 </dev/null
cd $D || exit 1
end() {{ echo "$*" > $D/result; }}
got=$(sha256sum kernel.img | cut -d' ' -f1)
[ "$got" = "{sha}" ] || {{ echo "sha256 $got, not {sha}"; end "refused sha256"; exit 1; }}
[ -c /dev/gunyah ] || {{ echo "no /dev/gunyah"; end "refused no /dev/gunyah"; exit 1; }}
tdev=
tsock=
{checks}rm -f serial.txt crosvm.log crosvm.sock fdt.dtb dmesg.txt display.txt touch.txt touch.sock stop.request
{touch_find}
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
set -f
$C --log-level debug run --disable-sandbox --no-balloon --no-rng --no-usb --hypervisor gunyah \
  --protected-vm-without-firmware --swiotlb {swiotlb} --hugepages --mem size={mem} --cpus num-cores={cpus} \
  --socket $D/crosvm.sock${{tsock:+ --input multi-touch[path=$tsock,width=4095,height=4095,name=oneios-touch]}} --serial type=file,hardware=serial,num=1,path=$D/serial.txt,console \
  --dump-device-tree-blob $D/fdt.dtb{blocks} $D/kernel.img </dev/null >$D/crosvm.log 2>&1 &
pid=$!
set +f
{pool_check}{bridge_start}{escape_start}t=0
while [ $t -lt {seconds} ] && [ ! -e $D/stop.request ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
how=itself
if kill -0 $pid 2>/dev/null; then
  how=stop; $C stop $D/crosvm.sock </dev/null >/dev/null 2>&1
  t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
fi
if kill -0 $pid 2>/dev/null; then how=kill; kill -9 $pid; fi
wait $pid; rc=$?
thp_restore
{bridge_end}{escape_end}echo "end $(date +%s) up $(cut -d' ' -f1 /proc/uptime) how $how exit $rc"
dmesg 2>/dev/null | grep -i -E 'gunyah|folio|gh_|crosvm|scm|memextent' | tail -n 80 > $D/dmesg.txt
if [ "$pool" = bad ]; then end "refused pool: end how $how exit $rc"; else end "end how $how exit $rc"; fi
"#,
        thp = if thp { 1 } else { 0 }
    )
}

fn gen_path(dir: &Path, name: &str) -> String {
    dir.join(name).to_string_lossy().into_owned()
}

/// What of a VM's RAM may be outside gh_hugepage_reserve's pool, in 4 KiB pages: Gunyah's own heap maps no more than
/// about 470 MiB of those for all VMs (vm-probe runs 3 to 8). A VM whose RAM it cannot map ends at the first fault there:
/// boot 4b, the pool at 198 pages after boot 4, "gunyah_vm_mgr: Failed to map guest address ...: 10" and crosvm's vCPU
/// "unknown gh exit reason: 3", 48 s into the run.
pub const SMALL_PAGES_MIB: u64 = 384;

/// A VM above 480 MiB with no more than SMALL_PAGES_MIB of its RAM outside the pool's free pages (`pool_mib`).
pub fn check_pool(ram_mib: u64, pool_mib: u64) -> Result<()> {
    if ram_mib > 480 && ram_mib.saturating_sub(pool_mib) > SMALL_PAGES_MIB {
        bail!(
            "gh_hugepage_reserve's pool has {pool_mib} MiB free, and a VM of {ram_mib} MiB may take no more than {SMALL_PAGES_MIB} MiB \
             in 4 KiB pages (Gunyah's heap): refill the pool first (`phone hugepool acquire`, beside One UI, or `phone hugepool grow`)"
        );
    }
    Ok(())
}

pub struct Options {
    pub seconds: u32,
    pub thp: bool,
    /// displayctl for the phone, pushed when the Image has a screen
    pub bridge: String,
    /// The touch forwarder's test swipes (boot_script_with)
    pub swipe_test: bool,
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
    if !(5..=3600).contains(&o.seconds) || !(1..=8).contains(&l.cpus) || !(2..=1024).contains(&l.swiotlb) {
        bail!("--seconds 5..3600; the Image's vCPUs 1..8, its pool 2..1024 MiB");
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
    if disks.as_ref().is_some_and(|d| d.fb.is_some()) && !Path::new(&o.bridge).is_file() {
        bail!("the Image has a screen and {} is not there: make -C runtime/display (or name it with --bridge)", o.bridge);
    }
    let up = adb.root_ok(
        "ps -A -o PID=,ARGS= | grep -F -e 'oneios-vm' -e 'displayctl pool' | grep -F -e crosvm -e displayctl | grep -v grep || true\n",
        "the VM check",
    )?;
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
    check_pool(l.ram as u64, pool_mib)?;
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR} && rm -f {DIR}/result {DIR}/run.txt {DIR}/serial.txt\n"), "the VM's folder")?;
    println!("vm-boot: {} MiB Image, sha256 {sha}: pushing", data.len() >> 20);
    adb.push(image, &format!("{DIR}/kernel.img"))?;
    if let Some(f) = disks.as_ref().and_then(|d| d.fb.as_ref()) {
        // The bridge and the display helper the VM runs, as built beside each other (runtime/display: make): the VM's
        // own copy, root's, so that neither depends on the staged runtime's version nor can be changed by the shell
        // user while root runs them.
        let built = Path::new(&o.bridge).parent().unwrap_or(Path::new("."));
        for (file, to) in [(o.bridge.clone(), "displayctl"), (gen_path(built, "oneios-display"), "oneios-display"),
                           (gen_path(built, "liboneios-display.so"), "liboneios-display.so"), (gen_path(built, "oneios-display.dex"), "oneios-display.dex")] {
            if !Path::new(&file).is_file() {
                bail!("{file} is not there: make -C runtime/display");
            }
            adb.push(&file, &format!("{DIR}/{to}"))?;
        }
        adb.root_ok(
            &format!(
                "cd {DIR} && chown root:root displayctl oneios-display liboneios-display.so oneios-display.dex && \
                 chmod 755 displayctl oneios-display && chmod 644 liboneios-display.so oneios-display.dex\n"
            ),
            "the bridge's and the helper's owner",
        )?;
        if disks.as_ref().is_some_and(|d| d.input) {
            println!(
                "vm-boot: the screen ({}x{}) on a layer over One UI, the touchscreen to the VM while it runs; volume down held 3 s stops it",
                f.width, f.height
            );
        } else {
            println!("vm-boot: the screen ({}x{}) on a layer over One UI while the VM runs (touches still go to One UI under it)", f.width, f.height);
        }
    }
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let script = Path::new(out).join("boot.sh");
    if o.swipe_test && !disks.as_ref().is_some_and(|d| d.input) {
        bail!("--swipe-test needs an Image with the touch (xnuloader-crosvm --display)");
    }
    fs::write(&script, boot_script_with(&l, o.seconds, o.thp, &sha, disks.as_ref(), o.swipe_test))?;
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
            "tail -c +{} {DIR}/serial.txt 2>/dev/null | head -c 262144; echo; echo '=== oneios result'; cat {DIR}/result 2>/dev/null; true\n",
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
    for name in ["serial.txt", "crosvm.log", "dmesg.txt", "run.txt", "fdt.dtb", "display.txt", "touch.txt"] {
        let path = Path::new(out).join(name);
        let mut child = adb.exec_out(&format!("su -c 'cat {DIR}/{name}'"))?;
        let mut file = fs::File::create(&path)?;
        copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
        child.wait()?;
    }
    let run = fs::read_to_string(Path::new(out).join("run.txt")).unwrap_or_default();
    for line in run.lines().filter(|l| {
        l.starts_with("disk") || l.starts_with("store:") || l.starts_with("pool:") || l.starts_with("/data:") || l.starts_with("display:")
            || l.starts_with("touch:")
    }) {
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
    fn the_pool_must_hold_what_gunyah_cannot_map_in_small_pages() {
        assert!(check_pool(2880, 2688).is_ok());      // boot 3: 192 MiB in 4 KiB pages
        assert!(check_pool(2400, 2406).is_ok());      // boot 4
        assert!(check_pool(2400, 396).is_err());      // boot 4b's first try: the VM ended at a fault 48 s in
        assert!(check_pool(2400, 2016).is_ok() && check_pool(2400, 2014).is_err());
        assert!(check_pool(256, 0).is_ok());          // a small VM needs no pool
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

    /// xnuloader-crosvm --display's list (2880 MiB, a pool of 32 MiB: the frame at its start, the count at 17973248).
    const SCREEN: &str = "swiotlb-mib 32\npool 0x132000000\nstore /data/local/tmp/oneios-vmboot/data.img 16\n\
        fb 0 17971200 1440 3120 5760 17973248\n\
        block 10359930880 path=/data/local/tmp/oneios/image/os.img,ro=true,id=os,pci-address=00:02.0\n\
        block - path=/data/local/tmp/oneios-vmboot/data.img,ro=false,id=data,pci-address=00:06.0\n";

    #[test]
    fn the_screen_is_in_the_pool() {
        let d = parse_disks(SCREEN).unwrap();
        assert_eq!(d.fb, Some(Fb { offset: 0, bytes: 17971200, width: 1440, height: 3120, stride: 5760, seq: 17973248 }));
        assert_eq!(parse_disks(DISKS).unwrap().fb, None);
        // the count inside the frame, past the pool's end, a frame that is not stride x height, an odd offset
        assert!(parse_disks(&SCREEN.replace(" 17973248\n", " 16384\n")).is_err());
        assert!(parse_disks(&SCREEN.replace(" 17973248\n", " 33554432\n")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200", "fb 0 17971204")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200", "fb 4096 17971200")).is_err());
        assert!(parse_disks(&SCREEN.replace("fb 0 17971200 1440 3120 5760 17973248", "fb 0 17971200 1440 3120")).is_err());
    }

    #[test]
    fn the_bridge_starts_after_the_pool_check_and_ends_after_the_vm() {
        let d = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 2880, cpus: 1, swiotlb: 32 };
        let s = boot_script(&l, 900, true, "ab12", Some(&d));
        let pool = s.find("grep -q -F restricted_dma_reserved@132000000 $D/fdt.dtb").unwrap();
        let bridge = s.find("TMPDIR=$D $D/displayctl pool --pid $pid").unwrap();
        let wait = s.find("while [ $t -lt 900 ]").unwrap();
        assert!(pool < bridge && bridge < wait);
        // the pool's offset in the memfd and its size, the frame and the count; the staged helper
        assert!(s.contains("--pool-offset 2986344448 --pool-bytes 33554432 --fb-offset 0 --size 1440x3120 --stride 5760 --seq-offset 17973248 --helper $H"));
        assert!(s.contains("H=$D/oneios-display\n"));
        assert!(s.contains("if [ \"$pool\" != bad ] && kill -0 $pid 2>/dev/null; then"));
        // after the VM, the bridge is waited for, asked to end, then ended
        let end = s.find("if [ -n \"$bridge\" ]; then").unwrap();
        assert!(s.find("wait $pid; rc=$?\nthp_restore\n").unwrap() < end);
        assert!(s[end..].find("kill -TERM $bridge").unwrap() < s[end..].find("kill -9 $bridge").unwrap());
        assert!(s.contains("--swiotlb 32 --hugepages --mem size=2880"));
        assert!(!boot_script(&l, 60, true, "x", Some(&parse_disks(DISKS).unwrap())).contains("displayctl"));
    }

    /// The S25's input devices that matter (2026-11-23: `cat /proc/bus/input/devices`), in their order.
    const DEVICES: &str = "I: Bus=0019 Vendor=0001 Product=0001 Version=0100\nN: Name=\"gpio-keys\"\nP: Phys=gpio-keys/input0\n\
        S: Sysfs=/devices/platform/soc/soc:gpio_keys/input/input0\nU: Uniq=\nH: Handlers=event0 sec_key_notifier cpufreq sec_tsp_dumpkey \n\
        B: PROP=0\nB: EV=3\nB: KEY=8000000000000 0\n\n\
        I: Bus=0000 Vendor=0000 Product=0000 Version=0000\nN: Name=\"pmic_pwrkey\"\nP: Phys=pmic_pwrkey/input0\n\
        S: Sysfs=/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-00/pwrkey/input/input3\nU: Uniq=\n\
        H: Handlers=event3 sec_key_notifier cpufreq sec_tsp_dumpkey \nB: PROP=0\nB: EV=3\nB: KEY=10000000000000 0\n\n\
        I: Bus=0000 Vendor=0000 Product=0000 Version=0000\nN: Name=\"pmic_resin\"\nP: Phys=pmic_resin/input0\n\
        S: Sysfs=/devices/platform/soc/c42d000.qcom,spmi/spmi-0/0-00/resin/input/input4\nU: Uniq=\n\
        H: Handlers=event4 sec_key_notifier cpufreq sec_tsp_dumpkey \nB: PROP=0\nB: EV=3\nB: KEY=4000000000000 0\n\n\
        I: Bus=0018 Vendor=0000 Product=0000 Version=0000\nN: Name=\"sec_touchscreen\"\nP: Phys=sec_touchscreen\n\
        S: Sysfs=/devices/platform/soc/ac0000.qcom,qupv3_1_geni_se/a90000.spi/spi_master/spi1/spi1.0/input/input8\nU: Uniq=\n\
        H: Handlers=event8 sec_key_notifier cpufreq sec_tsp_dumpkey kgsl sde \nB: PROP=2\nB: EV=2b\n\
        B: KEY=4000000000000000 0 4000000000 0 0 420 3000000 0 8000 0 0\nB: ABS=263800000000003\nB: SW=0\n\n\
        I: Bus=0018 Vendor=0000 Product=0000 Version=0000\nN: Name=\"sec_touchpad\"\nP: Phys=sec_touchpad\n\
        S: Sysfs=/devices/platform/soc/ac0000.qcom,qupv3_1_geni_se/a90000.spi/spi_master/spi1/spi1.0/input/input9\nU: Uniq=\n\
        H: Handlers=event9 sec_key_notifier cpufreq sec_tsp_dumpkey kgsl sde \nB: PROP=1\nB: EV=2b\nB: SW=0\n\n";

    /// The script's touchscreen lookup run by the PC's sh and awk on DEVICES (or a variant): what it leaves in tdev.
    fn touch_lookup(devices: &str) -> String {
        let d = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32 }, 900, true, "ab12", Some(&d));
        let (from, to) = (s.find("dev_of() {\n").unwrap(), s.find("if [ -n \"$tdev\" ] && [ -c /dev/input/$tdev ]; then").unwrap());
        let dir = std::env::temp_dir().join(format!("oneios-vmboot-touch-{}-{}", std::process::id(), devices.len()));
        fs::create_dir_all(&dir).unwrap();
        let file = dir.join("devices");
        fs::write(&file, devices).unwrap();
        let script = s[from..to].replace("/proc/bus/input/devices", file.to_str().unwrap()) + "echo \"tdev=$tdev\"\n";
        let out = std::process::Command::new("sh").arg("-c").arg(&script).output().unwrap();
        fs::remove_dir_all(&dir).ok();
        String::from_utf8_lossy(&out.stdout).trim().to_string()
    }

    #[test]
    fn the_touchscreen_is_found_by_name_and_never_a_side_key() {
        let d = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        assert!(d.input && !parse_disks(SCREEN).unwrap().input);
        assert!(parse_disks(&format!("{SCREEN}input mouse\n")).is_err());
        assert!(parse_disks(&format!("{DISKS}input touch\n")).is_err());           // the input bus comes with the screen
        assert_eq!(touch_lookup(DEVICES), "tdev=event8");
        // a touchscreen that is not direct, none at all, one on a side key's device: no touch
        assert_eq!(touch_lookup(&DEVICES.replace("B: PROP=2\n", "B: PROP=1\n")), "tdev=");
        assert_eq!(touch_lookup(&DEVICES.replace("sec_touchscreen", "other_ts")), "tdev=");
        let shared = DEVICES.replace("Handlers=event8 sec_key", "Handlers=event4 sec_key");
        assert_eq!(touch_lookup(&shared), "touch: event4 is a side key's device: not given to the VM\ntdev=");
        // the script: the lookup, then the forwarder listening, before crosvm; crosvm's multi-touch device on its socket
        // (no glob expansion of the brackets); the way back after the bridge; both ended after the VM
        let s = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32 }, 900, true, "ab12", Some(&d));
        let lookup = s.find("tdev=$(dev_of sec_touchscreen 2)").unwrap();
        let forward = s.find("$D/displayctl touch --evdev /dev/input/$tdev --socket $D/touch.sock </dev/null >>$D/touch.txt 2>&1 &").unwrap();
        let crosvm = s.find("$C --log-level debug run").unwrap();
        assert!(lookup < forward && forward < crosvm);
        assert!(s.contains("--socket $D/crosvm.sock${tsock:+ --input multi-touch[path=$tsock,width=4095,height=4095,name=oneios-touch]} --serial"));
        assert!(s[..crosvm].ends_with("set -f\n") && s[crosvm..].contains("pid=$!\nset +f\n"));
        assert_eq!(s.matches("--evdev").count(), 1);                                // the forwarder's, not crosvm's
        assert!(s.contains("if [ -S $D/touch.sock ]; then\n    tsock=$D/touch.sock;"));
        let ended = s.find("if [ -n \"$forward\" ]; then\n").unwrap();
        assert!(s.find("wait $pid; rc=$?").unwrap() < ended && s[ended..].contains("kill $forward 2>/dev/null; wait $forward"));
        let escape = s.find("( getevent -l /dev/input/$vd 2>/dev/null").unwrap();
        assert!(s.find("TMPDIR=$D $D/displayctl pool").unwrap() < escape && escape < s.find("while [ $t -lt 900 ]").unwrap());
        assert!(s.contains("[ $((now - down)) -ge 3 ]; then echo \"touch: volume down held $((now - down)) s: the VM stops\"; touch $D/stop.request"));
        assert!(s.find("thp_restore\n").unwrap() < s.find("if [ -n \"$escape\" ]; then kill $escape").unwrap());
        assert!(!s.contains("EVIOCGRAB") && !s.contains("--evdev /dev/input/event"));
        // without the input bus: no lookup, and the empty tdev puts nothing on crosvm's line
        let plain = boot_script(&Label { ram: 2880, cpus: 1, swiotlb: 32 }, 900, true, "ab12", Some(&parse_disks(SCREEN).unwrap()));
        assert!(!plain.contains("dev_of") && !plain.contains("getevent") && plain.contains("tdev=\ntsock=\n"));
        assert!(!plain.contains("displayctl touch"));
        // the test swipes only when asked, on the forwarder's line
        let swiped = boot_script_with(&Label { ram: 2880, cpus: 1, swiotlb: 32 }, 900, true, "ab12", Some(&d), true);
        assert!(swiped.contains("--socket $D/touch.sock --swipe-at 200 --swipe-every 60 </dev/null") && !s.contains("--swipe-at"));
    }

    #[test]
    fn the_boot_parses_as_posix_sh() {
        let d = parse_disks(DISKS).unwrap();
        let screen = parse_disks(SCREEN).unwrap();
        let l = Label { ram: 3072, cpus: 1, swiotlb: 8 };
        let l32 = Label { ram: 2880, cpus: 1, swiotlb: 32 };
        let touch = parse_disks(&format!("{SCREEN}input touch\n")).unwrap();
        for (n, script) in [boot_script(&l, 600, true, "ab12", Some(&d)), boot_script(&l, 60, false, "ab12", None), boot_script(&l32, 900, true, "ab12", Some(&screen)),
                            boot_script(&l32, 900, true, "ab12", Some(&touch))]
            .iter()
            .enumerate()
        {
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
