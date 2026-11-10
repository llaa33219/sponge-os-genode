/*
 * \brief  Regression test for pending-session lifetime and the
 *         close-completion race of 'Component_env::close'
 * \author Sponge OS contributors
 * \date   2026-11-10
 *
 * Phase 1 - create expiry (the original harness)
 *
 * A session creation that the provider never answers runs into the deadline
 * of 'Child::expire_pending_ops'. The expiry closes the session at the parent
 * (the close stays in flight there) and releases the client with a denial.
 *
 * The pending close operation must survive the 'session_ready' notification
 * that carries the denial to the client: 'Child::session_ready' and
 * 'Env_service::session_ready' both clear the pending operations of the
 * session, so registering the close BEFORE the notification silently drops
 * it. Without the close operation, 'Child::_pending_close_op' reports no
 * close in flight and 'Child::session_cap' destroys the session state while
 * the parent-side ID is still live.
 *
 * Phase 2 - the close-completion race of 'Component_env::close'
 *
 * 'Component_env::close' used to wait ONCE on the shared session Blockade
 * after 'Parent::close' answered PENDING. The Blockade signal is shared by
 * all sessions of the component, so the wake-up may be caused by an
 * unrelated session while the close is still in flight. The close then
 * returned with the parent still holding the session in CLOSE_REQUESTED, and
 * the next session request that re-used the client-side ID was refused with
 * "requested conflicting session ID" - a component-fatal denial. This is the
 * field failure 'requested conflicting session ID 97' with the existing
 * session in 'state=CLOSING'.
 *
 * The regression starts the test binary a second time as a real child of the
 * in-process 'Genode::Child' (the rm_fault pattern). The child role drives
 * 'Env::close' - the code under test - against the in-process
 * 'Pending_service', which takes requests but answers nothing until the test
 * completes a close explicitly. The wake-up that releases the buggy close
 * early is the create expiry of a second session (the existing behavior),
 * and the real close completion is 'Pending_service::complete_close' plus
 * one 'expire_pending_ops' tick: the probe reports 'Gone', the session state
 * is destroyed, and the parent-side ID is released for the same-ID re-create.
 *
 * Phase 3 - the parent-side close/create collision
 *
 * See '_create_collision_phase'. A create under a client-side ID whose old
 * session is still CLOSE_REQUESTED must be parked, not fatally denied.
 *
 * Phase 4 - the Root_proxy close-response ordering
 *
 * The child role announces a 'ProxyProbe' service whose root sleeps in
 * 'close' ('Slow_close_root'), so the child's 'Root_proxy' executes a
 * provider close that blocks. The parent creates and closes a session at
 * that service and observes its own session state: the 'CLOSED' response
 * must arrive - and the same-ID create must complete - while the provider
 * close is still sleeping. Pre-fix 'Root_proxy' calls 'Root_client::close'
 * before 'session_response', so the answer is held hostage by the provider
 * close: the state stays CLOSE_REQUESTED and the same-ID create stays
 * parked past the observation window. The delayed close must still run to
 * completion ('Root_client::close' remains the resource-cleanup call): the
 * child role hands the parent a marker session once it did.
 */

/* Genode includes */
#include <base/component.h>
#include <base/child.h>
#include <base/log.h>
#include <base/rpc_server.h>
#include <os/session_requester.h>
#include <root/root.h>
#include <timer_session/connection.h>

using namespace Genode;

namespace {

	/* client-side IDs of the close-race child role */
	enum {
		RACE_SESSION_ID = 100,
		WAKE_SESSION_ID = 101,

		/* marker session: the delayed provider close ran to completion */
		PROBE_MARKER_ID = 102,

		/* release channel of the delayed provider close */
		PROBE_SIGNAL_ID = 103
	};

	/* client-side ID of the ordering-phase session */
	enum { ORDER_SESSION_ID = 60 };

	enum {
		/* observation window inside the held provider close */
		ORDERING_WINDOW_MS = 500
	};

	/**
	 * Service that takes session requests but never answers them
	 *
	 * The create is accepted (the "parent-side" ID is allocated) but the
	 * capability never arrives, and the close of such a session stays in
	 * flight forever - exactly the situation the create deadline exists for.
	 *
	 * The only exception is 'complete_close', the close-completion helper
	 * of the close-race phase: it lets the test answer one pending close so
	 * that the completion path ('Child::_resolve_pending_op' with
	 * 'Parent_state::Gone') runs deterministically. The never-answered
	 * create behavior is unchanged.
	 */
	struct Pending_service : Service
	{
		Id_space<Parent::Client> _parent_ids { };

		Session_state const *_close_to_complete = nullptr;

		Pending_service() : Service("Pending") { }

		Pending_service(Pending_service const &) = delete;
		Pending_service &operator = (Pending_service const &) = delete;

		Initiate_result initiate_request(Session_state &session) override
		{
			if (session.phase == Session_state::CREATE_REQUESTED
			 && !session.id_at_parent.constructed())
				session.id_at_parent.construct(session.parent_client, _parent_ids);

			/*
			 * The completion of a request wakes the client's shared session
			 * blockade only for sessions marked as asynchronously notified.
			 * The create-expiry denial of a session must reach the client
			 * this way (also retroactively, via the 'session_sigh'
			 * re-delivery), because it is the spurious wake-up that releases
			 * the buggy 'Component_env::close' early.
			 */
			session.async_client_notify = true;

			/*
			 * The close-race phase establishes its race session explicitly
			 * ('establish=1'): the create is answered immediately so that
			 * the session is alive when it is closed. The capability is
			 * left invalid - no one requests it. Without this opt-in the
			 * create is taken but never answered, which is the
			 * create-expiry behavior the first phase depends on.
			 */
			if (session.phase == Session_state::CREATE_REQUESTED
			 && Arg_string::find_arg(session.args().string(),
			                         "establish").bool_value(false))
				session.phase = Session_state::AVAILABLE;

			/* the request is taken but never answered */
			return Ok();
		}

		Parent_state probe_at_parent(Session_state &session) override
		{
			/*
			 * A close that the test completed is gone at the parent. The
			 * pending-operation resolution then destroys the session state
			 * and releases the parent-side ID.
			 */
			if (_close_to_complete && &session == _close_to_complete) {
				_close_to_complete = nullptr;
				return Parent_state::Gone;
			}

			return Parent_state::Pending;
		}

		bool complete_create(Session_state &) override { return false; }

		/**
		 * Close-completion helper: answer the close of 'session' on the
		 * next 'Child::expire_pending_ops' tick
		 */
		void complete_close(Session_state const &session)
		{
			_close_to_complete = &session;
		}
	};


	/**
	 * Root whose 'close' holds - the delayed provider close under test
	 *
	 * The child role announces this root, which activates its 'Root_proxy'.
	 * A close request then runs 'Root_client::close' against this object:
	 * the hold below is the provider latency the close response must not
	 * wait for. The parent releases the hold by disposing of the signal
	 * session ('PROBE_SIGNAL_ID'), so the delay is deterministic and needs
	 * no timer session in the child. The session capability is a
	 * placeholder: only the close dispatch and its ordering are under
	 * test. The completed close is flagged so the child role can hand the
	 * parent a marker session afterwards.
	 */
	struct Slow_close_root : Rpc_object<Root>
	{
		Env &_env;

		bool volatile _closed = false;

		Slow_close_root(Env &env) : _env(env) { }

		Result session(Session_args const &, Affinity const &) override
		{
			return Session_capability();
		}

		void upgrade(Session_capability, Upgrade_args const &) override { }

		void close(Session_capability) override
		{
			log("child-pending-op: probe close entered, holding");

			/*
			 * The delayed provider close: the hold lasts until the
			 * parent disposed of the signal session. The response
			 * ordering may not replace this call with a mere response
			 * drop - the close must still run.
			 */
			for (unsigned i = 0; i < 1000000; i++) {

				bool released = false;

				_env.parent().session_cap(
					Parent::Client::Id { PROBE_SIGNAL_ID }).with_result(
						[&] (Capability<Session>) { },
						[&] (Parent::Session_cap_error) { released = true; });

				if (released)
					break;
			}

			log("child-pending-op: probe close released, cleaning up");

			_closed = true;
		}

		bool closed() const { return _closed; }
	};


	struct Test_policy : Child_policy, Async_service::Wakeup
	{
		Env             &_env;
		Rpc_entrypoint  &_ep;
		Pending_service &_pending_service;

		Genode::Parent_service _pd_service  { _env, "PD"  };
		Genode::Parent_service _cpu_service { _env, "CPU" };
		Genode::Parent_service _log_service { _env, "LOG" };
		Genode::Parent_service _rom_service { _env, "ROM" };
		Genode::Parent_service _timer_service { _env, "Timer" };

		/*
		 * Provides the "session_requests" ROM to the child role and owns
		 * the server-side ID space of sessions at child-provided services
		 * ('ProxyProbe'): the parent writes request nodes here, the
		 * child's 'Root_proxy' answers them via 'session_response' and
		 * 'deliver_session_cap'.
		 */
		Session_requester _session_requester { _ep, _env.ram(), _env.rm() };

		/* routed to the child role's 'ProxyProbe' root */
		Constructible<Async_service> _probe_service { };

		/* RAM quota of the child PD; also the parent/child role threshold */
		static constexpr Ram_quota CHILD_RAM_QUOTA { 1*1024*1024 };

		Pd_session_capability _child_pd_cap { };

		Genode::Child *_child = nullptr;

		bool _child_exited = false;
		int  _child_exit_value = -1;

		Test_policy(Env &env, Pending_service &pending_service, Rpc_entrypoint &ep)
		: _env(env), _ep(ep), _pending_service(pending_service) { }

		Test_policy(Test_policy const &) = delete;
		Test_policy &operator = (Test_policy const &) = delete;

		void child(Genode::Child &child) { _child = &child; }

		Name        name()        const override { return "child_pending_op"; }
		Binary_name binary_name() const override { return "test-child_pending_op"; }

		Ram_allocator &session_md_ram() override { return _env.ram(); }
		Pd_account    &ref_account()    override { return _env.pd(); }

		Capability<Pd_account> ref_account_cap() const override
		{
			return _env.pd_session_cap();
		}

		void init(Pd_session &session, Pd_session_capability cap) override
		{
			session.ref_account(_env.pd_session_cap());

			_child_pd_cap = cap;

			_env.pd().transfer_quota(cap, CHILD_RAM_QUOTA);
			_env.pd().transfer_quota(cap, Cap_quota { 20 });
		}

		/*
		 * The child role allocates its session-blockade signal context at
		 * 'Env::close' time and may run out of cap quota there. The request
		 * is granted from this component's own donation and the child is
		 * woken, exactly as a resource-providing parent would do - without
		 * it the child would block forever on the resource response.
		 */
		void resource_request(Parent::Resource_args const &args) override
		{
			Ram_quota const ram { ram_quota_from_args(args.string()) };
			Cap_quota const cap { cap_quota_from_args(args.string()) };

			if (_child_pd_cap.valid()) {
				if (ram.value) _env.pd().transfer_quota(_child_pd_cap, ram);
				if (cap.value) _env.pd().transfer_quota(_child_pd_cap, cap);
			}

			if (_child)
				_child->notify_resource_avail();
		}

		/*
		 * Only the PD environment session is established at construction
		 * time. The remaining environment sessions (which start the child
		 * process) are initiated manually, after the create-expiry phase.
		 */
		bool initiate_env_sessions() const override { return false; }

		void init_probe_service(Session_state::Factory &server_factory)
		{
			_probe_service.construct(Service::Name("ProxyProbe"),
			                         _session_requester.id_space(),
			                         server_factory, *this);
		}

		void wakeup_async_service() override
		{
			log("child-pending-op: proxy wakeup");
			_session_requester.trigger_update();
		}

		void _with_route(Service::Name     const &name,
		                 Session_label     const &label,
		                 With_route::Ft    const &fn,
		                 With_no_route::Ft const &denied_fn) override
		{
			if (name == "Pending") { fn(Route { _pending_service, label }); return; }
			if (name == "ProxyProbe" && _probe_service.constructed()) {
				fn(Route { *_probe_service, label }); return; }
			if (name == "PD")      { fn(Route { _pd_service,      label }); return; }
			if (name == "CPU")     { fn(Route { _cpu_service,     label }); return; }
			if (name == "LOG")     { fn(Route { _log_service,     label }); return; }
			if (name == "Timer")   { fn(Route { _timer_service,   label }); return; }
			if (name == "ROM") {
				if (label.last_element() == Session_requester::rom_name()) {
					fn(Route { _session_requester.service(), label });
					return;
				}
				fn(Route { _rom_service, label });
				return;
			}
			denied_fn();
		}

		Id_space<Parent::Server> &server_id_space() override
		{
			return _session_requester.id_space();
		}

		void exit(int exit_value) override
		{
			_child_exit_value = exit_value;
			_child_exited = true;
		}
	};


	/**
	 * Child role: drive 'Env::close' against the in-process parent
	 *
	 * This runs as a real child process of 'Main' (see 'Component::construct'
	 * below). Its 'Env::close' is 'Component_env::close' - the code under
	 * test - and its parent is the in-process 'Genode::Child' whose service
	 * is 'Pending_service'.
	 */
	void main_child(Env &env)
	{
		log("child-pending-op: child role started");

		Entrypoint  probe_ep { env, 32*1024, "probe_ep", Affinity::Location() };
		Slow_close_root probe_root { env };

		/*
		 * The signal session is the release channel of the delayed
		 * provider close: 'Slow_close_root::close' holds until the
		 * parent disposes of it. It is taken but never answered.
		 */
		Session_capability signal_cap { };
		env.try_session("Pending", Parent::Client::Id { PROBE_SIGNAL_ID },
		                Parent::Session_args("ram_quota=128K, cap_quota=0"),
		                Affinity::unrestricted(), signal_cap);

		/*
		 * The announce activates the child's 'Root_proxy', which answers
		 * the parent's "session_requests" ROM nodes at this root. The
		 * root is served on 'probe_ep' so that its slow 'close' never
		 * blocks this thread.
		 */
		Parent::announce(Parent::Service_name("ProxyProbe"),
		                 probe_ep.manage(probe_root));

		log("child-pending-op: probe root announced");

		/*
		 * Ordering phase (parent role '_close_ordering_phase'): the
		 * parent creates and closes a 'ProxyProbe' session and releases
		 * the held close by disposing of the signal session. Wait for
		 * the released close to have run to completion.
		 */
		for (unsigned i = 0; i < 1000000 && !probe_root.closed(); i++) {

			/* the IPC poll yields this thread; no timer session needed */
			env.parent().session_cap(
				Parent::Client::Id { PROBE_SIGNAL_ID }).with_result(
					[&] (Capability<Session>) { },
					[&] (Parent::Session_cap_error) { });
		}

		if (!probe_root.closed()) {
			error("child-pending-op: probe close never ran");
			env.parent().exit(3);
			return;
		}

		/*
		 * The marker session tells the parent that the delayed provider
		 * close ran to completion ('Root_client::close' is preserved by
		 * the response-ordering fix). It is taken but never answered -
		 * the parent disposes of it explicitly.
		 */
		Session_capability marker_cap { };
		env.try_session("Pending", Parent::Client::Id { PROBE_MARKER_ID },
		                Parent::Session_args("ram_quota=128K, cap_quota=0"),
		                Affinity::unrestricted(), marker_cap);

		Parent::Client::Id const id      { RACE_SESSION_ID };
		Parent::Client::Id const wake_id { WAKE_SESSION_ID };

		/*
		 * The race sessions donate minimal cap quota: the scenario's
		 * fixed cap budget must cover the fixture sessions too, and the
		 * mock provider accounts no quota.
		 */
		Parent::Session_args const args("ram_quota=128K, cap_quota=1");
		Parent::Session_args const established_args(
			"establish=1, ram_quota=128K, cap_quota=1");

		Session_capability cap { };

		/*
		 * The race session is established (the provider answers the create),
		 * the wake session is taken but never answered - its create expiry
		 * is the spurious wake-up below.
		 */
		env.try_session("Pending", id, established_args,
		                Affinity::unrestricted(), cap);
		env.try_session("Pending", wake_id, args,
		                Affinity::unrestricted(), cap);

		/*
		 * Wait for the create expiry of the wake session: the parent
		 * releases it with a denial, which is the spurious wake-up of the
		 * shared session blockade below.
		 */
		bool const wake_denied = [&] () -> bool {
			for (unsigned i = 0; i < 1000000; i++) {
				bool denied = false, missing = false;

				env.parent().session_cap(wake_id).with_result(
					[&] (Capability<Session>) { },
					[&] (Parent::Session_cap_error e) {
						denied  = (e == Parent::Session_cap_error::DENIED);
						missing = true; });

				if (missing)
					return denied;
			}
			return false;
		}();

		if (!wake_denied) {
			error("child-pending-op: wake session was not denied");
			env.parent().exit(2);
			return;
		}

		/*
		 * The code under test: close the session while the parent keeps it
		 * in CLOSE_REQUESTED. The shared blockade holds the wake-up of the
		 * denied wake session, so the pre-fix 'close' consumes that
		 * unrelated signal and returns before the close completed.
		 */
		env.close(id);

		/* immediate same-ID create: refused with a conflicting ID pre-fix */
		Env::Session_request_result const again =
			env.try_session("Pending", id, args, Affinity::unrestricted(), cap);

		bool const accepted = (again != Env::Session_request_result::DENIED);

		log("child-pending-op: same-ID create ",
		    accepted ? "accepted" : "DENIED (requested conflicting session ID)");

		env.parent().exit(accepted ? 0 : 1);
	}


	struct Main
	{
		Env &_env;

		/*
		 * The child's parent interface must be served while this
		 * constructor orchestrates the race ('Component::construct' runs on
		 * the main entrypoint), so the child gets its own entrypoint. The
		 * policy's 'Session_requester' serves the child's "session_requests"
		 * ROM on it, hence the entrypoint is constructed first.
		 */
		Entrypoint _child_ep { _env, 32*1024, "child_ep", Affinity::Location() };

		Pending_service _pending_service { };
		Test_policy     _policy          { _env, _pending_service, _child_ep.rpc_ep() };

		Child _child { _env.rm(), _child_ep.rpc_ep(), _policy };

		/*
		 * One timer connection for all phases: each connection draws
		 * cap quota from the scenario's fixed budget.
		 */
		Constructible<Timer::Connection> _timer { };

		unsigned _failures = 0;

		void check(bool condition, char const *what)
		{
			log("child-pending-op: ", condition ? "PASS " : "FAIL ", what);
			if (!condition)
				_failures++;
		}

		unsigned session_count() const
		{
			unsigned count = 0;
			_child.for_each_session([&] (Session_state const &session) {
				if (session.service().name() == Service::Name("Pending"))
					count++; });
			return count;
		}

		void log_session_error(Session_error e)
		{
			switch (e) {
			case Session_error::DENIED:
				log("child-pending-op: session() error DENIED"); break;
			case Session_error::OUT_OF_RAM:
				log("child-pending-op: session() error OUT_OF_RAM"); break;
			case Session_error::OUT_OF_CAPS:
				log("child-pending-op: session() error OUT_OF_CAPS"); break;
			case Session_error::INSUFFICIENT_RAM:
				log("child-pending-op: session() error INSUFFICIENT_RAM"); break;
			case Session_error::INSUFFICIENT_CAPS:
				log("child-pending-op: session() error INSUFFICIENT_CAPS"); break;
			}
		}

		Session_state const *_session_with_id(Parent::Client::Id id) const
		{
			Session_state const *result = nullptr;
			_child.for_each_session([&] (Session_state const &session) {
				if (session.id_at_client().value == id.value)
					result = &session; });
			return result;
		}

		/*
		 * Complete a close that is already in flight: the probe reports
		 * 'Gone' and the next tick destroys the session state.
		 */
		void _complete_pending_close(Parent::Client::Id id)
		{
			Session_state const *session = _session_with_id(id);
			if (!session)
				return;

			_pending_service.complete_close(*session);
			_child.expire_pending_ops(1);
		}

		void _close_and_complete(Parent::Client::Id id)
		{
			if (_child.close(id) == Parent::Close_result::DONE)
				return;

			_complete_pending_close(id);
		}

		void _create_expiry_phase()
		{
			Parent::Client::Id const id { 42 };

			/*
			 * The provider takes the create but never answers it: the
			 * session state is created and the create operation is
			 * registered as pending at the parent.
			 */
			_child.session(id, "Pending",
			               Parent::Session_args("ram_quota=128K, cap_quota=8"),
			               Affinity::unrestricted())
				.with_result(
					[&] (Capability<Session> const &) {
						log("child-pending-op: session() accepted"); },
					[&] (Session_error e) { log_session_error(e); });

			check(session_count() == 1, "pending session state created");

			/* drive the create into its deadline */
			_child.expire_pending_ops(60000);

			check(_child.pending_parent_ops(),
			      "close op survives create expiry (pending_parent_ops)");

			check(session_count() == 1,
			      "session state kept while close is in flight");

			bool const denied = _child.session_cap(id).convert<bool>(
				[&] (Capability<Session> const &) {
					return false; },
				[&] (Parent::Session_cap_error e) {
					return e == Parent::Session_cap_error::DENIED; });

			check(denied, "session_cap reports the denial");

			check(session_count() == 1,
			      "session state not destroyed by session_cap "
			      "while close is in flight");

			/*
			 * The completion path: the provider answers the pending close,
			 * the state is destroyed, and the parent-side ID is free again.
			 * An immediate same-ID create is accepted without a
			 * "requested conflicting session ID" refusal.
			 */
			_complete_pending_close(id);

			check(session_count() == 0,
			      "close completion destroys the session state");

			bool const accepted = _child.session(id, "Pending",
				Parent::Session_args("ram_quota=128K, cap_quota=8"),
				Affinity::unrestricted())
				.convert<bool>(
					[&] (Capability<Session> const &) { return true; },
					[&] (Session_error)               { return false; });

			check(accepted,
			      "same-ID create accepted after completed close");

			/* drop the accepted (never answered) create again */
			_close_and_complete(id);
		}

		/**
		 * Phase 3 - the parent-side close/create collision
		 *
		 * This is the field failure that survived the 'Component_env::close'
		 * fix: a create arriving under a client-side ID whose old session is
		 * still CLOSE_REQUESTED ('state=CLOSING') died in 'with_new_session'
		 * with 'requested conflicting session ID' - a component-fatal
		 * denial. The collision is driven at 'with_new_session' directly (via
		 * 'Child::session'), not through 'Component_env::close': the
		 * re-issue of a freed ID is the client's own ID allocation
		 * ('_unused_id' re-issues the last freed ID), the parent must
		 * tolerate the create under the closing ID nonetheless.
		 *
		 * The old close completes LATE (the provider answers nothing until
		 * the test completes the close explicitly): the colliding create
		 * must become pending and complete once the late close destroys the
		 * old state - the close may not be rushed, and the create may not
		 * be accepted into the still-mapped ID (that would leak the old
		 * state).
		 */
		void _create_collision_phase()
		{
			Parent::Client::Id const id { 55 };

			Parent::Session_args const established_args(
				"establish=1, ram_quota=128K, cap_quota=8");

			bool const created = _child.session(id, "Pending", established_args,
			                                    Affinity::unrestricted())
				.convert<bool>(
					[&] (Capability<Session> const &) { return true; },
					[&] (Session_error)               { return false; });

			check(created, "collision: established session created");

			/*
			 * The close is taken by the provider but never answered: the
			 * session stays in CLOSE_REQUESTED - the field's 'state=CLOSING'.
			 */
			Parent::Close_result const close_result = _child.close(id);

			check(close_result == Parent::Close_result::PENDING,
			      "collision: close stays in flight");

			Session_state const *old = _session_with_id(id);

			check(old && old->phase == Session_state::CLOSE_REQUESTED,
			      "collision: old session is state=CLOSING");

			if (old)
				log("child-pending-op: collision old state ", *old);

			/*
			 * The colliding create: pre-fix this dies in 'with_new_session'
			 * with 'requested conflicting session ID' and is denied. The
			 * fix parks the create and answers pending instead.
			 */
			bool denied = false, accepted = false;

			_child.session(id, "Pending", established_args,
			               Affinity::unrestricted())
				.with_result(
					[&] (Capability<Session> const &) { accepted = true; },
					[&] (Session_error)               { denied  = true; });

			log("child-pending-op: colliding create ",
			    denied ? "DENIED (requested conflicting session ID)" : "tolerated");

			check(!denied,
			      "collision: create under state=CLOSING is not denied");

			check(accepted,
			      "collision: create under state=CLOSING is pending");

			/* the old close is untouched by the colliding create */
			check(_child.pending_parent_ops(),
			      "collision: old close still in flight");

			/*
			 * The close completes LATE: the old state is destroyed and the
			 * parked create is re-driven under the same ID.
			 */
			_complete_pending_close(id);

			Session_state const *fresh = _session_with_id(id);

			check(fresh != nullptr && fresh->phase != Session_state::CLOSE_REQUESTED,
			      "collision: parked create completed after the late close");

			check(session_count() == 1,
			      "collision: exactly the parked session remains");

			_close_and_complete(id);
		}

		/**
		 * Phase 4 - the Root_proxy close-response ordering
		 *
		 * The child role provides 'ProxyProbe' via its 'Root_proxy'; its
		 * root holds 'close' until this phase releases it. The 'CLOSED'
		 * response must arrive - and the same-ID create must complete -
		 * while that provider close is still held. Pre-fix the answer is
		 * held hostage by 'Root_client::close': the state stays in
		 * CLOSE_REQUESTED and the same-ID create stays parked past the
		 * observation window.
		 */
		void _close_ordering_phase()
		{
			if (!_timer.constructed())
				_timer.construct(_env);

			Parent::Client::Id const id { ORDER_SESSION_ID };

			/*
			 * No quota donation: the ordering sessions are fixtures
			 * and must not add to the scenario's cap pressure.
			 */
			Parent::Session_args const args("ram_quota=128K, cap_quota=0");
			Parent::Session_args const established_args(
				"establish=1, ram_quota=128K, cap_quota=0");

			bool const requested =
				_child.session(id, "ProxyProbe", args, Affinity::unrestricted())
					.convert<bool>(
						[&] (Capability<Session> const &) { return true; },
						[&] (Session_error)               { return false; });

			check(requested, "ordering: ProxyProbe session request accepted");

			bool available = false;
			for (unsigned i = 0; i < 2000 && !available; i++) {

				Session_state const *session = _session_with_id(id);
				available = session
				         && session->phase == Session_state::AVAILABLE;
				if (!available)
					_timer->msleep(5);
			}

			check(available,
			      "ordering: ProxyProbe session established via session_requests");

			Parent::Close_result const close_result = _child.close(id);

			check(close_result == Parent::Close_result::PENDING,
			      "ordering: close is in flight at the provider");

			/*
			 * The observation window: the provider close is held.
			 * The response and the same-ID create must be through
			 * regardless.
			 */
			_timer->msleep(ORDERING_WINDOW_MS);

			check(!_child.pending_parent_ops(),
			      "ordering: CLOSED response arrived during delayed "
			      "provider close");

			check(_session_with_id(id) == nullptr,
			      "ordering: session state released during delayed "
			      "provider close");

			bool denied = false, accepted = false;

			_child.session(id, "Pending", established_args,
			               Affinity::unrestricted())
				.with_result(
					[&] (Capability<Session> const &) { accepted = true; },
					[&] (Session_error)               { denied  = true; });

			check(!denied,
			      "ordering: same-ID create not denied during delayed "
			      "provider close");

			Session_state const *fresh = _session_with_id(id);

			check(accepted && fresh
			   && fresh->phase == Session_state::AVAILABLE,
			      "ordering: same-ID create completed during delayed "
			      "provider close");

			/* release the held provider close */
			_close_and_complete(Parent::Client::Id { PROBE_SIGNAL_ID });

			/*
			 * Settle: pre-fix the parked create completes only when the
			 * released close finally answers. Dispose of whichever
			 * create is live so the session set is empty afterwards.
			 */
			fresh = nullptr;
			for (unsigned i = 0; i < 1000; i++) {

				fresh = _session_with_id(id);
				if (fresh && fresh->phase == Session_state::AVAILABLE)
					break;
				_timer->msleep(5);
			}

			if (fresh && fresh->phase == Session_state::AVAILABLE)
				_close_and_complete(id);
			else
				warning("child-pending-op: ordering session not settled");

			/*
			 * The delayed provider close must have run to completion:
			 * the child role reports it with a marker session once
			 * 'Slow_close_root::close' returned.
			 */
			bool close_ran = false;
			for (unsigned i = 0; i < 2000 && !close_ran; i++) {

				close_ran = _session_with_id(
					Parent::Client::Id { PROBE_MARKER_ID }) != nullptr;
				if (!close_ran)
					_timer->msleep(5);
			}

			check(close_ran,
			      "ordering: provider close ran to completion "
			      "(Root_client.close preserved)");

			_close_and_complete(Parent::Client::Id { PROBE_MARKER_ID });
		}

		void _close_race_phase()
		{
			/* the child process is started before the ordering phase */

			/* wait for the child's two pending creates */
			bool saw_creates = false;
			for (unsigned i = 0; i < 2000 && !saw_creates; i++) {

				unsigned ready = 0;
				_child.for_each_session([&] (Session_state const &session) {
					unsigned long const cid = session.id_at_client().value;
					if (cid == RACE_SESSION_ID)
						ready++;
					if (cid == WAKE_SESSION_ID
					 && session.phase == Session_state::CREATE_REQUESTED)
						ready++; });

				saw_creates = (ready == 2);
				if (!saw_creates)
					_timer->msleep(5);
			}

			check(saw_creates, "close race: child created both sessions");

			/*
			 * Expire the wake session's create: the existing create-expiry
			 * behavior releases the client with a denial and submits the
			 * shared session signal (or re-delivers it when the child
			 * registers its blockade) - the spurious wake-up for the close.
			 */
			_child.expire_pending_ops(60001);

			/* wait for the child's close of the race session to be in flight */
			bool close_in_flight = false;
			for (unsigned i = 0; i < 2000 && !close_in_flight; i++) {

				Session_state const *session =
					_session_with_id(Parent::Client::Id { RACE_SESSION_ID });

				close_in_flight = session
				               && session->phase == Session_state::CLOSE_REQUESTED;
				if (!close_in_flight)
					_timer->msleep(5);
			}

			check(close_in_flight, "close race: deferred close is in flight");

			/*
			 * The child reacts to the wake-up in microseconds; give it time
			 * to run into the conflict before the close completes below.
			 */
			_timer->msleep(300);

			bool const exited_early = _policy._child_exited;

			/* the close completion: probe 'Gone' -> destroy -> release ID */
			_complete_pending_close(Parent::Client::Id { RACE_SESSION_ID });

			/* wait for the child role to report its result */
			for (unsigned i = 0; i < 2000 && !_policy._child_exited; i++)
				_timer->msleep(5);

			check(_policy._child_exited,
			      "close race: child role finished");

			check(_policy._child_exited && _policy._child_exit_value == 0,
			      "close race: same-ID create accepted after completed close");

			check(!exited_early,
			      "close race: Env::close did not return before the "
			      "parent completed the close");
		}

		Main(Env &env) : _env(env)
		{
			_policy.child(_child);

			/* only the PD environment session is established */
			_child.initiate_env_pd_session();

			_policy.init_probe_service(_child.session_factory());

			_create_expiry_phase();
			_create_collision_phase();

			/*
			 * Start the child process (the remaining environment
			 * sessions complete its environment). The ordering phase
			 * needs the child role's 'Root_proxy', the close-race phase
			 * has the child role drive 'Env::close' against this object.
			 */
			_child.initiate_env_sessions();

			_close_ordering_phase();
			_close_race_phase();

			log("child-pending-op: VERDICT ", _failures ? "FAIL" : "PASS");

			env.parent().exit(_failures ? 1 : 0);
		}
	};

} /* anonymous namespace */


void Component::construct(Env &env)
{
	/*
	 * The parent role owns more RAM than the child PD (see
	 * 'Test_policy::CHILD_RAM_QUOTA'): this program starts itself as child,
	 * and the available RAM quota identifies the roles (the rm_fault
	 * pattern).
	 */
	if (env.pd().stats().ram.avail().value > Test_policy::CHILD_RAM_QUOTA.value) {
		static Main main(env);
	} else {
		main_child(env);
	}
}
