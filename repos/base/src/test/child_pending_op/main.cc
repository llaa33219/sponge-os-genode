/*
 * \brief  Regression test for the create-expiry teardown of pending sessions
 * \author Sponge OS contributors
 * \date   2026-11-06
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
 */

/* Genode includes */
#include <base/component.h>
#include <base/child.h>
#include <base/log.h>

using namespace Genode;

namespace {

/**
 * Service that takes session requests but never answers them
 *
 * The create is accepted (the "parent-side" ID is allocated) but the
 * capability never arrives, and the close of such a session stays in
 * flight forever - exactly the situation the create deadline exists for.
 */
struct Pending_service : Service
{
	Id_space<Parent::Client> _parent_ids { };

	Pending_service() : Service("Pending") { }

	Initiate_result initiate_request(Session_state &session) override
	{
		if (session.phase == Session_state::CREATE_REQUESTED
		 && !session.id_at_parent.constructed())
			session.id_at_parent.construct(session.parent_client, _parent_ids);

		/* the request is taken but never answered */
		return Ok();
	}

	Parent_state probe_at_parent(Session_state &) override
	{
		return Parent_state::Pending;
	}

	bool complete_create(Session_state &) override { return false; }
};


struct Test_policy : Child_policy
{
	Env             &_env;
	Pending_service &_pending_service;

	Genode::Parent_service _pd_service { _env, "PD" };

	Id_space<Parent::Server> _server_ids { };

	Test_policy(Env &env, Pending_service &pending_service)
	: _env(env), _pending_service(pending_service) { }

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

		_env.pd().transfer_quota(cap, Ram_quota { 1*1024*1024 });
		_env.pd().transfer_quota(cap, Cap_quota { 20 });
	}

	/*
	 * Only the PD environment session is needed to drive 'Child::session'.
	 * The remaining environment sessions would start a child process,
	 * which this test does not want.
	 */
	bool initiate_env_sessions() const override { return false; }

	void _with_route(Service::Name     const &name,
	                 Session_label     const &label,
	                 With_route::Ft    const &fn,
	                 With_no_route::Ft const &denied_fn) override
	{
		if (name == "Pending") { fn(Route { _pending_service, label }); return; }
		if (name == "PD")      { fn(Route { _pd_service,      label }); return; }
		denied_fn();
	}

	Id_space<Parent::Server> &server_id_space() override { return _server_ids; }
};


struct Main
{
	Env &_env;

	Pending_service _pending_service { };
	Test_policy     _policy          { _env, _pending_service };

	Child _child { _env.rm(), _env.ep().rpc_ep(), _policy };

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

	Main(Env &env) : _env(env)
	{
		/* only the PD environment session is established */
		_child.initiate_env_pd_session();

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

		log("child-pending-op: VERDICT ", _failures ? "FAIL" : "PASS");

		env.parent().exit(_failures ? 1 : 0);
	}
};

} /* anonymous namespace */


void Component::construct(Env &env)
{
	static Main main(env);
}
