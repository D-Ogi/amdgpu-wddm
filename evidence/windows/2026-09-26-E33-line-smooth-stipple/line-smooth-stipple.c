/*
 * Copyright © 2022 Collabora Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Erik Faye-Lund <erik.faye-lund@collabora.com>
 *
 */

/*
 * Test the coverage of stippled multiple line-segments
 */

#include "piglit-util-gl.h"

PIGLIT_GL_TEST_CONFIG_BEGIN

	config.supports_gl_compat_version = 10;

	config.window_width = 300;
	config.window_height = 100;
	config.window_visual = PIGLIT_GL_VISUAL_RGB | PIGLIT_GL_VISUAL_DOUBLE;

PIGLIT_GL_TEST_CONFIG_END

float stipple_pattern(float counter, int factor)
{
	counter += 0.5;
	counter = counter - floor(counter / (2 * factor)) * 2 * factor;
	return MIN2(MAX2(fabs(counter - 8.0) - (factor * 0.5) + 0.5,
					 0.0f), 1.0f);
}

enum piglit_result
piglit_display(void)
{
	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_LINE_SMOOTH);
	/* GL AA lines produce an alpha value */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	glLineWidth(3.0f);
	glEnable(GL_LINE_STIPPLE);

	int y;
	for (y = 0; y * 4 < piglit_height; y++) {
		float offset = 1.0 + y / 4.0f;
		glLineStipple(8, 0x5555);
		glColor4f(0.5, 0.0, 0.5, 1.0);
		glBegin(GL_LINES);
		glVertex2f(offset, 1.5 + y * 4);
		glVertex2f(piglit_width - 1.0, 1.5 + y * 4);
		glEnd();

		glLineStipple(8, ~0x5555);
		glColor4f(0.0, 0.5, 0.0, 1.0);
		glBegin(GL_LINES);
		glVertex2f(offset, 1.5 + y * 4);
		glVertex2f(piglit_width - 1.0, 1.5 + y * 4);
		glEnd();
	}

	GLboolean pass = GL_TRUE;

	float *expected = malloc(piglit_width * sizeof(float) * 3);
	for (y = 0; y * 4 < piglit_height; y++) {
		float offset = 1.0 + y / 4.0f;
		int start_x = ceil(offset);
		for (int x = 0; x < piglit_width; ++x) {
			expected[x * 3 + 0] = stipple_pattern(x - offset - 4, 8) * 0.5;
			expected[x * 3 + 1] = stipple_pattern(x - offset + 4, 8) * 0.5;
			expected[x * 3 + 2] = stipple_pattern(x - offset - 4, 8) * 0.5;
		}
		pass = pass && piglit_probe_rect_rgb_varying(start_x, y * 4 + 1, piglit_width - start_x - 2, 1, expected + start_x * 3, 0);
	}
	free(expected);

	piglit_present_results();

	return pass ? PIGLIT_PASS : PIGLIT_FAIL;
}

void
piglit_init(int argc, char **argv)
{
	piglit_ortho_projection(piglit_width, piglit_height, GL_FALSE);
}
