/*
 * \brief  seL4-specific RPC capability factory
 * \author Norman Feske
 * \date   2016-01-19
 */

/*
 * Copyright (C) 2016-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <base/capability.h>
#include <util/misc_math.h>

/* core includes */
#include <rpc_cap_factory.h>
#include <platform.h>

/* base-internal include */
#include <core_capability_space.h>

using namespace Core;


namespace {

	/*
	 * Source of RPC object keys
	 *
	 * Two invariants make the client-side key -> capability lookup sound:
	 *
	 * - Uniqueness: a key is never reused for a different RPC object. A
	 *   lookup hit names exactly the server incarnation the key was
	 *   issued for and can never alias a stale entry of a dead one.
	 *
	 * - Disjointness (upstream issue #5779): factory keys never collide
	 *   with the pager's keys, which are plain 'cap_sel' values below
	 *   '2^Core_cspace::NUM_CORE_SEL_LOG2'. The counter starts at that
	 *   boundary and only grows, so both key populations stay separate.
	 *
	 * The range ends at 'Rpc_obj_key::INVALID', which is never issued, and
	 * never wraps into the 'cap_sel' range (32-bit safe).
	 */
	addr_t next_rpc_obj_key_value()
	{
		addr_t const first = 1UL << Core_cspace::NUM_CORE_SEL_LOG2;

		static Mutex  mutex { };
		static addr_t counter = first;

		Mutex::Guard guard(mutex);

		if (counter == Rpc_obj_key::INVALID)
			return Rpc_obj_key::INVALID;

		addr_t const key = counter;

		counter = key + 1;

		if (counter < first || counter == Rpc_obj_key::INVALID)
			counter = Rpc_obj_key::INVALID;

		return key;
	}
}


Rpc_cap_factory::Alloc_result Rpc_cap_factory::alloc(Native_capability ep)
{
	if (!ep.valid())
		return Native_capability();

	Mutex::Guard guard(_mutex);

	Rpc_obj_key const rpc_obj_key(next_rpc_obj_key_value());

	if (!rpc_obj_key.valid())
		return Alloc_result(Alloc_error::OUT_OF_CAPS);

	auto cap = Capability_space::create_rpc_obj_cap(ep, rpc_obj_key);

	try {
		if (cap.valid()) {
			_pool.insert(new (_entry_slab) Entry(cap));
			return Alloc_result(cap);
		}
	}
	catch (Out_of_caps) { return Alloc_result(Alloc_error::OUT_OF_CAPS); }
	catch (Out_of_ram)  { return Alloc_result(Alloc_error::OUT_OF_RAM);  }
	catch (Denied)      { return Alloc_result(Alloc_error::DENIED);      }

	return Alloc_result( Alloc_error::DENIED );
}


void Rpc_cap_factory::free(Native_capability cap)
{
	if (!cap.valid())
		return;

	Mutex::Guard guard(_mutex);

	Entry * entry_ptr = nullptr;
	_pool.apply(cap, [&] (Entry * ptr) {
		if (!ptr)
			return;

		_pool.remove(ptr);

		/*
		 * The key comes from the monotonic range of
		 * 'next_rpc_obj_key_value' and is never released, so that it
		 * remains unique for the lifetime of the system.
		 */
		Capability_space::destroy_rpc_obj_cap(cap);

		entry_ptr = ptr;
	});

	if (entry_ptr)
		destroy(_entry_slab, entry_ptr);
}


Rpc_cap_factory::~Rpc_cap_factory()
{
	Mutex::Guard guard(_mutex);

	_pool.remove_all([this] (Entry *ptr) {
		if (ptr)
			destroy(_entry_slab, ptr); });
}
