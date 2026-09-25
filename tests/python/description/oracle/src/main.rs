use std::collections::HashMap;
use std::env;
use std::fs;
use std::process::ExitCode;

use svd_parser::expand;
use svd_parser::svd::{Access, Device, ModifiedWriteValues, Peripheral, ReadAction, RegisterCluster};

fn access_str(a: Access) -> &'static str {
    match a {
        Access::ReadOnly => "read_only",
        Access::ReadWrite => "read_write",
        Access::ReadWriteOnce => "read_write_once",
        Access::WriteOnce => "write_once",
        Access::WriteOnly => "write_only",
    }
}

fn write_effect_str(m: Option<ModifiedWriteValues>) -> &'static str {
    use ModifiedWriteValues::*;
    match m {
        None => "modify",
        Some(OneToClear) => "one_to_clear",
        Some(OneToSet) => "one_to_set",
        Some(OneToToggle) => "one_to_toggle",
        Some(ZeroToClear) => "zero_to_clear",
        Some(ZeroToSet) => "zero_to_set",
        Some(ZeroToToggle) => "zero_to_toggle",
        Some(Clear) => "clear",
        Some(Set) => "set",
        Some(Modify) => "modify",
    }
}

fn read_effect_str(r: Option<ReadAction>) -> &'static str {
    use ReadAction::*;
    match r {
        None => "none",
        Some(Clear) => "clear",
        Some(Set) => "set",
        Some(Modify) => "modify",
        Some(ModifyExternal) => "modify_external",
    }
}

fn dump(device: &Device) {
    for p in &device.peripherals {
        let info = match p {
            Peripheral::Single(info) => info,
            Peripheral::Array(..) => panic!("expand() left an unexpanded peripheral array"),
        };

        let size = info
            .address_block
            .as_ref()
            .map(|blocks| blocks.iter().map(|b| b.offset + b.size).max().unwrap_or(0))
            .unwrap_or(0);
        println!("P {} {} {}", info.name, info.base_address, size);

        let registers: Vec<_> = match &info.registers {
            Some(regs) => regs
                .iter()
                .map(|rc| match rc {
                    RegisterCluster::Register(r) => match r {
                        svd_parser::svd::Register::Single(info) => info,
                        svd_parser::svd::Register::Array(..) => {
                            panic!("expand() left an unexpanded register array")
                        }
                    },
                    RegisterCluster::Cluster(_) => panic!("expand() left an unflattened cluster"),
                })
                .collect(),
            None => Vec::new(),
        };

        let mut anchors: HashMap<(u32, String), String> = HashMap::new();
        for r in &registers {
            let width = r.properties.size.unwrap_or(32);
            let access = r.properties.access.unwrap_or(Access::ReadWrite);
            let reset = r.properties.reset_value.unwrap_or(0);
            let mask = r.properties.reset_mask.unwrap_or(0xffff_ffff);

            let alternate_of = if let Some(alt) = &r.alternate_register {
                Some(alt.clone())
            } else if let Some(group) = &r.alternate_group {
                let key = (r.address_offset, group.clone());
                match anchors.get(&key) {
                    Some(anchor) => Some(anchor.clone()),
                    None => {
                        anchors.insert(key, r.name.clone());
                        None
                    }
                }
            } else {
                None
            };

            println!(
                "R {} {} {} {} {} {} {} {} {}",
                info.name,
                r.name,
                r.address_offset,
                width,
                access_str(access),
                reset,
                mask,
                r.fields.as_ref().map(|f| f.len()).unwrap_or(0),
                alternate_of.as_deref().unwrap_or("-"),
            );

            for f in r.fields.iter().flatten() {
                let field = match f {
                    svd_parser::svd::Field::Single(info) => info,
                    svd_parser::svd::Field::Array(..) => {
                        panic!("expand() left an unexpanded field array")
                    }
                };
                let facc = field.access.unwrap_or(access);
                println!(
                    "F {} {} {} {} {} {} {} {}",
                    info.name,
                    r.name,
                    field.name,
                    field.bit_range.lsb(),
                    field.bit_range.width,
                    access_str(facc),
                    write_effect_str(field.modified_write_values),
                    read_effect_str(field.read_action),
                );
            }
        }

        for irq in &info.interrupt {
            println!("I {} {}", irq.value, irq.name);
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} <svd file>", args[0]);
        return ExitCode::from(2);
    }

    let xml = match fs::read_to_string(&args[1]) {
        Ok(xml) => xml,
        Err(err) => {
            eprintln!("{}: {}", args[1], err);
            return ExitCode::FAILURE;
        }
    };

    let device = match svd_parser::parse(&xml) {
        Ok(device) => device,
        Err(err) => {
            eprintln!("{}: {}", args[1], err);
            return ExitCode::FAILURE;
        }
    };

    let mut device = match expand::expand(&device) {
        Ok(device) => device,
        Err(err) => {
            eprintln!("{}: {}", args[1], err);
            return ExitCode::FAILURE;
        }
    };
    expand::expand_properties(&mut device);

    dump(&device);
    ExitCode::SUCCESS
}
