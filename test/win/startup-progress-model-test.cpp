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
		fprintf(stderr, "startup-progress-model-test: %s\n", message);

	return condition;
}

int main()
{
	bool ok = true;
	OBS::StartupProgressModel model;

	ok &= expect(model.Percent() == 0, "initial progress must be 0");

	model.SetStage(OBS::StartupProgressStage::AppInitialized);
	ok &= expect(model.Percent() == 15, "AppInitialized should map to 15%");

	model.SetStage(OBS::StartupProgressStage::LibobsInitialized);
	ok &= expect(model.Percent() == 30, "LibobsInitialized should map to 30%");

	model.SetStage(OBS::StartupProgressStage::ModuleDiscovery);
	ok &= expect(model.Percent() == 35, "ModuleDiscovery should map to 35%");

	model.SetModuleCount(4);
	model.SetStage(OBS::StartupProgressStage::ModuleLoading);
	ok &= expect(model.Percent() == 35, "ModuleLoading should start at 35%");

	model.MarkModuleStarted("module-a");
	ok &= expect(model.CurrentModuleName() == "module-a", "current module name should update on begin");
	ok &= expect(model.Percent() == 35, "begin callback must not advance module fraction");

	model.MarkModuleFinished("module-a");
	ok &= expect(model.Percent() > 35 && model.Percent() < 80, "terminal module callback should advance progress");

	model.MarkModuleFinished("module-b");
	model.MarkModuleFinished("module-c");
	model.MarkModuleFinished("module-d");
	ok &= expect(model.Percent() == 80, "all module callbacks should complete module slice");
	ok &= expect(model.CurrentModuleName().empty(), "current module should clear when module loading completes");

	model.SetStage(OBS::StartupProgressStage::ModulesLoaded);
	ok &= expect(model.Percent() == 80, "ModulesLoaded should remain 80%");

	model.SetStage(OBS::StartupProgressStage::ServiceInitialized);
	ok &= expect(model.Percent() == 90, "ServiceInitialized should map to 90%");

	model.SetStage(OBS::StartupProgressStage::SceneCollectionLoaded);
	ok &= expect(model.Percent() == 97, "SceneCollectionLoaded should map to 97%");

	model.SetStage(OBS::StartupProgressStage::UiReady);
	ok &= expect(model.Percent() == 99, "UiReady should map to 99%");

	model.SetStage(OBS::StartupProgressStage::Finished);
	ok &= expect(model.Percent() == 100, "Finished should map to 100%");

	model.SetStage(OBS::StartupProgressStage::Boot);
	ok &= expect(model.Percent() == 100, "progress must be monotonic when stage regresses");

	model.Reset();
	model.SetStage(OBS::StartupProgressStage::AppInitialized);
	model.SetStage(OBS::StartupProgressStage::LibobsInitialized);
	model.SetStage(OBS::StartupProgressStage::ModuleDiscovery);
	model.SetModuleCount(0);
	model.SetStage(OBS::StartupProgressStage::ModuleLoading);
	ok &= expect(model.Percent() == 80, "zero-module startup should complete module slice immediately");

	model.SetStage(OBS::StartupProgressStage::Finished);
	ok &= expect(model.Percent() == 100, "zero-module startup should still reach 100%");

	if (!ok)
		return 1;

	printf("startup-progress-model-test: success\n");
	return 0;
}
