/*
 *  linux/arch/i386/kernel/head32.c -- prepare to run common code
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *  Copyright (C) 2007 Eric Biederman <ebiederm@xmission.com>
 */

#include <linux/init.h>
#include <linux/start_kernel.h>
#include <linux/mm.h>
#include <linux/memblock.h>

#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/e820.h>
#include <asm/page.h>
#include <asm/trampoline.h>
#include <asm/apic.h>
#include <asm/io_apic.h>
#include <asm/bios_ebda.h>
#include <asm/tlbflush.h>

static void __init i386_default_early_setup(void)
{
	
	x86_init.resources.reserve_resources = i386_reserve_resources;
	x86_init.mpparse.setup_ioapic_ids = setup_ioapic_ids_from_mpc;

	reserve_ebda_region();
}

void __init i386_start_kernel(void)
{
	memblock_reserve(__pa_symbol(&_text),
			 __pa_symbol(&__bss_stop) - __pa_symbol(&_text));

#ifdef CONFIG_BLK_DEV_INITRD
	
	if (boot_params.hdr.type_of_loader && boot_params.hdr.ramdisk_image) {
		
		u64 ramdisk_image = boot_params.hdr.ramdisk_image;
		u64 ramdisk_size  = boot_params.hdr.ramdisk_size;
		u64 ramdisk_end   = PAGE_ALIGN(ramdisk_image + ramdisk_size);
		memblock_reserve(ramdisk_image, ramdisk_end - ramdisk_image);
	}
#endif

	
	switch (boot_params.hdr.hardware_subarch) {
	case X86_SUBARCH_MRST:
		x86_mrst_early_setup();
		break;
	case X86_SUBARCH_CE4100:
		x86_ce4100_early_setup();
		break;
	default:
		i386_default_early_setup();
		break;
	}


	start_kernel();
}
