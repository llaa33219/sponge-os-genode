/*
 * \brief  Implementation of the IO_MEM session interface
 * \author Norman Feske
 * \author Alexander Boettcher
 * \date   2015-05-01
 */

/*
 * Copyright (C) 2015-2025 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* core includes */
#include <io_mem_session_component.h>
#include <untyped_memory.h>
#include <platform.h>
#include <platform_generic.h>


using namespace Core;


Io_mem_session_component::Dataspace_attr Io_mem_session_component::_acquire(Phys_range request)
{
	if (!request.req_size)
		return Dataspace_attr();

	auto const size = request.size();
	auto const base = request.base();
	auto const req_base = request.req_base;
	auto const req_size = request.req_size;

	if (size < PAGE_SIZE)
		return Dataspace_attr();

	/* check whether first/last pages are already in use by another ds */
	bool first_page_used = _io_mem_alloc.alloc_addr(PAGE_SIZE, base).failed();
	bool  last_page_used = _io_mem_alloc.alloc_addr(PAGE_SIZE, base + size - PAGE_SIZE).failed();

	/* try allocation of whole requested region */
	bool const ok = _io_mem_alloc.alloc_addr(req_size, req_base).convert<bool>(
		[&] (Range_allocator::Allocation &) { return true; },
		[&] (Alloc_error) { return false; });

	if (!ok) {
		error("I/O memory ", Hex_range<addr_t>(req_base, req_size),
		      " not available");
		return Dataspace_attr();
	}

	auto convert_phys = base;
	auto convert_size = size;

	/* if first page was in use beforehand, exclude it from conversion */
	if (first_page_used) {
		convert_phys += PAGE_SIZE;
		convert_size -= PAGE_SIZE;
	}

	if (convert_size >= PAGE_SIZE) {
		/* if last page was in use beforehand, exclude it from conversion */
		if (last_page_used)
			convert_size -= PAGE_SIZE;

		size_t const num_pages = convert_size >> PAGE_SIZE_LOG2;

		/*
		 * Sponge (row 14): for phys_addr above HIGH_PHYS_BASE (8 GiB),
		 * trigger the lazy creation of the dedicated high-phys CNode
		 * before the retyp. The CNode MUST exist by the time
		 * convert_to_page_frames fires (it points node_index at
		 * TOP_CNODE_HIGH_PHYS_IDX), and convert_to_page_frames now
		 * handles both the high-phys retyp (via _high_phys_untyped_sel)
		 * and the low-phys retyp paths. Failure to construct the CNode
		 * (16K pool exhausted, etc.) is logged and the IO_MEM session
		 * fails cleanly with "I/O memory ... not available" — no
		 * crash. See docs/11-environment.md row 14.
		 */
		if (convert_phys >= Core_cspace::HIGH_PHYS_BASE
		    && !platform_specific().construct_high_phys_cnode()) {
			error("high-phys CNode unavailable; IO_MEM ",
			      Hex_range<addr_t>(req_base, req_size), " fails");
			return Dataspace_attr();
		}

		if (!Untyped_memory::convert_to_page_frames(convert_phys, num_pages))
			return Dataspace_attr();
	}

	return Dataspace_attr(size, 0 /* no core local mapping */, base,
	                      _cacheable, req_base);
}


void Io_mem_session_component::_release(Dataspace_attr const &attr)
{
	/*
	 * The generic base io_mem implementation already removed the region
	 * from _io_mem_alloc. Now we have to check, which parts must be
	 * converted back to untyped memory. The first and last page of the
	 * region may still be in use by another io-mem allocation, so check
	 * for first/last page and exclude it from conversion if required.
	 */
	auto convert_phys = attr.phys_addr;
	auto convert_size = attr.size;

	if (convert_size >= PAGE_SIZE)
		_io_mem_alloc.alloc_addr(PAGE_SIZE, convert_phys).with_result(
			/* if the region is not occupied anymore, it becomes untyped */
			[&] (Range_allocator::Allocation &) { },
			[&] (Alloc_error) {
				/*
				 * The region is still occupied by another allocation,
				 * exclude the page from conversion back to untyped memory
				 */
				convert_phys += PAGE_SIZE;
				convert_size -= PAGE_SIZE;
			});

	if (convert_size >= PAGE_SIZE)
		_io_mem_alloc.alloc_addr(PAGE_SIZE, convert_phys + convert_size -
			                                PAGE_SIZE).with_result(

			/* if the region is not occupied anymore, it becomes untyped */
			[&] (Range_allocator::Allocation &) { },
			[&] (Alloc_error) {
				/*
				 * The region is still occupied by another allocation,
				 * exclude the page from conversion back to untyped memory
				 */
				convert_size -= PAGE_SIZE;
			});

	auto const num_pages = convert_size >> PAGE_SIZE_LOG2;

	if (num_pages)
		Untyped_memory::convert_to_untyped_frames(convert_phys,
		                                          PAGE_SIZE * num_pages);
}
