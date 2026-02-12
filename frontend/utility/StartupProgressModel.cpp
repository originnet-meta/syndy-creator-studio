/******************************************************************************
    Copyright (C) 2026 by OBS Project

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

#include "StartupProgressModel.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kAppInitializedPercent = 15.0;
constexpr double kLibobsInitializedPercent = 30.0;
constexpr double kModuleDiscoveryPercent = 35.0;
constexpr double kModuleLoadingStartPercent = 35.0;
constexpr double kModuleLoadingWeight = 45.0;
constexpr double kModulesLoadedPercent = 80.0;
constexpr double kServiceInitializedPercent = 90.0;
constexpr double kSceneCollectionLoadedPercent = 97.0;
constexpr double kUiReadyPercent = 99.0;
constexpr double kFinishedPercent = 100.0;

constexpr double ClampPercent(double value)
{
	return value < 0.0 ? 0.0 : (value > 100.0 ? 100.0 : value);
}

constexpr bool StageAfter(OBS::StartupProgressStage lhs, OBS::StartupProgressStage rhs)
{
	return static_cast<int>(lhs) > static_cast<int>(rhs);
}

} // namespace

namespace OBS {

StartupProgressModel::StartupProgressModel()
{
	Reset();
}

void StartupProgressModel::Reset()
{
	stage = StartupProgressStage::Boot;
	moduleCountKnown = false;
	totalModules = 0;
	processedModules = 0;
	progressPercent = 0.0;
	currentModuleName.clear();
	finishedModules.clear();
}

void StartupProgressModel::SetStage(StartupProgressStage newStage)
{
	if (!StageAfter(newStage, stage))
		return;

	stage = newStage;

	if (StageAfter(stage, StartupProgressStage::ModuleLoading))
		currentModuleName.clear();

	UpdateProgress();
}

void StartupProgressModel::SetModuleCount(size_t count)
{
	moduleCountKnown = true;
	totalModules = count;
	processedModules = 0;
	currentModuleName.clear();
	finishedModules.clear();

	UpdateProgress();
}

void StartupProgressModel::MarkModuleStarted(std::string_view moduleName)
{
	if (StageAfter(StartupProgressStage::ModuleLoading, stage))
		stage = StartupProgressStage::ModuleLoading;

	currentModuleName.assign(moduleName.begin(), moduleName.end());
	UpdateProgress();
}

void StartupProgressModel::MarkModuleFinished(std::string_view moduleName)
{
	if (StageAfter(StartupProgressStage::ModuleLoading, stage))
		stage = StartupProgressStage::ModuleLoading;

	const std::string moduleKey(moduleName);
	if (finishedModules.emplace(moduleKey).second)
		processedModules++;

	if (moduleCountKnown && processedModules > totalModules)
		processedModules = totalModules;

	if (moduleCountKnown && processedModules >= totalModules)
		currentModuleName.clear();
	else
		currentModuleName = moduleKey;

	UpdateProgress();
}

double StartupProgressModel::CalculateRawPercent() const
{
	double moduleFraction = 0.0;

	if (moduleCountKnown) {
		if (totalModules == 0) {
			moduleFraction = 1.0;
		} else {
			size_t clampedProcessed = std::min(processedModules, totalModules);
			moduleFraction = static_cast<double>(clampedProcessed) / static_cast<double>(totalModules);
		}
	}

	switch (stage) {
	case StartupProgressStage::Boot:
		return 0.0;
	case StartupProgressStage::AppInitialized:
		return kAppInitializedPercent;
	case StartupProgressStage::LibobsInitialized:
		return kLibobsInitializedPercent;
	case StartupProgressStage::ModuleDiscovery:
		return kModuleDiscoveryPercent;
	case StartupProgressStage::ModuleLoading:
		return kModuleLoadingStartPercent + (moduleFraction * kModuleLoadingWeight);
	case StartupProgressStage::ModulesLoaded:
		return kModulesLoadedPercent;
	case StartupProgressStage::ServiceInitialized:
		return kServiceInitializedPercent;
	case StartupProgressStage::SceneCollectionLoaded:
		return kSceneCollectionLoadedPercent;
	case StartupProgressStage::UiReady:
		return kUiReadyPercent;
	case StartupProgressStage::Finished:
		return kFinishedPercent;
	}

	return kFinishedPercent;
}

void StartupProgressModel::UpdateProgress()
{
	progressPercent = std::max(progressPercent, ClampPercent(CalculateRawPercent()));
}

int StartupProgressModel::Percent() const
{
	return static_cast<int>(std::floor(progressPercent));
}

double StartupProgressModel::PercentPrecise() const
{
	return progressPercent;
}

} // namespace OBS
