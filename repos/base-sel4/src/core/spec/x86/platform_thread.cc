/*
 * \brief   Platform thread interface implementation - x86 specific
 * \author  Alexander Boettcher
 * \date    2017-08-08
 */

/*
 * Copyright (C) 2017-2025 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#include <platform_thread.h>
#include <arch_kernel_object.h>

using namespace Core;


namespace {

	/*
	 * Sponge (row 15): metadata backing for the 16 KiB physical pool's
	 * allocator tree, served from a static core-local arena instead of
	 * core's mapped-memory allocator. Growing the AVL slab via
	 * 'core_mem_alloc' allocates a fresh core mapping on the fly
	 * (_map_local -> page tables/CNodes), which itself allocates from
	 * the 16 KiB pool while the pool's mutex is still held by the
	 * outer alloc_aligned — a same-thread mutex re-acquire that
	 * deadlocks core ("deadlock ahead") once runtime concurrency
	 * (boot_fb blit timer x storage chain, Phase 15) grows the tree
	 * past its initial blocks. The arena is a plain bump allocator:
	 * the metadata (4 MiB, ~85 nodes per 4 KiB block) is tiny compared
	 * to the pool it manages, and exhaustion fails allocations cleanly
	 * with OUT_OF_RAM instead of deadlocking.
	 */
	struct Avl_metadata_arena : Genode::Allocator
	{
		enum { ARENA_SIZE = 4 * 1024 * 1024 };

		alignas(4096) char _space[ARENA_SIZE] { };
		size_t _consumed { 0 };

		Alloc_result try_alloc(size_t num_bytes) override
		{
			if (num_bytes > sizeof(_space) - _consumed)
				return Genode::Alloc_error::OUT_OF_RAM;

			void * const ptr = _space + _consumed;
			_consumed += num_bytes;
			return { *this, { ptr, num_bytes } };
		}

		void   _free(Allocation &) override { }
		void   free(void *, size_t) override { }
		bool   need_size_for_free() const override { return false; }
		size_t overhead(size_t) const override { return 0; }
		size_t consumed() const override { return _consumed; }
	};
}


Phys_allocator &Core::phys_alloc_16k(Allocator * /* core_mem_alloc */)
{
	/*
	 * Sponge (row 15): the pool's AVL metadata is served from a static
	 * core-local arena (see Avl_metadata_arena above) instead of
	 * core's mapped-memory allocator. Growing the AVL slab via
	 * 'core_mem_alloc' allocates a fresh core mapping on the fly
	 * (_map_local -> page tables/CNodes), which itself allocates from
	 * this very pool while the pool's mutex is still held by the outer
	 * alloc_aligned — a same-thread mutex re-acquire that deadlocks
	 * core ("deadlock ahead") under runtime concurrency (boot_fb blit
	 * timer x storage chain, Phase 15). Arena exhaustion fails
	 * allocations cleanly with OUT_OF_RAM instead of deadlocking.
	 *
	 * The arena must be a function-local static like the allocator
	 * itself: a file-scope object's constructor does not run before
	 * first use in core, leaving its vtable null.
	 */
	static Avl_metadata_arena avl_metadata_arena { };
	static Phys_allocator phys_alloc_16k(&avl_metadata_arena);
	return phys_alloc_16k;
}


void Platform_thread::affinity(Affinity::Location const location)
{
	seL4_Error const res = seL4_TCB_SetAffinity(tcb_sel().value(), location.xpos());
	if (res == seL4_NoError)
		_location = location;
}


bool Thread_info::init_vcpu(Core::Platform &platform, Cap_sel ept)
{
	enum { PAGES_16K = (1UL << Vcpu_kobj::SIZE_LOG2) / 4096 };

	auto phys_result = Untyped_memory::alloc_pages(phys_alloc_16k(), PAGES_16K);

	if (phys_result.failed())
		return false;

	phys_result.with_result([&](auto &result) {
		result.deallocate = false;
		this->vcpu_state_phys = addr_t(result.ptr);
	}, [](auto) { /* handled before by explicit failed() check */ });

	return platform.core_sel_alloc().alloc().convert<bool>([&](auto sel) {
		this->vcpu_sel = Cap_sel(unsigned(sel));

		seL4_Untyped const service = Untyped_memory::_core_local_sel(Core_cspace::TOP_CNODE_UNTYPED_16K, vcpu_state_phys, Vcpu_kobj::SIZE_LOG2).value();

		if (!create<Vcpu_kobj>(service, platform.core_cnode().sel(), vcpu_sel))
			return false;

		seL4_Error res = seL4_X86_VCPU_SetTCB(vcpu_sel.value(), tcb_sel.value());
		if (res != seL4_NoError)
			return false;

		int error = seL4_TCB_SetEPTRoot(tcb_sel.value(), ept.value());
		if (error != seL4_NoError)
			return false;

		return true;
	}, [](auto) { return false; });
}
