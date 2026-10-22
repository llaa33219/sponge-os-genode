/*
 * \brief  Deadline of the session operations that the parent took but has
 *         not answered yet
 * \author Sponge OS contributors
 * \date   2026
 */

#ifndef _LIB__SANDBOX__PENDING_SESSION_OP_H_
#define _LIB__SANDBOX__PENDING_SESSION_OP_H_

/* Genode includes */
#include <base/affinity.h>
#include <base/child.h>
#include <base/id_space.h>
#include <base/log.h>
#include <timer_session/client.h>
#include <timer_session/connection.h>

/* local includes */
#include <child_registry.h>

namespace Sandbox { class Pending_op_timer; }


/**
 * Timer that advances the deadline of the pending session operations
 *
 * A session operation that the parent took but has not answered yet is
 * bounded by a deadline that 'Genode::Child' owns. This timer advances that
 * deadline, so an operation the parent never answers runs into its deadline
 * instead of waiting forever.
 *
 * The timer is armed while the sandbox has a Timer service. Without one, the
 * deadline cannot be advanced - the degradation is announced once, because a
 * silent unbounded wait is the very defect the deadline exists to prevent.
 */
class Sandbox::Pending_op_timer
{
	public:

		/**
		 * Period of the timer
		 *
		 * The period is the granularity of the deadline and the upper bound
		 * of the latency with which an operation the parent has already
		 * answered is resolved.
		 *
		 */
		static constexpr unsigned PERIOD_MS = 50;

	private:

		Env &_env;

		Child_registry &_children;

		/*
		 * Client-side ID of the Timer session. The ID is allocated when the
		 * timer is constructed because the session request needs one - the
		 * very same mechanism a 'Timer::Connection' uses.
		 */
		Parent::Client _parent_client { };

		Id_space<Parent::Client>::Element const _timer_id {
			_parent_client, _env.id_space()
		};

		Constructible<Timer::Session_capability> _timer_cap { };

		/**
		 * Client for the Timer session
		 *
		 * The generic 'Timer::Session_client' leaves the sleeping methods
		 * unimplemented - the sandbox needs the timer only as a clock, never
		 * as a sleep source.
		 */
		struct Timer_client : Timer::Session_client
		{
			explicit Timer_client(Timer::Session_capability cap)
			: Timer::Session_client(cap) { }

			void msleep(uint64_t) override
			{
				warning("Timer_client: msleep is not implemented - the "
				        "sandbox uses the timer as a clock only");
			}

			void usleep(uint64_t) override
			{
				warning("Timer_client: usleep is not implemented - the "
				        "sandbox uses the timer as a clock only");
			}
		};

		Constructible<Timer_client> _timer_client { };

		Signal_handler<Pending_op_timer> _timer_handler;

		bool _no_timer_reported = false;

		void _handle_timer()
		{
			_children.for_each_child([&] (Child &child) {
				child.expire_pending_ops(PERIOD_MS);
			});
		}

	public:

		Pending_op_timer(Env &env, Child_registry &children)
		:
			_env(env), _children(children),
			_timer_handler(_env.ep(), *this, &Pending_op_timer::_handle_timer)
		{ }

		/**
		 * Arm the timer
		 *
		 * The availability of a Timer service is determined the same way a
		 * 'Timer::Connection' would determine it: the session is requested
		 * from the parent and the parent's routing decides. In contrast to
		 * the connection, a denial is not fatal here - a sandbox without a
		 * Timer service must survive and report the degradation instead of
		 * taking the whole system down.
		 *
		 * The request is issued on the caller's thread, which is the
		 * sandbox's configuration path - never on a thread that serves the
		 * children's RPCs.
		 */
		void arm()
		{
			if (_timer_client.constructed())
				return;

			_env.try_session(Timer::Session::service_name(), _timer_id.id(),
			                 Parent::Session_args("ram_quota=10240, cap_quota=2"),
			                 Affinity()).with_result(
				[&] (Session_capability cap) {

					_timer_cap.construct(static_cap_cast<Timer::Session>(cap));
					_timer_client.construct(*_timer_cap);

					_timer_client->sigh(_timer_handler);
					_timer_client->trigger_periodic(PERIOD_MS*1000);
				},
				[&] (Session_error e) {
					if (_no_timer_reported)
						return;

					_no_timer_reported = true;
					warning("no Timer service (session request failed: ",
					        (int)e, ") - the deadline of the pending session "
					        "operations is unbounded");
				}
			);
		}

		/**
		 * Disarm the timer
		 */
		void disarm()
		{
			if (_timer_client.constructed())
				_timer_client.destruct();
		}
};

#endif /* _LIB__SANDBOX__PENDING_SESSION_OP_H_ */
