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

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QScreen>
#include <QStackedLayout>
#include <QVBoxLayout>

namespace {

constexpr int kSplashFallbackWidth = 420;
constexpr int kSplashFallbackHeight = 630;
constexpr int kSplashMaxScreenPercent = 75;
constexpr auto kSplashImagePath = ":/res/images/startup-splash-image-rounded_02.png";

} // namespace

StartupSplashWidget::StartupSplashWidget(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	setAttribute(Qt::WA_TranslucentBackground, true);
	setAutoFillBackground(false);
	setObjectName("startupSplashWidget");
	QPixmap splashImage(kSplashImagePath);
	if (splashImage.isNull())
		splashImage.load(":/res/images/obs.png");

	QSize targetSize = splashImage.isNull() ? QSize(kSplashFallbackWidth, kSplashFallbackHeight) : splashImage.size();
	if (const QScreen *screen = QGuiApplication::primaryScreen()) {
		const QSize availableSize = screen->availableGeometry().size();
		QSize maxSize((availableSize.width() * kSplashMaxScreenPercent) / 100,
			      (availableSize.height() * kSplashMaxScreenPercent) / 100);
		if (maxSize.width() > 0 && maxSize.height() > 0 && (targetSize.width() > maxSize.width() ||
								      targetSize.height() > maxSize.height())) {
			targetSize = targetSize.scaled(maxSize, Qt::KeepAspectRatio);
		}
	}
	setFixedSize(targetSize);

	auto *stackLayout = new QStackedLayout(this);
	stackLayout->setContentsMargins(0, 0, 0, 0);
	stackLayout->setStackingMode(QStackedLayout::StackAll);

	auto *imageLabel = new QLabel(this);
	imageLabel->setAlignment(Qt::AlignCenter);
	imageLabel->setScaledContents(true);
	imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	if (!splashImage.isNull()) {
		imageLabel->setPixmap(splashImage.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	stackLayout->addWidget(imageLabel);

	auto *overlayWidget = new QWidget(this);
	overlayWidget->setObjectName("startupSplashOverlay");
	overlayWidget->setAttribute(Qt::WA_TranslucentBackground, true);
	auto *overlayLayout = new QVBoxLayout(overlayWidget);
	overlayLayout->setContentsMargins(28, 24, 28, 24);
	overlayLayout->setSpacing(6);
	overlayLayout->addStretch();

	statusLabel = new QLabel(overlayWidget);
	statusLabel->setObjectName("startupSplashStatus");
	statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	overlayLayout->addWidget(statusLabel);

	moduleLabel = new QLabel(overlayWidget);
	moduleLabel->setObjectName("startupSplashModule");
	moduleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	moduleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
	moduleLabel->setWordWrap(false);
	overlayLayout->addWidget(moduleLabel);

	auto *progressRow = new QHBoxLayout();
	progressRow->setSpacing(0);

	progressBar = new QProgressBar(overlayWidget);
	progressBar->setObjectName("startupSplashProgress");
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setTextVisible(true);
	progressBar->setFormat("%p%");
	progressRow->addWidget(progressBar, 1);

	stepLabel = new QLabel(overlayWidget);
	stepLabel->setObjectName("startupSplashStep");
	stepLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	stepLabel->hide();

	overlayLayout->addLayout(progressRow);
	stackLayout->addWidget(overlayWidget);
	stackLayout->setCurrentWidget(overlayWidget);

	setStyleSheet(
		"#startupSplashWidget {"
		"  background: transparent;"
		"}"
		"#startupSplashOverlay {"
		"  background: transparent;"
		"}"
		"#startupSplashStatus {"
		"  background: transparent;"
		"  color: #f7f8fb;"
		"  font-size: 13px;"
		"  font-weight: 600;"
		"}"
		"#startupSplashModule {"
		"  background: transparent;"
		"  color: #e5e8ef;"
		"  font-size: 12px;"
		"}"
		"#startupSplashStep {"
		"  background: transparent;"
		"  color: #e2e6ee;"
		"  font-size: 11px;"
		"}"
		"#startupSplashProgress {"
		"  min-height: 12px;"
		"  background: transparent;"
		"  border: none;"
		"  border-radius: 6px;"
		"  text-align: center;"
		"  color: #f7f8fb;"
		"}"
		"#startupSplashProgress::chunk {"
		"  border-radius: 5px;"
		"  background-color: rgba(240, 90, 40, 220);"
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

