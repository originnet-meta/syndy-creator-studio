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

#include <utility/StartupProgressModel.hpp>

#include <stdio.h>

static bool expect(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "startup-progress-mapping-test: %s\n", message);

	return condition;
}

int main()
{
	bool ok = true;
	OBS::StartupProgressModel model;

	model.SetStage(OBS::StartupProgressStage::AppInitialized);
	model.SetStage(OBS::StartupProgressStage::LibobsInitialized);
	model.SetStage(OBS::StartupProgressStage::ModuleDiscovery);
	model.SetModuleCount(5);
	model.SetStage(OBS::StartupProgressStage::ModuleLoading);
	ok &= expect(model.Percent() == 35, "module loading base percent should be 35");

	model.MarkModuleFinished("module-1");
	ok &= expect(model.Percent() == 44, "1/5 modules should map to 44%");
	model.MarkModuleFinished("module-2");
	ok &= expect(model.Percent() == 53, "2/5 modules should map to 53%");
	model.MarkModuleFinished("module-3");
	ok &= expect(model.Percent() == 62, "3/5 modules should map to 62%");
	model.MarkModuleFinished("module-4");
	ok &= expect(model.Percent() == 71, "4/5 modules should map to 71%");
	model.MarkModuleFinished("module-5");
	ok &= expect(model.Percent() == 80, "5/5 modules should map to 80%");

	model.Reset();
	model.SetStage(OBS::StartupProgressStage::AppInitialized);
	model.SetStage(OBS::StartupProgressStage::LibobsInitialized);
	model.SetStage(OBS::StartupProgressStage::ModuleDiscovery);
	model.SetModuleCount(2);
	model.SetStage(OBS::StartupProgressStage::ModuleLoading);
	model.MarkModuleFinished("module-a");
	const int firstCompletionPercent = model.Percent();
	model.MarkModuleFinished("module-a");
	ok &= expect(model.Percent() == firstCompletionPercent, "duplicate completion must not increase progress");
	model.MarkModuleFinished("module-b");
	model.MarkModuleFinished("module-c");
	ok &= expect(model.Percent() == 80, "processed module count must be clamped to total modules");

	model.Reset();
	model.SetStage(OBS::StartupProgressStage::ModuleLoading);
	model.MarkModuleFinished("module-x");
	ok &= expect(model.Percent() == 35, "unknown module count should stay at module loading base percent");

	model.SetStage(OBS::StartupProgressStage::Finished);
	model.SetStage(OBS::StartupProgressStage::AppInitialized);
	ok &= expect(model.Percent() == 100, "stage regression after finish must not decrease progress");

	if (!ok)
		return 1;

	printf("startup-progress-mapping-test: success\n");
	return 0;
}
