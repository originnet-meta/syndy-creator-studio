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

#include <widgets/StartupSplashWidget.hpp>

#include <QApplication>

#include <stdio.h>

static bool expect(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "startup-splash-widget-test: %s\n", message);

	return condition;
}

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	StartupSplashWidget widget;
	bool ok = true;

	widget.SetStatusText("Loading core systems");
	widget.SetModuleName("obs-browser");
	widget.SetStepText("Modules");
	widget.SetProgressPercent(42);
	app.processEvents();

	ok &= expect(widget.StatusText() == "Loading core systems", "status text update failed");
	ok &= expect(widget.ModuleName() == "obs-browser", "module text update failed");
	ok &= expect(widget.StepText() == "Modules", "step text update failed");
	ok &= expect(widget.ProgressPercent() == 42, "progress update failed");

	widget.UpdateState("Initializing UI", "obs-websocket", 87, "Finalize");
	app.processEvents();

	ok &= expect(widget.StatusText() == "Initializing UI", "combined status update failed");
	ok &= expect(widget.ModuleName() == "obs-websocket", "combined module update failed");
	ok &= expect(widget.StepText() == "Finalize", "combined step update failed");
	ok &= expect(widget.ProgressPercent() == 87, "combined progress update failed");

	widget.SetProgressPercent(150);
	ok &= expect(widget.ProgressPercent() == 100, "progress upper clamp failed");

	widget.SetProgressPercent(-5);
	ok &= expect(widget.ProgressPercent() == 0, "progress lower clamp failed");

	if (!ok)
		return 1;

	printf("startup-splash-widget-test: success\n");
	return 0;
}
