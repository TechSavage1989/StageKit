/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "../common/box-alignment.hpp"
#include "../common/tooltip.hpp"
#include <obs-module.h>
#include <QButtonGroup>
#include <QGridLayout>
#include <QToolButton>

namespace stagekit {
class AlignmentGrid : public QWidget {
public:
	AlignmentGrid(QWidget *parent, const QString &name, const char *tooltip)
		: QWidget(parent),
		  group(new QButtonGroup(this))
	{
		setObjectName(name);
		auto *grid = new QGridLayout(this);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(2);
		static const char *const places[] = {"Dock.FitAlign.TopLeft",    "Dock.FitAlign.Top",
						     "Dock.FitAlign.TopRight",   "Dock.FitAlign.Left",
						     "Dock.FitAlign.Middle",     "Dock.FitAlign.Right",
						     "Dock.FitAlign.BottomLeft", "Dock.FitAlign.Bottom",
						     "Dock.FitAlign.BottomRight"};
		for (int cell = 0; cell < 9; ++cell) {
			const QString label = QString::fromUtf8(obs_module_text(places[cell]));
			auto *square = new QToolButton(this);
			square->setObjectName(name + QString::number(cell));
			square->setCheckable(true);
			square->setFixedSize(12, 12);
			square->setAccessibleName(label);
			square->setToolTip(QString::fromStdString(
				wrap_tip(QString::fromUtf8(obs_module_text(tooltip)).arg(label).toStdString())));
			group->addButton(square, fit::kTowardGrid[cell]);
			grid->addWidget(square, cell / 3, cell % 3);
		}
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	}
	QButtonGroup *const group;
};
} // namespace stagekit
