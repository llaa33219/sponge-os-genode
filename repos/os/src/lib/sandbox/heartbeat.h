/*
 * \brief  Heartbeat monitoring
 * \author Norman Feske
 * \date   2018-11-15
 */

/*
 * Copyright (C) 2018 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _LIB__SANDBOX__HEARTBEAT_H_
#define _LIB__SANDBOX__HEARTBEAT_H_

/* local includes */
#include <state_reporter.h>
#include <child_registry.h>
#include <util/noncopyable.h>

namespace Sandbox { class Heartbeat; }


class Sandbox::Heartbeat : Noncopyable
{
	private:

		Env &_env;

		Child_registry &_children;

		Report_update_trigger &_report_update_trigger;

		Constructible<Timer::Connection> _timer { };

		uint64_t _rate_ms = 0;

		/*
		 * Milliseconds elapsed since the heartbeat timer was armed. The
		 * sandbox has no wall clock of its own, so the heartbeat period is
		 * the clock: the startup grace of a child is measured in heartbeat
		 * periods.
		 */
		uint64_t _elapsed_ms = 0;

		Signal_handler<Heartbeat> _timer_handler;

		void _handle_timer()
		{
			bool any_skipped_heartbeats = false;

			_elapsed_ms += _rate_ms;

			_children.for_each_child([&] (Child &child) {

				if (child.skipped_heartbeats())
					any_skipped_heartbeats = true;

				child.heartbeat((unsigned)_elapsed_ms, (unsigned)_rate_ms);
			});

			if (any_skipped_heartbeats)
				_report_update_trigger.trigger_report_update();
		}

	public:

		Heartbeat(Env &env, Child_registry &children,
		          Report_update_trigger &report_update_trigger)
		:
			_env(env), _children(children),
			_report_update_trigger(report_update_trigger),
			_timer_handler(_env.ep(), *this, &Heartbeat::_handle_timer)
		{ }

		void disable()
		{
			_timer.destruct();
			_rate_ms = 0;
		}

		void apply_config(Node const &heartbeat)
		{
			if (!_timer.constructed()) {
				_timer.construct(_env);
				_timer->sigh(_timer_handler);
			}

			unsigned const rate_ms = heartbeat.attribute_value("rate_ms", 1000U);

			if (rate_ms != _rate_ms) {
				_rate_ms = rate_ms;
				_timer->trigger_periodic(_rate_ms*1000);
			}
		}
};

#endif /* _LIB__SANDBOX__HEARTBEAT_H_ */
