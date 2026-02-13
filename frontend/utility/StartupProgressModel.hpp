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

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace OBS {

enum class StartupProgressStage {
	Boot,
	AppInitialized,
	LibobsInitialized,
	ModuleDiscovery,
	ModuleLoading,
	ModulesLoaded,
	ServiceInitialized,
	SceneCollectionLoaded,
	UiReady,
	Finished,
};

class StartupProgressModel {
public:
	StartupProgressModel();

	void Reset();

	void SetStage(StartupProgressStage stage);
	void SetModuleCount(size_t totalModules);
	void MarkModuleStarted(std::string_view moduleName);
	void MarkModuleFinished(std::string_view moduleName);

	int Percent() const;
	double PercentPrecise() const;

	StartupProgressStage Stage() const { return stage; }
	const std::string &CurrentModuleName() const { return currentModuleName; }
	size_t TotalModules() const { return totalModules; }
	size_t ProcessedModules() const { return processedModules; }

private:
	double CalculateRawPercent() const;
	void UpdateProgress();

	StartupProgressStage stage = StartupProgressStage::Boot;
	bool moduleCountKnown = false;
	size_t totalModules = 0;
	size_t processedModules = 0;
	double progressPercent = 0.0;

	std::string currentModuleName;
	std::unordered_set<std::string> finishedModules;
};

} // namespace OBS
