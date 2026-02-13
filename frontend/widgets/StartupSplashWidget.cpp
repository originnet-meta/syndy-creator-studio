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

#include "StartupSplashWidget.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QVBoxLayout>

namespace {

constexpr int kSplashWidth = 420;
constexpr int kSplashHeight = 320;

} // namespace

StartupSplashWidget::StartupSplashWidget(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	setAttribute(Qt::WA_TranslucentBackground, false);
	setObjectName("startupSplashWidget");
	setFixedSize(kSplashWidth, kSplashHeight);

	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	auto *imageLabel = new QLabel(this);
	imageLabel->setAlignment(Qt::AlignCenter);
	imageLabel->setMinimumHeight(200);
	imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	QPixmap splashImage(":/res/images/obs.png");
	if (!splashImage.isNull()) {
		imageLabel->setPixmap(splashImage.scaled(220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	} else {
		imageLabel->setText("OBS");
	}
	rootLayout->addWidget(imageLabel);

	auto *bottomWidget = new QWidget(this);
	bottomWidget->setObjectName("startupSplashBottom");
	auto *bottomLayout = new QVBoxLayout(bottomWidget);
	bottomLayout->setContentsMargins(20, 14, 20, 18);
	bottomLayout->setSpacing(6);

	statusLabel = new QLabel(bottomWidget);
	statusLabel->setObjectName("startupSplashStatus");
	statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	bottomLayout->addWidget(statusLabel);

	moduleLabel = new QLabel(bottomWidget);
	moduleLabel->setObjectName("startupSplashModule");
	moduleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	moduleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
	moduleLabel->setWordWrap(false);
	bottomLayout->addWidget(moduleLabel);

	auto *progressRow = new QHBoxLayout();
	progressRow->setSpacing(10);

	progressBar = new QProgressBar(bottomWidget);
	progressBar->setObjectName("startupSplashProgress");
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setTextVisible(true);
	progressBar->setFormat("%p%");
	progressRow->addWidget(progressBar, 1);

	stepLabel = new QLabel(bottomWidget);
	stepLabel->setObjectName("startupSplashStep");
	stepLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	progressRow->addWidget(stepLabel);

	bottomLayout->addLayout(progressRow);
	rootLayout->addWidget(bottomWidget);

	setStyleSheet(
		"#startupSplashWidget {"
		"  background-color: #12131a;"
		"  border: 1px solid #2c313a;"
		"}"
		"#startupSplashBottom {"
		"  background-color: #1b1f28;"
		"}"
		"#startupSplashStatus {"
		"  color: #f3f5f8;"
		"  font-size: 13px;"
		"  font-weight: 600;"
		"}"
		"#startupSplashModule {"
		"  color: #c3c9d4;"
		"  font-size: 12px;"
		"}"
		"#startupSplashStep {"
		"  color: #9ea5b2;"
		"  font-size: 11px;"
		"}"
		"#startupSplashProgress {"
		"  min-height: 12px;"
		"  border: 1px solid #2c313a;"
		"  border-radius: 6px;"
		"  text-align: center;"
		"  color: #f3f5f8;"
		"}"
		"#startupSplashProgress::chunk {"
		"  border-radius: 5px;"
		"  background-color: #f05a28;"
		"}");

	SetStatusText("Starting up");
	SetModuleName({});
	SetStepText("Startup");
	SetProgressPercent(0);
}

void StartupSplashWidget::SetStatusText(const QString &text)
{
	statusText = text;
	statusLabel->setText(statusText.isEmpty() ? "Starting up" : statusText);
}

void StartupSplashWidget::SetModuleName(const QString &name)
{
	moduleName = name;
	RefreshModuleLabel();
}

void StartupSplashWidget::SetStepText(const QString &text)
{
	stepText = text;
	RefreshStepLabel();
}

void StartupSplashWidget::SetProgressPercent(int percent)
{
	const int clampedPercent = qBound(0, percent, 100);
	progressBar->setValue(clampedPercent);
}

void StartupSplashWidget::UpdateState(const QString &text, const QString &name, int percent, const QString &step)
{
	SetStatusText(text);
	SetModuleName(name);
	SetProgressPercent(percent);
	SetStepText(step);
}

QString StartupSplashWidget::StatusText() const
{
	return statusLabel->text();
}

QString StartupSplashWidget::ModuleName() const
{
	return moduleName;
}

QString StartupSplashWidget::StepText() const
{
	return stepLabel->text();
}

int StartupSplashWidget::ProgressPercent() const
{
	return progressBar->value();
}

void StartupSplashWidget::RefreshModuleLabel()
{
	if (moduleName.isEmpty()) {
		moduleLabel->setText("Module: -");
	} else {
		moduleLabel->setText(QString("Module: %1").arg(moduleName));
	}
}

void StartupSplashWidget::RefreshStepLabel()
{
	if (stepText.isEmpty()) {
		stepLabel->setText("Stage");
	} else {
		stepLabel->setText(stepText);
	}
}
