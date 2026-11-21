/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "countdown.hpp"
#include <QFontMetricsF>
#include <QPainter>
#include <algorithm>
#include <array>

namespace stagekit {

// Digits share a tabular advance and a baseline even in a proportional face.
// Literal runs retain their font's shaping, kerning and fallback glyphs.
class TimerLine {
public:
	explicit TimerLine(const QFont &font) : font_(font), metrics_(font)
	{
		for (int i = 0; i < 10; ++i)
			advance_ = std::max(advance_, metrics_.horizontalAdvance(QChar('0' + i)));
		for (int i = 0; i < 10; ++i) {
			const QChar c('0' + i);
			offsets_[i] = (advance_ - metrics_.horizontalAdvance(c)) / 2;
			digit_ |= ink(QString(c)).translated(offsets_[i], 0);
		}
		colon_ = ink(QStringLiteral(":"));
		colon_advance_ = metrics_.horizontalAdvance(QLatin1Char(':'));
	}

	QRectF ink(const QString &s) const
	{
		QRectF bounds = metrics_.tightBoundingRect(s);
		const qreal advance = metrics_.horizontalAdvance(s);
		if (font_.underline())
			bounds |= QRectF(0, metrics_.underlinePos() - metrics_.lineWidth(), advance,
					 2 * metrics_.lineWidth());
		if (font_.strikeOut())
			bounds |= QRectF(0, -metrics_.strikeOutPos() - metrics_.lineWidth(), advance,
					 2 * metrics_.lineWidth());
		return bounds;
	}

	QRectF layout(const QString &text, QPainter *painter = nullptr, QPointF origin = {}) const
	{
		QRectF bounds;
		qreal x = 0;
		for (qsizetype i = 0; i < text.size();) {
			const auto c = text[i];
			if (is_digit(c) || c == timer_digit_space) {
				if (!painter)
					bounds |= digit_.translated(x, 0);
				else if (c != timer_digit_space)
					painter->drawText(origin + QPointF(x + offsets_[c.digitValue()], 0),
							  QString(c));
				x += advance_;
				++i;
			} else if (c == QLatin1Char(':') || c == timer_colon_space) {
				if (!painter)
					bounds |= colon_.translated(x, 0);
				else if (c != timer_colon_space)
					painter->drawText(origin + QPointF(x, 0), QString(c));
				x += colon_advance_;
				++i;
			} else {
				const auto first = i++;
				while (i < text.size() && !is_digit(text[i]) && text[i] != timer_digit_space &&
				       text[i] != QLatin1Char(':') && text[i] != timer_colon_space)
					++i;
				const auto run = text.mid(first, i - first);
				if (painter)
					painter->drawText(origin + QPointF(x, 0), run);
				else
					bounds |= ink(run).translated(x, 0);
				x += metrics_.horizontalAdvance(run);
			}
		}
		return bounds;
	}

	static bool is_digit(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }

private:
	QFont font_;
	QFontMetricsF metrics_;
	QRectF digit_, colon_;
	qreal advance_ = 0, colon_advance_ = 0;
	std::array<qreal, 10> offsets_{};
};

} // namespace stagekit
