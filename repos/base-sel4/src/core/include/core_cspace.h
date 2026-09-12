/*
 * \brief   Core's CSpace layout definition
 * \author  Norman Feske
 * \date    2015-05-06
 */

/*
 * Copyright (C) 2015-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__INCLUDE__CORE_CSPACE_H_
#define _CORE__INCLUDE__CORE_CSPACE_H_

#include <sel4_boot_info.h>

namespace Core { class Core_cspace; }


class Core::Core_cspace
{
	public:

		/* CNode dimensions */
		enum {
			NUM_TOP_SEL_LOG2  = 11UL,
			/* CONFIG_ROOT_CNODE_SIZE_BITS from seL4 autoconf.h */
			NUM_CORE_SEL_LOG2 = CONFIG_ROOT_CNODE_SIZE_BITS,
			NUM_PHYS_SEL_LOG2 = 21UL,

			/*
			 * Sponge Phase 15 W4 (row 14 patch — complements row 13
			 * in the seL4 kernel). The flat phys CNode above caps
			 * frame caps at 2^21 slots = 8 GiB of physical address
			 * space at 4 KiB granularity; anything at or above
			 * HIGH_PHYS_BASE needs a second CNode.
			 *
			 * v2 (17ZD90N measured values): the high-phys frame
			 * caps live in a SMALL dedicated CNode whose slots are
			 * allocated SEQUENTIALLY, with a slot->phys table
			 * (high_phys_frame_phys[]) for the phys->slot lookup at
			 * map time. The v1 design flat-indexed slots by
			 * (phys - HIGH_PHYS_BASE) >> 12 into a 2^23-slot CNode;
			 * it failed on the 17ZD90N twice over: (a) the 256 MiB
			 * contiguous CNode backing cannot be allocated from the
			 * 16 KiB pool on the Insyde-fragmented memory map
			 * ("high-phys CNode backing: 16k pool exhausted"), and
			 * (b) the real targets — GOP framebuffer at
			 * 0x4000000000 (256 GiB) and the PCH xHCI BAR at
			 * 0x601d140000 (~384.5 GiB) — sit 128 GiB apart, so no
			 * affordable flat window covers both. Sequential
			 * allocation makes the backing 512 KiB and removes any
			 * address-window assumption. 2^14 slots = 64 MiB of
			 * concurrently mapped high-phys memory (the 17ZD90N
			 * needs ~4000 slots for the FB + 16 for the BAR).
			 */
			NUM_HIGH_PHYS_SEL_LOG2 = 14UL,

			NUM_CORE_PAD_SEL_LOG2 = 32UL - NUM_TOP_SEL_LOG2 - NUM_CORE_SEL_LOG2,
		};

		/*
		 * Sponge (row 14): threshold above which a physical address
		 * cannot live in the flat low phys CNode (2^21 slots at
		 * 4 KiB granularity = 8 GiB). High-phys frame caps are kept
		 * in the dedicated high-phys CNode; their slots come from
		 * the sequential allocator below, not from address math.
		 */
		static addr_t constexpr HIGH_PHYS_BASE = 1ULL << 33; /* 8 GiB */

		/* selectors for initially created CNodes during core bootup */
		static unsigned top_cnode_sel()       { return (unsigned)sel4_boot_info().empty.start; }
		static unsigned core_pad_cnode_sel()  { return top_cnode_sel()      + 1; }
		static unsigned core_cnode_sel()      { return core_pad_cnode_sel() + 1; }
		static unsigned phys_cnode_sel()      { return core_cnode_sel()     + 1; }
		static unsigned untyped_cnode_4k()    { return phys_cnode_sel()     + 1; }
		static unsigned untyped_cnode_8k()    { return untyped_cnode_4k()   + 1; }
		static unsigned untyped_cnode_16k()   { return untyped_cnode_8k()   + 1; }
		static unsigned io_port_sel()         { return untyped_cnode_16k()  + 1; }

		/*
		 * Sponge (row 14): selector reserved for the high-phys CNode
		 * cap in the initial thread CNode. The slot is allocated at
		 * boot (no kernel allocation needed) but the CNode itself is
		 * constructed lazily by Platform::construct_high_phys_cnode()
		 * on first IO_MEM request that needs it. Reserving the slot
		 * upfront keeps the cap-sel arithmetic in
		 * untyped_memory.h::high_phys_cnode_sel() simple (it's a
		 * static offset relative to the top CNode just like the
		 * other CNode selectors).
		 */
		static unsigned high_phys_cnode_sel() { return io_port_sel()        + 1; }

		static unsigned core_static_sel_end() { return high_phys_cnode_sel() + 1; }

		/* indices within top-level CNode */
		enum Top_cnode_idx {
			TOP_CNODE_CORE_IDX       = 0,

			TOP_CNODE_UNTYPED_16K    = 0x7fc, /* untyped objects 16K  */
			TOP_CNODE_UNTYPED_8K     = 0x7fd, /* untyped objects  8K  */
			TOP_CNODE_UNTYPED_4K     = 0x7fe, /* untyped objects  4K  */
			TOP_CNODE_PHYS_IDX       = 0x7ff, /* physical page frames */

			/*
			 * Sponge row 14: dedicated CNode for page-frame caps of
			 * physical addresses above 8 GiB. See NUM_HIGH_PHYS_SEL_LOG2
			 * for the window size rationale. The slot 0x7e0 sits
			 * well below the four reserved slots (0x7fc-0x7ff) and
			 * is far from the core PD root-cnode entries (0-0x7fb),
			 * so it never collides with the existing layout.
			 */
			TOP_CNODE_HIGH_PHYS_IDX  = 0x7e0  /* physical page frames >= 8 GiB */
		};
		enum { CORE_VM_ID = 1 };
};

namespace Core {

/*
 * Sponge (row 14 v2): slot<->phys bookkeeping for the high-phys CNode.
 * Slots are allocated sequentially (never by address math — the real
 * high-MMIO targets can sit hundreds of GiB apart). Zero entries are
 * free; freed slots are reused. Shared by untyped_memory.h (frame
 * creation/destruction) and vm_space.h (map-time lookup); defined as
 * inline variables here because both headers already include this one.
 */
inline Genode::addr_t high_phys_frame_phys[1UL << Core_cspace::NUM_HIGH_PHYS_SEL_LOG2] = { };
inline unsigned      high_phys_frame_count = 0;

/*
 * Allocate a slot for 'phys_addr' and record it. Reuses freed (zero)
 * slots first. Returns false when the table is full — the caller
 * fails the IO_MEM session cleanly.
 */
inline bool high_phys_slot_alloc(Genode::addr_t phys_addr, unsigned &slot)
{
	for (unsigned i = 0; i < high_phys_frame_count; i++) {
		if (high_phys_frame_phys[i] == 0) {
			slot = i;
			high_phys_frame_phys[slot] = phys_addr;
			return true;
		}
	}
	if (high_phys_frame_count >= (1UL << Core_cspace::NUM_HIGH_PHYS_SEL_LOG2))
		return false;
	slot = high_phys_frame_count++;
	high_phys_frame_phys[slot] = phys_addr;
	return true;
}

/* Find the slot holding 'phys_addr'. Returns false if never allocated. */
inline bool high_phys_slot_find(Genode::addr_t phys_addr, unsigned &slot)
{
	for (unsigned i = 0; i < high_phys_frame_count; i++) {
		if (high_phys_frame_phys[i] == phys_addr) {
			slot = i;
			return true;
		}
	}
	return false;
}

/* Release a slot (frame revoked + deleted by the caller). */
inline void high_phys_slot_free(unsigned slot)
{
	high_phys_frame_phys[slot] = 0;
}

}


#endif /* _CORE__INCLUDE__CORE_CSPACE_H_ */
