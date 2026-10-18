/*
 * \brief   Platform interface implementation - x86_64 specific
 * \author  Alexander Boettcher
 * \date    2017-07-05
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* base includes */
#include <base/internal/crt0.h>

/* core includes */
#include <boot_modules.h>
#include <platform.h>

#include <thread_sel4.h>
#include "arch_kernel_object.h"

using Platform = Core::Platform;

using Core::Range_allocator;

seL4_Word Core::Untyped_memory::smallest_page_type() { return seL4_X86_4K; }


/**
 * Sponge: pick the physical-memory allocator backing a child-PD CNode.
 *
 * CNodes whose backing exceeds one page (size_log2 > 7 on 64-bit, i.e. the
 * 2nd-level 65536-slot-geometry CNodes) are backed by the 16 KiB untyped
 * pool that already exists for VCPUs. Smaller CNodes use the caller's
 * default (4 KiB RAM) allocator, preserving upstream behaviour byte-for-byte.
 */
Range_allocator &Core::Untyped_memory::cnode_backing_alloc(uint8_t cnode_size_log2,
                                                           Range_allocator &default_alloc)
{
	addr_t const backing_log2 = Cnode_kobj::SIZE_LOG2 + cnode_size_log2;

	if (backing_log2 > PAGE_SIZE_LOG2)
		return phys_alloc_16k();

	return default_alloc;
}


void Platform::init_sel4_ipc_buffer()
{
	/*
	 * Setup tls pointer such, that it points to the (kernel created) core
	 * main thread IPC buffer. The fs register is used in seL4_GetIPCBuffer().
	 */
	seL4_BootInfo const &bi = sel4_boot_info();
	seL4_SetTLSBase((unsigned long)&bi.ipcBuffer);
}


long Platform::_unmap_page_frame(Cap_sel const &sel) {
	return seL4_X86_Page_Unmap(sel.value()); }


void Platform::_init_core_page_table_registry()
{
	seL4_BootInfo const &bi = sel4_boot_info();

	addr_t virt_addr = (addr_t)(&_prog_img_beg);
	unsigned sel     = (unsigned)bi.userImagePaging.start;

	/* we don't know the physical location of some objects XXX */
	enum { XXX_PHYS_UNKNOWN = ~0UL };

	/*
	 * Register initial pdpt and page directory
	 */
	if (_core_page_table_registry.insert_page_level3(virt_addr, Cap_sel(sel++),
	                                                 XXX_PHYS_UNKNOWN,
	                                                 PAGE_PDPT_LOG2_SIZE).failed())
		error(__func__, ":", __LINE__, " page table allocation failed");

	if (_core_page_table_registry.insert_page_directory(virt_addr,
	                                                    Cap_sel(sel++),
	                                                    XXX_PHYS_UNKNOWN,
	                                                    PAGE_DIR_LOG2_SIZE).failed())
		error(__func__, ":", __LINE__, " page table allocation failed");

	/*
	 * Register initial page tables
	 */
	for (; sel < bi.userImagePaging.end; sel++) {
		if (_core_page_table_registry.insert_page_table(virt_addr, Cap_sel(sel),
		                                                XXX_PHYS_UNKNOWN,
		                                                PAGE_TABLE_LOG2_SIZE).failed())
			error(__func__, ":", __LINE__, " page table allocation failed");

		virt_addr += 512 * PAGE_SIZE;
	}

	/* initialize 16k memory allocator */
	phys_alloc_16k(&core_mem_alloc());

	/*
	 * Reserve 16 KiB memory for three consumers of this pool:
	 *   - VCPUs (upstream):                  MAX_VCPU_COUNT * Vcpu_kobj
	 *   - large child-PD main-CSpace CNodes: MAX_CNODE_PD_COUNT PDs, each
	 *                                        backing (1 << CSPACE_SIZE_LOG2_1ST)
	 *                                        2nd-level CNodes of 16 KiB.
	 *   - vm_space leaf CNodes (Sponge C2):  lazily constructed 512-entry leaves
	 *                                        (16 KiB each), sized for the realistic
	 *                                        concurrent working set of heavy PDs
	 *                                        (falkon ~200 leaves, rom_pkg ~256).
	 *
	 * The CNode term is bounded to a fixed, realistic working set (NOT
	 * proportional to total RAM). An earlier proportional carve (~247 MiB)
	 * removed too much low RAM from the general pool and disturbed the
	 * platform-driver/ahci boot dependency chain; this fixed bound keeps
	 * the reservation modest while covering all canary scenarios.
	 */
	enum {
		MAX_VCPU_COUNT    = 16,
		MAX_CNODE_PD_COUNT = 64,
		/*
		 * Sponge: the 16 KiB pool reservation stays at the proven
		 * per-PD magnitude even though CSPACE_SIZE_LOG2_1ST is now 7
		 * (128 possible 2nd-level CNodes per PD). Realistic boots
		 * construct only a handful of CNodes per PD (the heavy
		 * storage PDs reach ~70); doubling the eager carve to
		 * 128 MiB regressed the 4 GiB desktop before (the row-14
		 * 256 MiB lesson), so the reservation is decoupled from the
		 * geometry and pinned at 64/PD.
		 */
		SECOND_LEVEL_CNODES_PER_PD = 64,
		MAX_VM_LEAF_COUNT = 2048,
		/*
		 * Sponge (row 14): NO eager reservation for the high-phys
		 * CNode backing (2^23 slots x 32 B CTE = 256 MiB). The CNode
		 * is constructed lazily on the first high-phys IO_MEM request
		 * (Platform::construct_high_phys_cnode), which allocates the
		 * backing from the 16 KiB pool at that time — early in boot,
		 * when contiguous RAM is plentiful. An eager 256 MiB carve
		 * here regressed the AHCI desktop scenario on 4 GiB VMs
		 * (alpha-probe: sponge_pkgd did not answer install hello) —
		 * the same reservation-regression class this block's comment
		 * above already documents. Lazy failure mode is clean: the
		 * IO_MEM session fails with "not available", no crash.
		 */
	};

	addr_t const max_pd_mem =
		MAX_VCPU_COUNT            * (1UL << Vcpu_kobj::SIZE_LOG2) +
		MAX_CNODE_PD_COUNT        * (SECOND_LEVEL_CNODES_PER_PD << Vcpu_kobj::SIZE_LOG2) +
		MAX_VM_LEAF_COUNT         * (1UL << Vcpu_kobj::SIZE_LOG2);

	_initial_untyped_pool.turn_into_untyped_object(Core_cspace::TOP_CNODE_UNTYPED_16K,
		[&] (Initial_untyped_pool::Range const &, addr_t const phys, addr_t const size, bool const device_memory) -> bool {

			if (device_memory)
				return false;

			if (_unused_phys_alloc.remove_range(phys, size).failed()) {
				warning("unable to register range as RAM: ", Hex_range(phys, size));
				return false;
			}

			if (phys_alloc_16k().add_range(phys, size).failed()) {
				if (_unused_phys_alloc.add_range(phys, size).failed())
					warning("unable to remove range as RAM: ", Hex_range(phys, size));
				warning("unable to register range as RAM: ", Hex_range(phys, size));
				return false;
			}

			return true;
		},
		[&] (Initial_untyped_pool::Range const &, addr_t const phys, addr_t const size, bool const device_memory) {
			if (device_memory)
				return;

			if (phys_alloc_16k()  .remove_range(phys, size).failed() ||
			    _unused_phys_alloc.add_range   (phys, size).failed())
				warning("unable to re-add phys RAM: ", Hex_range(phys, size));
		},
		Vcpu_kobj::SIZE_LOG2, max_pd_mem);

	log(":phys_mem_16k:     ",  phys_alloc_16k());
}


void Platform::_init_io_ports()
{
	enum { PORTS = 0x10000, PORT_FIRST = 0, PORT_LAST = PORTS - 1 };

	/* I/O port allocator (only meaningful for x86) */
	if (_io_port_alloc.add_range(PORT_FIRST, PORTS).failed())
		warning("unable to register default I/O-port range");

	/* create I/O port capability used by io_port_session_support.cc */
	auto const root   = _core_cnode.sel().value();
	auto const index  = Core_cspace::io_port_sel();
	auto const depth  = CONFIG_ROOT_CNODE_SIZE_BITS;

	auto const result = seL4_X86_IOPortControl_Issue(seL4_CapIOPortControl,
	                                                 PORT_FIRST, PORT_LAST,
	                                                 root, index, depth);
	if (result != 0)
		error("IO Port access not available");
}
