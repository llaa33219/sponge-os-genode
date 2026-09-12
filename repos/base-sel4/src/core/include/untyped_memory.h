/*
 * \brief   Utilities for dealing with untyped memory
 * \author  Norman Feske
 * \author  Alexander Boettcher
 * \date    2015-05-06
 */

/*
 * Copyright (C) 2015-2025 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__INCLUDE__UNTYPED_MEMORY_H_
#define _CORE__INCLUDE__UNTYPED_MEMORY_H_

/* Genode includes */
#include <base/allocator.h>

/* core includes */
#include <util.h>
#include <cap_sel_alloc.h>
#include <core_cspace.h>

/* seL4 includes */
#include <sel4/sel4.h>

namespace Core { struct Untyped_memory; }

namespace Core {

/*
 * Sponge (row 14 v2): seL4's Untyped_Retype creates objects at the
 * untyped's free-offset watermark, never at a requested offset — so a
 * frame for phys X inside a multi-GiB untyped only exists after the
 * watermark has advanced to X. The watermark cannot be rewound; we
 * track it per untyped and fast-forward by retyping throwaway frames
 * (the memory stays consumed, which is exactly the skip we want for
 * MMIO space) into a scratch slot that is deleted again immediately.
 */
struct High_phys_watermark { seL4_Word service; Genode::addr_t next; };
inline High_phys_watermark high_phys_watermarks[8] = { };
inline unsigned            high_phys_watermark_count = 0;

enum { HIGH_PHYS_SCRATCH_SLOT = (1UL << Core_cspace::NUM_HIGH_PHYS_SEL_LOG2) - 1 };

inline Genode::addr_t &high_phys_watermark(seL4_Untyped service, Genode::addr_t base)
{
	for (unsigned i = 0; i < high_phys_watermark_count; i++)
		if (high_phys_watermarks[i].service == service)
			return high_phys_watermarks[i].next;
	unsigned const i = high_phys_watermark_count++;
	high_phys_watermarks[i].service = service;
	high_phys_watermarks[i].next    = base;
	return high_phys_watermarks[i].next;
}

inline bool high_phys_skip_to(seL4_Untyped service, Genode::addr_t base,
                              Genode::addr_t size_bits, Genode::addr_t &next,
                              Genode::addr_t target)
{
	Genode::addr_t const untyped_end = base + (1UL << size_bits);

	while (next < target) {
		Genode::addr_t const remaining = target - next;

		/*
		 * Advance with the largest VALID frame type that is aligned
		 * to 'next', fits 'remaining', and stays inside the untyped.
		 * Only 2 MiB (seL4_X86_LargePageObject) and 4 KiB (seL4_X86_4K)
		 * frames exist on this kernel — CONFIG_HUGE_PAGE is off, so
		 * seL4_X64_HugePageObject is the invalid sentinel 0xfffffffe
		 * and must not be requested (the 17ZD90N answered "Invalid
		 * object type").
		 */
		Genode::addr_t const large = 1UL << 21;
		Genode::addr_t      sz;
		seL4_Word           type;
		if ((next & (large - 1)) == 0 && large <= remaining
		    && next + large <= untyped_end) {
			sz   = large;
			type = seL4_X86_LargePageObject;
		} else {
			sz   = 1UL << 12 /* 4 KiB frame */;
			type = seL4_X86_4K;
		}
		if (sz > remaining || next + sz > untyped_end)
			return false;

		long const ret = seL4_Untyped_Retype(service, type, 0,
		                                     Core_cspace::top_cnode_sel(),
		                                     Core_cspace::TOP_CNODE_HIGH_PHYS_IDX,
		                                     Core_cspace::NUM_TOP_SEL_LOG2,
		                                     HIGH_PHYS_SCRATCH_SLOT, 1);
		if (ret != seL4_NoError)
			return false;

		/* the throwaway's memory stays consumed in the untyped */
		seL4_CNode_Delete(Core_cspace::high_phys_cnode_sel(),
		                  HIGH_PHYS_SCRATCH_SLOT,
		                  Core_cspace::NUM_HIGH_PHYS_SEL_LOG2);
		next += sz;
	}
	return true;
}

}

struct Core::Untyped_memory
{
	/*
	 * Object size of the large-CNode-backing untyped pool. On x86_64 this
	 * is the 16 KiB pool that also backs VCPUs (Vcpu_kobj::SIZE_LOG2 == 14).
	 * The large-backing branch is only reached when a child-PD CNode exceeds
	 * one page, which the configured CSPACE sizes permit only on x86_64;
	 * the value is inert on architectures where CNodes stay page-sized.
	 */
	enum { LARGE_BACKING_UNTYPED_LOG2 = 14 };

	static inline Allocator::Alloc_result alloc_pages(Range_allocator &phys,
	                                                  size_t const num_pages)
	{
		return phys.alloc_aligned(num_pages*PAGE_SIZE, AT_PAGE);
	}


	static inline Allocator::Alloc_result alloc_page(Range_allocator &phys)
	{
		return alloc_pages(phys, 1);
	}


	static inline void free_page(Range_allocator &phys_alloc, addr_t addr)
	{
		phys_alloc.free(reinterpret_cast<void *>(addr));
	}


	static inline void free_pages(Range_allocator &phys_alloc, addr_t addr,
	                              size_t const num_pages)
	{
		phys_alloc.free(reinterpret_cast<void *>(addr), num_pages * PAGE_SIZE);
	}


	/**
	 * Local utility solely used by 'untyped_sel' and 'frame_sel'
	 */
	static inline Cap_sel _core_local_sel(Core_cspace::Top_cnode_idx top_idx,
	                                      addr_t phys_addr,
	                                      addr_t size_log2 = PAGE_SIZE_LOG2)
	{
		unsigned const upper_bits = top_idx << Core_cspace::NUM_PHYS_SEL_LOG2;
		unsigned const mask       = (1ul << Core_cspace::NUM_PHYS_SEL_LOG2) - 1;
		unsigned const lower_bits = unsigned(phys_addr >> size_log2) & mask;

		return Cap_sel(upper_bits | lower_bits);
	}


	/*
	 * Sponge (row 14 v2): selector for a high-phys frame cap at a
	 * SEQUENTIALLY ALLOCATED slot of the high-phys CNode (top index
	 * TOP_CNODE_HIGH_PHYS_IDX). Slots are managed by the
	 * Core::high_phys_slot_* helpers (core_cspace.h) — no address
	 * math, so frames at any phys_addr work (the 17ZD90N's GOP
	 * framebuffer sits at 256 GiB, its xHCI BAR at ~384.5 GiB).
	 */
	static inline Cap_sel _high_phys_sel(unsigned slot)
	{
		return Cap_sel((Core_cspace::TOP_CNODE_HIGH_PHYS_IDX
		                << Core_cspace::NUM_HIGH_PHYS_SEL_LOG2) | slot);
	}


	/**
	 * Return core-local selector for untyped page at given physical address
	 *
	 * Sponge (row 14): only valid for phys_addr < HIGH_PHYS_BASE — the
	 * 4 KiB-untyped CNode only covers the lower 8 GiB. High-phys
	 * device untypeds are looked up by bootinfo scan in
	 * _high_phys_untyped_sel() and passed directly to seL4_Untyped_Retype
	 * as the 'service' argument; the slot-indexed CNode encoding has
	 * no meaning for them.
	 */
	static inline Cap_sel untyped_sel(addr_t phys_addr)
	{
		return _core_local_sel(Core_cspace::TOP_CNODE_UNTYPED_4K, phys_addr);
	}


	/**
	 * Return core-local selector for the untyped object backing a CNode of
	 * the given backing size at 'phys_addr'.
	 *
	 * Backings up to one page (4 KiB) are served by the 4 KiB untyped pool.
	 * Larger backings are served by the 16 KiB untyped pool, whose objects
	 * are addressed by phys_addr >> LARGE_BACKING_UNTYPED_LOG2.
	 */
	static inline Cap_sel untyped_sel(addr_t phys_addr, addr_t backing_log2)
	{
		if (backing_log2 > PAGE_SIZE_LOG2)
			return _core_local_sel(Core_cspace::TOP_CNODE_UNTYPED_16K,
			                       phys_addr, LARGE_BACKING_UNTYPED_LOG2);

		return untyped_sel(phys_addr);
	}


	/**
	 * Per-arch physical-memory allocator used for CNode backing.
	 *
	 * On x86_64, backings larger than one page are drawn from the 16 KiB
	 * untyped pool (the same pool that backs VCPUs). On other architectures
	 * CNodes stay page-sized, so the default allocator is returned unchanged
	 * and the large-backing path is never taken.
	 */
	static Range_allocator &cnode_backing_alloc(uint8_t  cnode_size_log2,
	                                            Range_allocator &default_alloc);


	/**
	 * Return core-local selector for 4K page frame at given physical address
	 *
	 * Sponge (row 14 v2): for phys_addr >= HIGH_PHYS_BASE (8 GiB), the
	 * frame cap is looked up in the slot table (the slot was recorded
	 * when the frame was created). A miss yields a deliberately
	 * out-of-range slot so the cap operation fails loudly instead of
	 * silently addressing a wrong frame.
	 */
	static inline Cap_sel frame_sel(addr_t phys_addr)
	{
		if (phys_addr >= Core_cspace::HIGH_PHYS_BASE) {
			unsigned slot = 0;
			if (Core::high_phys_slot_find(phys_addr, slot))
				return _high_phys_sel(slot);
			/* slot-encoded dataspace reference or unconverted phys —
			 * fall through to the low flat formula (v1 behavior) */
		}

		return _core_local_sel(Core_cspace::TOP_CNODE_PHYS_IDX, phys_addr);
	}


	static seL4_Word smallest_page_type();


	/*
	 * Sponge (row 14 v2): locate the device untyped covering phys_addr
	 * by bootinfo scan; outputs its base and size_bits. The returned
	 * selector resolves at runtime via the core-CNode wiring.
	 */
	static inline seL4_Untyped _high_phys_untyped_find(addr_t   phys_addr,
	                                                   addr_t & base,
	                                                   addr_t & size_bits)
	{
		seL4_BootInfo const &bi = sel4_boot_info();
		unsigned const count = (unsigned)(bi.untyped.end - bi.untyped.start);

		for (unsigned i = 0; i < count; i++) {
			auto const &desc = bi.untypedList[i];
			if (!desc.isDevice)
				continue;
			addr_t const b = desc.paddr;
			addr_t const s = 1UL << desc.sizeBits;
			if (phys_addr >= b && phys_addr < b + s) {
				base      = b;
				size_bits = desc.sizeBits;
				return (seL4_Untyped)(bi.untyped.start + i);
			}
		}
		return 0;
	}




	/**
	 * Create page frames from untyped memory
	 */
	static inline bool convert_to_page_frames(addr_t phys_addr,
	                                          size_t num_pages)
	{
		auto const phys_addr_base = phys_addr;

		for (size_t i = 0; i < num_pages; i++, phys_addr += PAGE_SIZE) {

			seL4_Untyped service;
			seL4_Word    node_index;
			seL4_Word    node_offset;

			/*
			 * Sponge (row 14 v2): a high-phys frame must be created at
			 * the untyped's watermark, so fast-forward the watermark to
			 * the target page first (see _high_phys_skip_to), then
			 * retype the frame into a sequentially allocated slot of
			 * the high-phys CNode and record slot->phys for the
			 * map-time lookup. Handled entirely here (continue).
			 */
			if (phys_addr >= Core_cspace::HIGH_PHYS_BASE) {
				addr_t    base      = 0;
				addr_t    size_bits = 0;
				service = _high_phys_untyped_find(phys_addr, base, size_bits);
				if (!service) {
					error(__FUNCTION__, ": no high-phys untyped for ",
					      Hex_range<addr_t>(phys_addr, PAGE_SIZE));
					convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);
					return false;
				}
				addr_t &next = Core::high_phys_watermark(service, base);
				if (phys_addr < next) {
					/* a second IO_MEM target below the watermark in
					 * the same untyped — not needed by the boot chain */
					error(__FUNCTION__, ": high-phys address below watermark: ",
					      Hex_range<addr_t>(phys_addr, PAGE_SIZE));
					convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);
					return false;
				}
				if (!Core::high_phys_skip_to(service, base, size_bits, next, phys_addr)) {
					error(__FUNCTION__, ": high-phys skip failed for ",
					      Hex_range<addr_t>(phys_addr, PAGE_SIZE));
					convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);
					return false;
				}
				unsigned slot = 0;
				if (!Core::high_phys_slot_alloc(phys_addr, slot)) {
					error(__FUNCTION__, ": high-phys slot table full for ",
					      Hex_range<addr_t>(phys_addr, PAGE_SIZE));
					convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);
					return false;
				}
				long const hret = seL4_Untyped_Retype(service,
				                                      smallest_page_type(), 0,
				                                      Core_cspace::top_cnode_sel(),
				                                      Core_cspace::TOP_CNODE_HIGH_PHYS_IDX,
				                                      Core_cspace::NUM_TOP_SEL_LOG2,
				                                      slot, 1);
				if (hret != seL4_NoError) {
					error(__FUNCTION__, ": high-phys retype returned ", hret,
					      " for ", Hex_range<addr_t>(phys_addr, PAGE_SIZE));
					Core::high_phys_slot_free(slot);
					convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);
					return false;
				}
				next += PAGE_SIZE;
				continue;
			}

			{
				service     = untyped_sel(phys_addr).value();
				node_index  = Core_cspace::TOP_CNODE_PHYS_IDX;
				node_offset = phys_addr >> PAGE_SIZE_LOG2;
			}

			seL4_Word    const type        = smallest_page_type();
			seL4_Word    const size_bits   = 0;
			seL4_CNode   const root        = Core_cspace::top_cnode_sel();
			seL4_Word    const node_depth  = Core_cspace::NUM_TOP_SEL_LOG2;
			seL4_Word    const num_objects = 1;

			long const ret = seL4_Untyped_Retype(service,
			                                     type,
			                                     size_bits,
			                                     root,
			                                     node_index,
			                                     node_depth,
			                                     node_offset,
			                                     num_objects);

			if (ret == seL4_NoError)
				continue;

			/* Sponge (row 14 v2): drop the slot recorded for this page */
			if (phys_addr >= Core_cspace::HIGH_PHYS_BASE) {
				unsigned slot = 0;
				if (Core::high_phys_slot_find(phys_addr, slot))
					Core::high_phys_slot_free(slot);
			}

			error(__FUNCTION__, ": seL4_Untyped_RetypeAtOffset "
			      "returned ", ret, " - physical_range=",
			      Hex_range(node_offset << PAGE_SIZE_LOG2,
			                (num_pages - i) * PAGE_SIZE));

			/* revert already converted memory */
			convert_to_untyped_frames(phys_addr_base, PAGE_SIZE * i);

			return false;
		}

		return true;
	}


	/**
	 * Free up page frames and turn it so into untyped memory
	 */
	static inline void convert_to_untyped_frames(addr_t const phys_addr,
	                                             addr_t const phys_size)
	{
		for (addr_t phys = phys_addr; phys < phys_addr + phys_size; phys += PAGE_SIZE) {

			seL4_Untyped service;
			seL4_Uint8   space_size;
			unsigned     index;

			/*
			 * Sponge (row 14 v2): high-phys frames live in the
			 * high-phys CNode at their recorded slot; the slot is
			 * freed afterwards so it can be reused.
			 */
			bool const high = phys >= Core_cspace::HIGH_PHYS_BASE;
			if (high) {
				unsigned slot = 0;
				if (!Core::high_phys_slot_find(phys, slot))
					continue;  /* never converted — nothing to release */
				service    = Core_cspace::high_phys_cnode_sel();
				space_size = (seL4_Uint8)Core_cspace::NUM_HIGH_PHYS_SEL_LOG2;
				index      = slot;
			} else {
				service    = Core_cspace::phys_cnode_sel();
				space_size = (seL4_Uint8)Core_cspace::NUM_PHYS_SEL_LOG2;
				index      = (unsigned)(phys >> PAGE_SIZE_LOG2);
			}

			/**
			 * Without the revoke, one gets sporadically
			 *  Untyped Retype: Insufficient memory ( xx bytes needed, x bytes
			 *                                        available)
			 * for the phys_addr when it gets reused.
			 */
			int ret = seL4_CNode_Revoke(service, index, space_size);
			if (ret != seL4_NoError)
				error(__FUNCTION__, ": seL4_CNode_Revoke returned ", ret);

			/**
			 * Without the delete, one:
			 *  Untyped Retype: Slot #xxxx in destination window non-empty
			 */
			ret = seL4_CNode_Delete(service, index, space_size);
			if (ret != seL4_NoError)
				error(__FUNCTION__, ": seL4_CNode_Delete returned ", ret);

			if (high)
				Core::high_phys_slot_free(index);
		}
	}
	};

#endif /* _CORE__INCLUDE__UNTYPED_MEMORY_H_ */
