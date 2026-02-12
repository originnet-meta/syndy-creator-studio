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

#include <QWidget>

class QLabel;
class QProgressBar;

class StartupSplashWidget : public QWidget {
public:
	explicit StartupSplashWidget(QWidget *parent = nullptr);

	void SetStatusText(const QString &statusText);
	void SetModuleName(const QString &moduleName);
	void SetStepText(const QString &stepText);
	void SetProgressPercent(int percent);

	void UpdateState(const QString &statusText, const QString &moduleName, int percent, const QString &stepText);

	QString StatusText() const;
	QString ModuleName() const;
	QString StepText() const;
	int ProgressPercent() const;

private:
	void RefreshModuleLabel();
	void RefreshStepLabel();

	QLabel *statusLabel = nullptr;
	QLabel *moduleLabel = nullptr;
	QLabel *stepLabel = nullptr;
	QProgressBar *progressBar = nullptr;

	QString statusText;
	QString moduleName;
	QString stepText;
};
