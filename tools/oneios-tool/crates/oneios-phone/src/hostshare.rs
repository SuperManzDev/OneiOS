//! `oneios-tool phone hostshare status | load | unload`: Droid-VM's gunyah_host_share (GPL-2.0-or-later, built by
//! scripts/hugepage/build-share.sh) on the phone. It adds `/dev/gunyah_share`, through which Droid-VM's crosvm SHAREs
//! a running VM's RAM with it as a Gunyah memparcel instead of lending it: the pseudo-unprotected VM, whose RAM Android
//! can still read, so that the phone's GPU can work on iOS's own memory (docs/native.md "The shared-RAM VM").
//! - `status`: its parameters (named files) and `/dev/gunyah_share`, read only.
//! - `load`: the preflight, then the .ko pushed, its SHA-256 checked, a `sync` and `insmod` with the layout the
//!   preflight read. The module reads three fields of the kernel's private `struct gunyah_vm` at fixed offsets, builds
//!   the resource manager's parcel structs itself, and calls six kernel functions it finds by name (a call through a
//!   pointer whose type differs from the function's is a kCFI panic). So the preflight checks all of it against this
//!   phone's kernel: its BTF (`/sys/kernel/btf/vmlinux`, read on the PC with pahole and pfunct) for the offsets, the
//!   structs' layout and the functions' prototypes, and `/proc/kallsyms` (a named file) for the functions themselves.
//!   Any difference loads nothing.
//! - `unload`: rmmod, when it holds no parcel.
use crate::adb::Adb;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::copy;
use std::path::Path;
use std::process::Command;

/// The module's folder on the phone, the .ko there, its parameters and its device.
pub const DIR: &str = "/data/local/tmp/oneios-hostshare";
pub const KO: &str = "/data/local/tmp/oneios-hostshare/gunyah_host_share.ko";
pub const PARAMS: &str = "/sys/module/gunyah_host_share/parameters";
pub const DEV: &str = "/dev/gunyah_share";

/// The functions the module calls, as their types (no parameter names): the required ones, then `__fput_sync`, which
/// it uses when it is there.
pub const FUNCTIONS: [(&str, &str, bool); 6] = [
    ("gunyah_rm_mem_share", "int gunyah_rm_mem_share(struct gunyah_rm *, struct gunyah_rm_mem_parcel *)", true),
    ("gunyah_rm_mem_reclaim", "int gunyah_rm_mem_reclaim(struct gunyah_rm *, struct gunyah_rm_mem_parcel *)", true),
    ("gunyah_rm_get_vmid", "int gunyah_rm_get_vmid(struct gunyah_rm *, u16 *)", true),
    ("gunyah_rm_get", "struct device * gunyah_rm_get(struct gunyah_rm *)", true),
    ("gunyah_rm_put", "void gunyah_rm_put(struct gunyah_rm *)", true),
    ("__fput_sync", "void __fput_sync(struct file *)", false),
];

/// The parcel structs the module builds itself (its own copies of the driver's private types): each one's size and
/// the offset of each member, as `gunyah_share_mod.c` lays them out.
pub const PARCEL: [(&str, usize, &[(&str, usize)]); 3] = [
    (
        "gunyah_rm_mem_parcel",
        48,
        &[("mem_type", 0), ("label", 4), ("n_acl_entries", 8), ("acl_entries", 16), ("n_mem_entries", 24), ("mem_entries", 32), ("mem_handle", 40)],
    ),
    ("gunyah_rm_mem_acl_entry", 4, &[("vmid", 0), ("perms", 2), ("reserved", 3)]),
    ("gunyah_rm_mem_entry", 16, &[("phys_addr", 0), ("size", 8)]),
];

/// One struct as pahole prints it: its size and its members' (name, offset), nested ones included in order.
#[derive(Debug, Default, PartialEq)]
pub struct Layout {
    pub size: Option<usize>,
    pub members: Vec<(String, usize)>,
}

impl Layout {
    pub fn offset(&self, name: &str) -> Option<usize> {
        self.members.iter().find(|(n, _)| n == name).map(|(_, o)| *o)
    }
}

/// pahole's `-C A,B,...` output, struct by struct. A member line is `TYPE NAME; /* OFFSET SIZE */` (a bit field's
/// offset is `BYTE: BIT`); the size line `/* size: N, ... */`. Nested anonymous structs' members come with their
/// offsets in the outer struct, which is what a lookup by a top-level name needs.
pub fn layouts(pahole: &str) -> BTreeMap<String, Layout> {
    let mut all = BTreeMap::new();
    let mut current: Option<(String, Layout)> = None;
    let mut depth = 0usize;
    for line in pahole.lines() {
        let t = line.trim();
        if current.is_none() {
            if let Some(rest) = t.strip_prefix("struct ") {
                if let Some(name) = rest.strip_suffix(" {") {
                    current = Some((name.trim().to_string(), Layout::default()));
                    depth = 1;
                }
            }
            continue;
        }
        let (_, layout) = current.as_mut().unwrap();
        if let Some(rest) = t.strip_prefix("/* size: ") {
            if depth == 1 {
                layout.size = rest.split(',').next().and_then(|n| n.trim().parse().ok());
            }
            continue;
        }
        if t.ends_with('{') {
            depth += 1;
            continue;
        }
        if t.starts_with('}') {
            depth -= 1;
            if depth == 0 {
                let (name, layout) = current.take().unwrap();
                all.insert(name, layout);
            }
            // `} NAME; /* OFFSET SIZE */`: the nested struct itself, a member too
        }
        let (Some(semi), Some(open)) = (t.find(';'), t.find("/*")) else { continue };
        if semi > open {
            continue;
        }
        let decl = t[..semi].trim();
        let name = decl.rsplit(|c: char| c.is_whitespace() || c == '*' || c == '}').next().unwrap_or("");
        // an array's brackets, a bit field's width
        let name = name.split(['[', ':']).next().unwrap_or("").trim();
        let offset = t[open + 2..].split_whitespace().next().unwrap_or("").trim_end_matches(':');
        if let (false, Ok(offset), Some((_, layout))) = (name.is_empty(), offset.parse::<usize>(), current.as_mut()) {
            layout.members.push((name.to_string(), offset));
        }
    }
    all
}

/// A prototype as pfunct prints it (`int f(struct a * x, u16 * y);`) with the parameter names dropped:
/// `int f(struct a *, u16 *)`.
pub fn prototype_types(line: &str) -> Option<String> {
    let line = line.trim().trim_end_matches(';');
    let (head, rest) = line.split_once('(')?;
    let params = rest.strip_suffix(')')?;
    let mut out = Vec::new();
    for p in params.split(',') {
        let p = p.trim();
        if p == "void" || p.is_empty() || p == "..." {
            out.push(p.to_string());
            continue;
        }
        // the last token is the name unless the parameter is only a type
        let cut = p.rfind(|c: char| c.is_whitespace() || c == '*').map(|i| i + 1).unwrap_or(0);
        let ty = p[..cut].trim_end();
        out.push(if ty.is_empty() { p.to_string() } else { ty.to_string() });
    }
    Some(format!("{}({})", head.trim(), out.join(", ")))
}

/// The preflight's verdict on the BTF: the three `struct gunyah_vm` offsets for insmod, or what differs.
pub fn check_layouts(pahole: &str) -> Result<(usize, usize, usize)> {
    let all = layouts(pahole);
    let mut bad = Vec::new();
    for (name, size, members) in PARCEL {
        let Some(l) = all.get(name) else {
            bad.push(format!("no struct {name}"));
            continue;
        };
        if l.size != Some(size) {
            bad.push(format!("struct {name}: {:?} bytes, not {size}", l.size));
        }
        for (m, at) in members {
            if l.offset(m) != Some(*at) {
                bad.push(format!("{name}.{m} at {:?}, not {at}", l.offset(m)));
            }
        }
    }
    let vm = all.get("gunyah_vm");
    let get = |m: &str| vm.and_then(|l| l.offset(m));
    let (vmid, rm, status) = (get("vmid"), get("rm"), get("vm_status"));
    if vm.is_none() {
        bad.push("no struct gunyah_vm".into());
    } else if vmid.is_none() || rm.is_none() || status.is_none() {
        bad.push(format!("struct gunyah_vm: vmid {vmid:?}, rm {rm:?}, vm_status {status:?}"));
    }
    if !bad.is_empty() {
        bail!("the phone's kernel does not have the layout gunyah_host_share builds on: {}", bad.join("; "));
    }
    Ok((vmid.unwrap(), rm.unwrap(), status.unwrap()))
}

/// The insmod line, with the offsets the BTF gave.
pub fn insmod_line(vmid: usize, rm: usize, status: usize) -> String {
    format!("insmod {KO} vmid_off={vmid} rm_off={rm} vm_status_off={status}")
}

/// `status`'s read on the phone.
pub fn status_script() -> String {
    format!(
        "if [ -d {PARAMS} ]; then echo '=== loaded'; for p in vmid_off rm_off vm_status_off outstanding; do echo \"$p: $(cat {PARAMS}/$p)\"; done\n\
         else echo '=== not loaded'; fi\nls -l {DEV} 2>&1\n\
         echo '=== kernel lines'\ndmesg 2>/dev/null | grep -E 'gunyah_share|GH-SHIM|GUNYAH-SHARE' | tail -n 20\n"
    )
}

/// `phone hostshare status`.
pub fn status(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    print!("{}", adb.root_ok(&status_script(), "the module's status")?);
    Ok(())
}

/// `phone hostshare load`.
pub fn load(adb: &Adb, ko: &str, out: &str) -> Result<()> {
    let data = fs::read(ko).with_context(|| format!("cannot read {ko} (scripts/hugepage/build-share.sh makes it)"))?;
    let sha = format!("{:x}", Sha256::digest(&data));
    adb.need_root()?;
    adb.refuse_in_session()?;
    let names: Vec<&str> = FUNCTIONS.iter().map(|(n, _, _)| *n).collect();
    let r = adb.root_ok(
        &format!(
            "[ -d {PARAMS} ] && echo loaded || echo absent\nuname -r\n\
             grep -E ' [Tt] ({})$' /proc/kallsyms | while read -r a t n; do echo \"sym $n\"; done\n",
            names.join("|")
        ),
        "the module check",
    )?;
    if r.lines().next() == Some("loaded") {
        bail!("gunyah_host_share is loaded already: `phone hostshare status`");
    }
    let have: Vec<&str> = r.lines().filter_map(|l| l.strip_prefix("sym ")).collect();
    let missing: Vec<&str> = FUNCTIONS.iter().filter(|(n, _, need)| *need && !have.contains(n)).map(|(n, _, _)| *n).collect();
    if !missing.is_empty() {
        bail!("the phone's kernel has no {} (in /proc/kallsyms): not loading gunyah_host_share", missing.join(", "));
    }
    // the BTF, on the PC
    fs::create_dir_all(out).with_context(|| format!("cannot make {out}"))?;
    let btf = Path::new(out).join("btf.vmlinux");
    let mut child = adb.exec_out("su -c 'cat /sys/kernel/btf/vmlinux'")?;
    let mut file = fs::File::create(&btf)?;
    copy(child.stdout.as_mut().context("no output from adb")?, &mut file)?;
    child.wait()?;
    let btf_s = btf.to_str().context("path")?;
    let structs = ["gunyah_vm", "gunyah_rm_mem_parcel", "gunyah_rm_mem_acl_entry", "gunyah_rm_mem_entry"].join(",");
    let p = Command::new("pahole").args(["-F", "btf", "-C", &structs, btf_s]).output().context("cannot run pahole (dwarves)")?;
    let pahole = String::from_utf8_lossy(&p.stdout).to_string();
    fs::write(Path::new(out).join("pahole.txt"), &pahole)?;
    let (vmid, rm, st) = check_layouts(&pahole)?;
    let mut said = String::new();
    for (name, want, need) in FUNCTIONS {
        if !have.contains(&name) {
            said += &format!("{name}: not in kallsyms (the module goes without it)\n");
            continue;
        }
        let f = Command::new("pfunct").args(["-F", "btf", "-P", "-f", name, btf_s]).output().context("cannot run pfunct (dwarves)")?;
        let line = String::from_utf8_lossy(&f.stdout).lines().next().unwrap_or("").to_string();
        let got = prototype_types(&line).unwrap_or_default();
        said += &format!("{name}: {got}\n");
        if got != want && (need || !got.is_empty()) {
            fs::write(Path::new(out).join("functions.txt"), &said)?;
            bail!("{name} is \"{got}\" in the phone's kernel, not \"{want}\": a call through the module's pointer would be a kCFI panic. Not loading");
        }
    }
    fs::write(Path::new(out).join("functions.txt"), &said)?;
    println!("preflight (kernel {}): struct gunyah_vm vmid {vmid}, rm {rm}, vm_status {st}; the parcel structs and the six functions as the module has them", r.lines().nth(1).unwrap_or("?"));
    adb.root_ok(&format!("mkdir -p {DIR} && chmod 777 {DIR}\n"), "the module's folder")?;
    adb.push(ko, KO)?;
    let line = insmod_line(vmid, rm, st);
    println!("load: {} bytes, sha256 {sha}; {line}", data.len());
    let script = format!(
        "got=$(sha256sum {KO} | cut -d' ' -f1); [ \"$got\" = \"{sha}\" ] || {{ echo \"sha256 $got, not {sha}\"; exit 1; }}\n\
         sync\n{line}; echo \"insmod exit $?\"\nls -l {DEV} 2>&1\ndmesg 2>/dev/null | grep 'gunyah_share:' | tail -n 5\n"
    );
    let r = adb.root(&script)?;
    print!("{}{}", r.out, r.err);
    fs::write(Path::new(out).join("load.txt"), format!("{line}\n{}{}", r.out, r.err))?;
    if !r.out.contains("insmod exit 0") {
        bail!("the module did not load");
    }
    if r.out.contains("No such file") {
        bail!("the module loaded but {DEV} is not there");
    }
    Ok(())
}

/// `phone hostshare unload`.
pub fn unload(adb: &Adb) -> Result<()> {
    adb.need_root()?;
    let st = adb.root_ok(&format!("cat {PARAMS}/outstanding 2>/dev/null || echo absent\n"), "the module's state")?;
    if st.contains("absent") {
        println!("unload: gunyah_host_share is not loaded");
        return Ok(());
    }
    if !st.contains("total=0 ") {
        bail!("the module holds parcels: {}", st.trim());
    }
    let r = adb.root("rmmod gunyah_host_share; echo \"rmmod exit $?\"\n")?;
    print!("{}{}", r.out, r.err);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const PAHOLE: &str = "struct gunyah_vm {
	u16                        vmid;                 /*     0     2 */

	/* XXX 6 bytes hole, try to pack */

	struct maple_tree          mm;                   /*     8    16 */
	struct gunyah_rm *         rm;                   /*   504     8 */
	enum gunyah_rm_vm_status   vm_status;            /*   536     4 */
	struct {
		struct gunyah_vm_dtb_config config;      /*   816    16 */
		u64                parcel_start;         /*   832     8 */
	} dtb;                                           /*   816    32 */
	unsigned int               flag:1;               /*   848: 0  4 */
	/* size: 992, cachelines: 16, members: 29 */
};

struct gunyah_rm_mem_parcel {
	enum gunyah_rm_mem_type    mem_type;             /*     0     4 */
	u32                        label;                /*     4     4 */
	size_t                     n_acl_entries;        /*     8     8 */
	struct gunyah_rm_mem_acl_entry * acl_entries;    /*    16     8 */
	size_t                     n_mem_entries;        /*    24     8 */
	struct gunyah_rm_mem_entry * mem_entries;        /*    32     8 */
	u32                        mem_handle;           /*    40     4 */

	/* size: 48, cachelines: 1, members: 7 */
	/* padding: 4 */
};

struct gunyah_rm_mem_acl_entry {
	__le16                     vmid;                 /*     0     2 */
	u8                         perms;                /*     2     1 */
	u8                         reserved;             /*     3     1 */

	/* size: 4, cachelines: 1, members: 3 */
};

struct gunyah_rm_mem_entry {
	__le64                     phys_addr;            /*     0     8 */
	__le64                     size;                 /*     8     8 */

	/* size: 16, cachelines: 1, members: 2 */
};
";

    #[test]
    fn the_layouts_are_read_as_pahole_prints_them() {
        let all = layouts(PAHOLE);
        let vm = &all["gunyah_vm"];
        assert_eq!(vm.size, Some(992));
        assert_eq!((vm.offset("vmid"), vm.offset("rm"), vm.offset("vm_status")), (Some(0), Some(504), Some(536)));
        // a nested struct's members and the struct itself, a bit field's byte offset
        assert_eq!((vm.offset("parcel_start"), vm.offset("dtb"), vm.offset("flag")), (Some(832), Some(816), Some(848)));
        assert_eq!(all["gunyah_rm_mem_parcel"].offset("mem_handle"), Some(40));
        assert_eq!(all["gunyah_rm_mem_entry"].size, Some(16));
        assert_eq!(check_layouts(PAHOLE).unwrap(), (0, 504, 536));
        assert_eq!(insmod_line(0, 504, 536), format!("insmod {KO} vmid_off=0 rm_off=504 vm_status_off=536"));
    }

    #[test]
    fn a_layout_that_moved_loads_nothing() {
        let moved = PAHOLE.replace("mem_handle;           /*    40", "mem_handle;           /*    44");
        let e = check_layouts(&moved).unwrap_err().to_string();
        assert!(e.contains("gunyah_rm_mem_parcel.mem_handle at Some(44), not 40"), "{e}");
        let gone = PAHOLE.replace("vm_status;", "status;");
        assert!(check_layouts(&gone).unwrap_err().to_string().contains("vm_status None"));
        assert!(check_layouts("").unwrap_err().to_string().contains("no struct gunyah_vm"));
    }

    #[test]
    fn prototypes_are_compared_by_their_types() {
        assert_eq!(
            prototype_types("int gunyah_rm_mem_share(struct gunyah_rm * rm, struct gunyah_rm_mem_parcel * p);").unwrap(),
            FUNCTIONS[0].1
        );
        assert_eq!(prototype_types("int gunyah_rm_get_vmid(struct gunyah_rm * rm, u16 * vmid);").unwrap(), FUNCTIONS[2].1);
        assert_eq!(prototype_types("struct device * gunyah_rm_get(struct gunyah_rm * rm);").unwrap(), FUNCTIONS[3].1);
        assert_eq!(prototype_types("void __fput_sync(struct file * file);").unwrap(), FUNCTIONS[5].1);
        // another type is another hash
        assert_ne!(prototype_types("int gunyah_rm_get_vmid(struct gunyah_rm * rm, u32 * vmid);").unwrap(), FUNCTIONS[2].1);
        assert_eq!(prototype_types("void f(void);").unwrap(), "void f(void)");
    }
}
