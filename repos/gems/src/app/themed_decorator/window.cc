/*
 * \brief  Example window decorator that mimics the Motif look
 * \author Norman Feske
 * \date   2014-01-10
 */

/*
 * Copyright (C) 2014-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* local includes */
#include "window.h"

Decorator::Window_base::Hover Decorator::Window::hover(Point abs_pos) const
{
	Hover hover;

	if (!_decor_geometry().contains(abs_pos))
		return hover;

	hover.window_id = id();

	/* omit the decoration checks below whenever the content is hovered */
	if (geometry().contains(abs_pos))
		return hover;

	/*
	 * Sizer bands are evaluated BEFORE the control/title checks,
	 * mirroring the motif decorator's border-first semantics
	 * (decorator/window.cc:310-325). The themed title strip spans
	 * the full width of the frame's top band, so a title-first
	 * order would make the top edge ungrabbable for resize.
	 */
	{
		/*
		 * Sizer band computation uses _decor_geometry() (the
		 * decor band = the frame's interactive region), NOT
		 * outer_geometry() (which also includes the aura
		 * drop-shadow the hover gate above already excludes).
		 * Using the aura-inclusive outer would place the band
		 * in the non-hoverable shadow region.
		 */
		Rect const frame = _decor_geometry();

		int const x = abs_pos.x;
		int const y = abs_pos.y;

		int const border = 4, corner = 16;

		bool const at_border = x < frame.x1() + border
		                    || x > frame.x2() - border
		                    || y < frame.y1() + border
		                    || y > frame.y2() - border;

		if (at_border) {
			hover.left_sizer   = x < frame.x1() + corner;
			hover.right_sizer  = x > frame.x2() - corner;
			hover.top_sizer    = y < frame.y1() + corner;
			hover.bottom_sizer = y > frame.y2() - corner;
			return hover;
		}
	}

	Rect const closer_geometry =
		_theme.absolute(_theme.element_geometry(Theme::ELEMENT_TYPE_CLOSER),
		                                        outer_geometry());
	if (_closer.present() && closer_geometry.contains(abs_pos)) {
		hover.closer = true;
		return hover;
	}

	Rect const maximizer_geometry =
		_theme.absolute(_theme.element_geometry(Theme::ELEMENT_TYPE_MAXIMIZER),
		                                        outer_geometry());
	if (_maximizer.present() && maximizer_geometry.contains(abs_pos)) {
		hover.maximizer = true;
		return hover;
	}

	Rect const minimizer_geometry =
		_theme.absolute(_theme.element_geometry(Theme::ELEMENT_TYPE_MINIMIZER),
		                                        outer_geometry());
	if (_minimizer.present() && minimizer_geometry.contains(abs_pos)) {
		hover.minimizer = true;
		return hover;
	}

	Rect const title_geometry = _theme.absolute(_theme.title_geometry(),
	                                            outer_geometry());
	if (title_geometry.contains(abs_pos)) {
		hover.title = true;
		return hover;
	}

	return hover;
}
