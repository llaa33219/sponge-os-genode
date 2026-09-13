/*
 * \brief   Initial pool of untyped memory
 * \author  Norman Feske
 * \date    2016-02-11
 */

/*
 * Copyright (C) 2016-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__INCLUDE__INITIAL_UNTYPED_POOL_H_
#define _CORE__INCLUDE__INITIAL_UNTYPED_POOL_H_

/* Genode includes */
#include <base/exception.h>
#include <base/internal/crt0.h>
#include <util/string.h>

/* core includes */
#include <types.h>
#include <sel4_boot_info.h>

/* seL4 includes */
#include <sel4/sel4.h>
#include <sel4/bootinfo.h>
#include <interfaces/sel4_client.h>

namespace Core { class Initial_untyped_pool; }


class Core::Initial_untyped_pool
{
	private:

		/* base limit on sel4's autoconf.h */
		enum { MAX_UNTYPED = (unsigned)CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS };

		struct Free_offset { addr_t value = 0; };

		Free_offset _free_offset[MAX_UNTYPED];

		/*
		 * Sponge (row 11 → core-side migration): artifact ranges of
		 * the seL4 kernel's init_freemem subtraction bug. The kernel
		 * subtracts the reserved regions (kernel+userland image) from
		 * the available regions with a pointer that advances
		 * unconditionally per reserved entry — on fragmented maps the
		 * later fragments are never subtracted, leak into freemem,
		 * and surface as RAM untypeds that overlap the reserved image
		 * or even each other. Retyping such an untyped corrupts
		 * kernel state (the historical crash: null deref in
		 * decodeInvocation during Untyped_Retype). The historical fix
		 * patched the kernel (former ledger row 11,
		 * sel4-uefi-untyped-overlap.patch); this core-side detection
		 * replaces it: flagged ranges are leaked on purpose — never
		 * retyped, never allocated from — keeping the seL4 kernel
		 * pristine.
		 */
		bool _artifact[MAX_UNTYPED] { };
		bool _artifacts_detected   { false };

		void _detect_artifacts_once()
		{
			using Genode::warning;

			if (_artifacts_detected)
				return;

			_artifacts_detected = true;

			seL4_BootInfo const &bi = sel4_boot_info();
			unsigned const count =
				(unsigned)(bi.untyped.end - bi.untyped.start);
			if (count > MAX_UNTYPED)
				return;

			/* flag every RAM untyped overlapping [base, base+size) */
			auto flag_overlaps = [&] (addr_t base, addr_t size) {
				for (unsigned i = 0; i < count; i++) {
					seL4_UntypedDesc const &d = bi.untypedList[i];
					if (d.isDevice) continue;
					addr_t const end = d.paddr + (1UL << d.sizeBits);
					if (base < end && d.paddr < base + size)
						_artifact[i] = true;
				}
			};

			/*
			 * (1) mutual overlap among RAM untypeds — freemem
			 * regions are disjoint by construction, any overlap is
			 * a subtraction artifact
			 */
			for (unsigned i = 0; i < count; i++) {
				seL4_UntypedDesc const &a = bi.untypedList[i];
				if (a.isDevice) continue;
				addr_t const a_end = a.paddr + (1UL << a.sizeBits);

				for (unsigned j = i + 1; j < count; j++) {
					seL4_UntypedDesc const &b = bi.untypedList[j];
					if (b.isDevice) continue;
					addr_t const b_end = b.paddr + (1UL << b.sizeBits);

					if (a.paddr < b_end && b.paddr < a_end)
						_artifact[i] = _artifact[j] = true;
				}
			}

			/*
			 * (2) overlap with core's own image (core + boot
			 * modules, the full rootserver image). The physical
			 * base is queried from the kernel: the bootinfo's
			 * userImageFrames.start is the frame cap of the image's
			 * first page — seL4_X86_Page_GetAddress returns its
			 * paddr, independent of where the loader actually
			 * placed the image (under UEFI the image is NOT loaded
			 * at its link address). The size is the linked virtual
			 * extent, which equals the physical extent because the
			 * rootserver image occupies one contiguous physical
			 * region. Fallback if the query fails: the linked
			 * extent (correct for identity-placed boots).
			 */
			{
				addr_t const img_beg_linked =
					(addr_t)&_prog_img_beg;
				addr_t const img_size =
					(addr_t)&_prog_img_end - img_beg_linked;

				addr_t img_beg = img_beg_linked;

				seL4_X86_Page_GetAddress_t const g =
					seL4_X86_Page_GetAddress(
						(seL4_X86_Page)bi.userImageFrames.start);

				if (g.error == seL4_NoError) {
					img_beg = g.paddr;
				}

				flag_overlaps(img_beg, img_size);
			}

			/*
			 * (3) overlap with firmware-reserved regions of the raw
			 * multiboot memory map (X86_MBMMAP bootinfo chunk).
			 * GRUB marks the image area (EfiLoaderData) as usable,
			 * so this is a defensive net rather than the primary
			 * detector — see (2).
			 */
			if (bi.extraLen) {

				struct Mb_mmap_entry {
					uint32_t size;
					uint64_t base_addr;
					uint64_t length;
					uint32_t type;    /* 1 == usable RAM */
				} __attribute__((packed));

				addr_t const extra     =
					reinterpret_cast<addr_t>(&bi) + 4096;
				addr_t const extra_end = extra + bi.extraLen;

				for (seL4_BootInfoHeader const *element =
				     reinterpret_cast<seL4_BootInfoHeader const *>(extra),
				     *next = nullptr;
				     (next = reinterpret_cast<seL4_BootInfoHeader const *>(
				         reinterpret_cast<addr_t>(element) + element->len))
				     && next <= reinterpret_cast<seL4_BootInfoHeader const *>(extra_end)
				     && element->id != SEL4_BOOTINFO_HEADER_PADDING;
				     element = next)
				{
					if (element->id != SEL4_BOOTINFO_HEADER_X86_MBMMAP)
						continue;

					uint32_t const mmap_length =
						*reinterpret_cast<uint32_t const *>(
							reinterpret_cast<addr_t>(element)
							+ sizeof(*element));

					addr_t const entries_beg =
						reinterpret_cast<addr_t>(element)
						+ sizeof(*element) + sizeof(mmap_length);
					addr_t const entries_end = entries_beg + mmap_length;

					/* bounded by the chunk header's length, too */
					if (entries_end >
					    reinterpret_cast<addr_t>(element) + element->len)
						break;

					addr_t pos = entries_beg;
					while (pos + sizeof(Mb_mmap_entry) <= entries_end) {

						Mb_mmap_entry const *entry =
							reinterpret_cast<Mb_mmap_entry const *>(pos);

						/* variable-stride multiboot1 entries */
						size_t const stride =
							entry->size + sizeof(entry->size);
						if (!entry->size || stride < sizeof(Mb_mmap_entry)
						    || pos + stride > entries_end)
							break;

						if (entry->type != 1)
							flag_overlaps((addr_t)entry->base_addr,
							              (addr_t)entry->length);

						pos += stride;
					}
					break; /* single MBMMAP chunk */
				}
			}

			for (unsigned i = 0; i < count; i++)
				if (_artifact[i])
					warning("init_freemem artifact untyped skipped: ",
					        Hex(bi.untypedList[i].paddr),
					        " size ",
					        Hex(1UL << bi.untypedList[i].sizeBits));
		}

	public:

		class Initial_untyped_pool_exhausted : Exception { };

		struct Range
		{
			/* core-local cap selector */
			unsigned const sel;

			/* index into 'untypedSizeBitsList' */
			unsigned const index = (unsigned)(sel - sel4_boot_info().untyped.start);

			/* original size of untyped memory range */
			size_t const size = 1UL << sel4_boot_info().untypedList[index].sizeBits;

			/* physical address of the begin of the untyped memory range */
			addr_t const phys = sel4_boot_info().untypedList[index].paddr;

			bool const device = sel4_boot_info().untypedList[index].isDevice;

			/* offset to the unused part of the untyped memory range */
			addr_t &free_offset;

			Range(Initial_untyped_pool &pool, unsigned sel)
			:
				sel(sel), free_offset(pool._free_offset[index].value)
			{ }
		};

	private:

		/**
		 * Calculate free index after allocation
		 */
		addr_t _align_offset(Range const &range, Align align)
		{
			/*
			 * The seL4 kernel naturally aligns allocations within untuped
			 * memory ranges. So we have to apply the same policy to our
			 * shadow version of the kernel's 'FreeIndex'.
			 */
			addr_t const aligned_free_offset = align_addr(range.free_offset,
			                                              align);

			return aligned_free_offset + (1 << align.log2);
		}

		/**
		 * Apply functor to each untyped memory range
		 *
		 * The functor is called with 'Range &', the current physical
		 * address (offset into the untyped), the size (typically
		 * PAGE_SIZE*256 == 1 MiB for the standard retyp batch), and
		 * the device flag.
		 */
		void for_each_range(auto const &fn)
		{
			seL4_BootInfo const &bi = sel4_boot_info();
			for (addr_t sel = bi.untyped.start; sel < bi.untyped.end; sel++) {
				Range range(*this, (unsigned)sel);
				fn(range, range.phys + range.free_offset,
				   min(1UL << 20, range.size - range.free_offset),
				   range.device);
			}
		}

	public:

		/**
		 * Return selector of untyped memory range where the allocation of
		 * the specified size is possible
		 *
		 * \param kernel object size
		 *
		 * This function models seL4's allocation policy of untyped memory. It
		 * is solely used at boot time to setup core's initial kernel objects
		 * from the initial pool of untyped memory ranges as reported by the
		 * kernel.
		 *
		 * \throw Initial_untyped_pool_exhausted
		 */
		unsigned alloc(uint8_t size_log2)
		{
			_detect_artifacts_once();

			enum { UNKNOWN = 0 };
			unsigned sel = UNKNOWN;

			/*
			 * Go through the known initial untyped memory ranges to find
			 * a range that is able to host a kernel object of 'size'.
			 */
			for_each_range([&] (Range const &range, addr_t const, addr_t const, bool const) {
				/* ignore device memory */
				if (range.device)
					return;

				/* Sponge (row 11 → core): skip artifact ranges */
				if (_artifact[range.index])
					return;

				/* calculate free index after allocation */
				addr_t const new_free_offset = _align_offset(range, { .log2 = size_log2 });

				/* check if allocation fits within current untyped memory range */
				if (new_free_offset > range.size)
					return;

				if (sel == UNKNOWN) {
					sel = range.sel;
					return;
				}

				/* check which range is smaller - take that */
				addr_t const rest = range.size - new_free_offset;

				Range best_fit(*this, sel);
				addr_t const new_free_offset_best = _align_offset(best_fit, { .log2 = size_log2 });
				addr_t const rest_best = best_fit.size - new_free_offset_best;

				if (rest_best >= rest)
					/* current range fits better then best range */
					sel = range.sel;
			});

			if (sel == UNKNOWN) {
				warning("Initial_untyped_pool exhausted");
				throw Initial_untyped_pool_exhausted();
			}

			Range best_fit(*this, sel);
			addr_t const new_free_offset = _align_offset(best_fit, { .log2 = size_log2 });
			ASSERT(new_free_offset <= best_fit.size);

			/*
			 * We found a matching range, consume 'size' and report the
			 * selector. The returned selector is used by the caller
			 * of 'alloc' to perform the actual kernel-object creation.
			 */
			best_fit.free_offset = new_free_offset;

			return best_fit.sel;
		}

		/**
		 * Convert (remainder) of the initial untyped memory into untyped
		 * objects of size_log2 and up to a maximum as specified by max_memory
		 */
		void turn_into_untyped_object(addr_t  const node_index,
		                              auto    const &fn,
		                              auto    const &fn_revert,
		                              uint8_t const size_log2 = PAGE_SIZE_LOG2,
		                              addr_t  max_memory = 0UL - 0x1000UL)
		{
			_detect_artifacts_once();

			for_each_range([&] (Range const &range, addr_t const /*phys*/, addr_t const /*size*/, bool const /*device*/) -> bool {

				/* Sponge (row 11 → core): skip artifact ranges */
				if (_artifact[range.index])
					return true;

				/*
				 * The kernel limits the maximum number of kernel objects to
				 * be created via a single untyped-retype operation. So we
				 * need to iterate for each range, converting a limited batch
				 * of pages in each step.
				 */
				for (;;) {

					addr_t const page_aligned_free_offset =
						align_addr(range.free_offset, { .log2 = size_log2 });

					/* back out if no further page can be allocated */
					if (page_aligned_free_offset + (1UL << size_log2) > range.size)
						return true;

					if (!max_memory)
						return true;

					size_t const remaining_size    = range.size - page_aligned_free_offset;
					size_t const retype_size_limit = PAGE_SIZE*256;
					size_t const batch_size        = min(min(remaining_size, retype_size_limit), max_memory);

					addr_t const phys_addr = range.phys + page_aligned_free_offset;
					size_t const num_pages = batch_size / (1UL << size_log2);

					seL4_Untyped const service     = range.sel;
					addr_t       const type        = seL4_UntypedObject;
					addr_t       const size_bits   = size_log2;
					seL4_CNode   const root        = Core_cspace::top_cnode_sel();
					addr_t       const node_depth  = Core_cspace::NUM_TOP_SEL_LOG2;
					addr_t       const node_offset = phys_addr >> size_log2;
					addr_t       const num_objects = num_pages;

					/* skip memory because of limited untyped phys cnode range */
					if (node_offset >= (1UL << (Core_cspace::NUM_PHYS_SEL_LOG2))) {
						/*
						 * Sponge (row 14): device memory above 8 GiB is
						 * NOT dropped here — Platform::_init_allocators
						 * registers the range in _io_mem_alloc and the
						 * functor returns false to skip the eager retyp.
						 * The page-frame caps are created lazily — on
						 * demand — in the dedicated high-phys CNode
						 * (constructed on first IO_MEM request that
						 * needs it, see Platform::construct_high_phys_cnode()).
						 * Skip the warning AND fall through to the
						 * functor call so the range is registered.
						 */
						bool const high_phys_device =
							range.device && range.phys >= Core_cspace::HIGH_PHYS_BASE;

						if (!high_phys_device)
							warning(range.device ? "device" : "      ", " memory in range ",
							        Hex_range<addr_t>(range.phys, range.size),
							        " is unavailable (due to limited untyped cnode range)");

						/* fall through to functor for high_phys_device */
						if (!high_phys_device)
							return true;
					}

					/* invoke callback about the range */
					bool const used = fn(range, phys_addr, num_pages << size_log2,
					                     range.device);

					if (!used)
						return true;

					long const ret = seL4_Untyped_Retype(service,
					                                     type,
					                                     size_bits,
					                                     root,
					                                     node_index,
					                                     node_depth,
					                                     node_offset,
					                                     num_objects);

					if (ret != 0) {
						error("turn_into_untyped_object : "
						      "seL4_Untyped_Retype (untyped) returned ", ret);
						fn_revert(range, phys_addr, num_pages << size_log2, range.device);
						return false;
					}

					/* mark consumed untyped memory range as allocated */
					range.free_offset += batch_size;

					/* track memory left to be converted */
					max_memory -= batch_size;
				}
			});
		}
};

#endif /* _CORE__INCLUDE__INITIAL_UNTYPED_POOL_H_ */
