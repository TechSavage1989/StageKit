/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "countdown.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cstring>

namespace stagekit {
namespace {

constexpr char kDockId[] = "stagekit-countdown-dock";

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

class TimerDock : public QWidget {
public:
	TimerDock()
	{
		setObjectName("stagekitCountdownControls");
		auto *layout = new QVBoxLayout(this);
		auto *form = new QFormLayout;
		targets_ = new QComboBox(this);
		targets_->setObjectName("countdownTarget");
		form->addRow(text("Countdown.Timer"), targets_);
		auto *settings = new QPushButton(text("Countdown.Settings"), this);
		settings->setObjectName("countdownSettings");
		form->addRow(settings);
		connect(settings, &QPushButton::clicked, this, [this] {
			auto source = target();
			if (source) {
				edit_timer(this, source);
				if (targets_->currentIndex() == 0) {
					private_settings_ = obs_source_get_settings(source);
					obs_frontend_save();
				}
				refresh_time();
			}
		});
		layout->addLayout(form);

		readout_ = new QLabel(this);
		readout_->setObjectName("countdownReadout");
		readout_->setTextFormat(Qt::PlainText);
		readout_->setWordWrap(true);
		readout_->setAlignment(Qt::AlignCenter);
		auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		font.setPointSize(24);
		font.setBold(true);
		readout_->setFont(font);
		layout->addWidget(readout_);
		status_ = new QLabel(this);
		status_->setObjectName("countdownStatus");
		status_->setAlignment(Qt::AlignCenter);
		layout->addWidget(status_);
		auto *buttons = new QHBoxLayout;
		auto button = [&](const char *name, const char *key, void (*command)(obs_source_t *)) {
			auto *widget = new QPushButton(text(key), this);
			widget->setObjectName(name);
			buttons->addWidget(widget);
			connect(widget, &QPushButton::clicked, this, [this, command] {
				auto source = target();
				if (source)
					command(source);
			});
			return widget;
		};
		start_ = button("countdownStart", "Countdown.Start", [](obs_source_t *s) { obs_source_media_play_pause(s, false); });
		pause_ = button("countdownPause", "Countdown.Pause", [](obs_source_t *s) { obs_source_media_play_pause(s, true); });
		button("countdownReset", "Countdown.Reset", obs_source_media_stop);
		layout->addLayout(buttons);
		layout->addStretch();
		connect(targets_, &QComboBox::currentIndexChanged, this, [this] {
			selected_ = targets_->currentData().toString();
			refresh_time();
			obs_frontend_save();
		});
		connect(&refresh_, &QTimer::timeout, this, [this] { refresh_time(); });
		refresh_.setInterval(100);
		for (const char *signal : {"source_create", "source_remove", "source_rename"})
			signal_handler_connect(obs_get_signal_handler(), signal, source_changed, this);
	}

	~TimerDock() override { shutdown(); }

	void shutdown()
	{
		if (closed_)
			return;
		closed_ = true;
		refresh_.stop();
		for (const char *signal : {"source_create", "source_remove", "source_rename"})
			signal_handler_disconnect(obs_get_signal_handler(), signal, source_changed, this);
		private_ = nullptr;
	}

	void clear_collection()
	{
		loaded_ = false;
		refresh_.stop();
		private_ = nullptr;
	}

	void save_load(obs_data_t *root, bool saving)
	{
		if (saving) {
			if (private_)
				private_settings_ = obs_source_get_settings(private_);
			if (private_settings_)
				obs_data_set_obj(root, "stagekit.countdown.private_settings", private_settings_);
			obs_data_set_string(root, "stagekit.countdown.selected", selected_.toUtf8().constData());
		} else if (!closed_) {
			clear_collection();
			private_settings_ = obs_data_get_obj(root, "stagekit.countdown.private_settings");
			selected_ = QString::fromUtf8(obs_data_get_string(root, "stagekit.countdown.selected"));
			open_collection();
		}
	}

	void open_collection()
	{
		if (closed_)
			return;
		if (!loaded_) {
			private_ = obs_source_create_private(kCountdownId, nullptr, private_settings_);
			loaded_ = true;
		}
		refresh_sources();
		if (isVisible())
			refresh_.start();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (loaded_ && !closed_) {
			refresh_sources();
			refresh_.start();
		}
	}

	void hideEvent(QHideEvent *event) override
	{
		refresh_.stop();
		QWidget::hideEvent(event);
	}

private:
	OBSSourceAutoRelease target() const
	{
		if (!loaded_ || closed_)
			return {};
		const auto uuid = targets_->currentData().toString().toUtf8();
		return uuid.isEmpty() ? obs_source_get_ref(private_) : obs_get_source_by_uuid(uuid.constData());
	}

	static void source_changed(void *data, calldata_t *cd)
	{
		auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (source && !obs_obj_is_private(source) && std::strcmp(obs_source_get_id(source), kCountdownId) == 0) {
			auto *self = static_cast<TimerDock *>(data);
			QMetaObject::invokeMethod(self, [self] { self->refresh_sources(); }, Qt::QueuedConnection);
		}
	}

	void refresh_sources()
	{
		if (!loaded_ || closed_)
			return;
		using List = QList<QPair<QString, QString>>;
		List sources;
		obs_enum_sources([](void *p, obs_source_t *s) {
			if (!obs_source_removed(s) && !obs_obj_is_private(s) && std::strcmp(obs_source_get_id(s), kCountdownId) == 0)
				static_cast<List *>(p)->append({QString::fromUtf8(obs_source_get_name(s)), QString::fromUtf8(obs_source_get_uuid(s))});
			return true;
		}, &sources);
		std::sort(sources.begin(), sources.end());
		QSignalBlocker block(targets_);
		targets_->clear();
		targets_->addItem(text("Countdown.Private"), QString());
		for (const auto &entry : sources)
			targets_->addItem(entry.first, entry.second);
		targets_->setCurrentIndex(std::max(0, targets_->findData(selected_)));
		refresh_time();
	}

	void refresh_time()
	{
		auto source = target();
		if (!source)
			return;
		const QString time = timer_readout(source);
		if (readout_->text() != time)
			readout_->setText(time);
		const auto state = obs_source_media_get_state(source);
		const char *key = "Countdown.Ready";
		if (state == OBS_MEDIA_STATE_PLAYING)
			key = "Countdown.Running";
		else if (state == OBS_MEDIA_STATE_PAUSED)
			key = "Countdown.Paused";
		else if (state == OBS_MEDIA_STATE_ENDED)
			key = "Countdown.Finished";
		status_->setText(text(key));
		start_->setEnabled(state != OBS_MEDIA_STATE_PLAYING);
		pause_->setEnabled(state == OBS_MEDIA_STATE_PLAYING);
	}

	QComboBox *targets_ = nullptr;
	QLabel *readout_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *start_ = nullptr;
	QPushButton *pause_ = nullptr;
	QTimer refresh_;
	OBSSourceAutoRelease private_;
	QString selected_;
	OBSDataAutoRelease private_settings_;
	bool loaded_ = false;
	bool closed_ = false;
};

QPointer<TimerDock> dock;
bool registered = false;

void collection_data(obs_data_t *root, bool saving, void *)
{
	if (dock)
		dock->save_load(root, saving);
}

void frontend_event(obs_frontend_event e, void *)
{
	if (!dock)
		return;
	if (e == OBS_FRONTEND_EVENT_EXIT || e == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
		dock->shutdown();
	else if (e == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
		dock->clear_collection();
	else if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING || e == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		dock->open_collection();
}

} // namespace

void register_countdown_dock()
{
	if (!obs_frontend_get_main_window())
		return;
	dock = new TimerDock;
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Countdown.Dock"), dock.data())) {
		delete dock.data();
		return;
	}
	obs_frontend_add_save_callback(collection_data, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	registered = true;
}

void unregister_countdown_dock()
{
	if (!registered)
		return;
	registered = false;
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_frontend_remove_save_callback(collection_data, nullptr);
	if (dock) {
		dock->shutdown();
		obs_frontend_remove_dock(kDockId);
	}
}

} // namespace stagekit
