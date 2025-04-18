// Copyright 2023 Richard Hughes <richard@hughsie.com>
// SPDX-License-Identifier: LGPL-2.1-or-later

#[derive(ToString)]
#[repr(u32le)]
enum FuTpmEventlogItemKind {
    EV_PREBOOT_CERT = 0x00000000,
    EV_POST_CODE = 0x00000001,
    EV_NO_ACTION = 0x00000003,
    EV_SEPARATOR = 0x00000004,
    EV_ACTION = 0x00000005,
    EV_EVENT_TAG = 0x00000006,
    EV_S_CRTM_CONTENTS = 0x00000007,
    EV_S_CRTM_VERSION = 0x00000008,
    EV_CPU_MICROCODE = 0x00000009,
    EV_PLATFORM_CONFIG_FLAGS = 0x0000000a,
    EV_TABLE_OF_DEVICES = 0x0000000b,
    EV_COMPACT_HASH = 0x0000000c,
    EV_NONHOST_CODE = 0x0000000f,
    EV_NONHOST_CONFIG = 0x00000010,
    EV_NONHOST_INFO = 0x00000011,
    EV_OMIT_BOOT_DEVICE_EVENTS = 0x00000012,
    EV_EFI_EVENT_BASE = 0x80000000,
    EV_EFI_VARIABLE_DRIVER_CONFIG = 0x80000001,
    EV_EFI_VARIABLE_BOOT = 0x80000002,
    EV_EFI_BOOT_SERVICES_APPLICATION = 0x80000003,
    EV_EFI_BOOT_SERVICES_DRIVER = 0x80000004,
    EV_EFI_RUNTIME_SERVICES_DRIVER = 0x80000005,
    EV_EFI_GPT_EVENT = 0x80000006,
    EV_EFI_ACTION = 0x80000007,
    EV_EFI_PLATFORM_FIRMWARE_BLOB = 0x80000008,
    EV_EFI_HANDOFF_TABLES = 0x80000009,
    EV_EFI_HCRTM_EVENT = 0x80000010,
    EV_EFI_VARIABLE_AUTHORITY = 0x800000e0,
}

#[derive(Parse)]
#[repr(C, packed)]
struct FuStructTpmEventLog2 {
    pcr: u32le,
    type: FuTpmEventlogItemKind,
    digest_count: u32le,
}

#[derive(ParseBytes, Default)]
#[repr(C, packed)]
struct FuStructTpmEfiStartupLocalityEvent {
    signature: [char; 16] == "StartupLocality",
    locality: u8,    // from which TPM2_Startup() was issued -- which is the initial value of PCR0
}
