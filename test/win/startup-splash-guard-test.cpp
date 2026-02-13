/******************************************************************************
    Copyright (C) 2026 Uniflow, Inc.
    Author: Kim Taehyung <gaiaengine@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <utility/StartupSplashGuard.hpp>

#include <stdio.h>

static bool expect(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "startup-splash-guard-test: %s\n", message);

	return condition;
}

int main()
{
	bool ok = true;

	/* Default execution path: splash enabled when both guards allow it. */
	ok &= expect(OBS::IsStartupSplashEnabled(false, true), "default scenario should enable splash");

	/* CLI guard must force-disable splash even when config is enabled. */
	ok &= expect(!OBS::IsStartupSplashEnabled(true, true), "cli disable should disable splash");

	/* Config guard must disable splash even when CLI does not disable it. */
	ok &= expect(!OBS::IsStartupSplashEnabled(false, false), "config disable should disable splash");

	if (!ok)
		return 1;

	printf("startup-splash-guard-test: success\n");
	return 0;
}
