#pragma once

#include "scene-controller-dock-test.hpp"
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVariantMap>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <util/bmem.h>

// Exercise the public dock and normal OBS objects. This probe deliberately
// does not link the native catalog or call the panel's private implementation.
namespace control_panel_test {

using namespace scene_controller_dock_test;

inline QString uuid(obs_source_t *source)
{
	return QString::fromUtf8(source ? obs_source_get_uuid(source) : "");
}

inline QJsonObject json(obs_data_t *data)
{
	return QJsonDocument::fromJson(data ? obs_data_get_json(data) : "{}").object();
}

inline QVariantMap target(QAbstractButton *button)
{
	return button ? button->property("viewsControlTarget").toMap() : QVariantMap{};
}

inline bool matches(const QVariantMap &value, const QVariantMap &wanted)
{
	for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
		if (value.value(it.key()) != it.value())
			return false;
	return true;
}

inline QAbstractButton *button(QWidget *panel, const QVariantMap &wanted)
{
	for (auto *candidate : panel->findChildren<QAbstractButton *>())
		if (candidate->property("viewsControlId").isValid() && matches(target(candidate), wanted))
			return candidate;
	return nullptr;
}

inline QAbstractButton *button_id(QWidget *panel, const QString &id)
{
	return button(panel, {{"id", id}});
}

inline QString control_id(QWidget *panel, const QVariantMap &wanted)
{
	const auto *found = button(panel, wanted);
	return found ? found->property("viewsControlId").toString() : QString();
}

inline bool checked(QWidget *panel, const QString &id)
{
	const auto *found = button_id(panel, id);
	return found && found->isChecked();
}

// The panel's settings (edit, manage, layout, size, titles) open above its
// bottom bar from the settings button; the fixture drives them while open.
inline void open_panel_settings(QWidget *panel)
{
	auto *toggle = panel ? panel->findChild<QAbstractButton *>("viewsPanelSettingsToggle") : nullptr;
	if (toggle && !toggle->isChecked()) {
		toggle->click();
		QCoreApplication::processEvents();
	}
}

inline void close_panel_settings(QWidget *panel)
{
	auto *toggle = panel ? panel->findChild<QAbstractButton *>("viewsPanelSettingsToggle") : nullptr;
	if (toggle && toggle->isChecked()) {
		toggle->click();
		QCoreApplication::processEvents();
	}
}

inline bool panel_ready(QWidget *panel)
{
	open_panel_settings(panel);
	const auto *edit = panel->findChild<QAbstractButton *>("viewsPanelEdit");
	return edit && edit->isEnabled();
}

inline QTreeWidgetItem *tree_item(QTreeWidget *tree, const QString &id)
{
	if (!tree)
		return nullptr;
	for (QTreeWidgetItemIterator it(tree); *it; ++it)
		if ((*it)->data(0, Qt::UserRole).toString() == id)
			return *it;
	return nullptr;
}

inline QList<QTreeWidgetItem *> tree_controls(QTreeWidget *tree)
{
	QList<QTreeWidgetItem *> result;
	if (tree)
		for (QTreeWidgetItemIterator it(tree); *it; ++it)
			if (!(*it)->data(0, Qt::UserRole).toString().isEmpty())
				result.append(*it);
	return result;
}

inline QTreeWidget *control_list(QWidget *panel)
{
	return panel->findChild<QTreeWidget *>("viewsPanelList");
}

inline bool layout_is(QWidget *panel, const char *layout)
{
	const auto *choice = panel->findChild<QComboBox *>("viewsPanelLayout");
	return choice && choice->currentData().toString() == layout;
}

inline bool select_layout(QWidget *panel, const char *layout)
{
	open_panel_settings(panel);
	auto *choice = panel->findChild<QComboBox *>("viewsPanelLayout");
	const int at = choice ? choice->findData(layout) : -1;
	if (at < 0)
		return false;
	choice->setCurrentIndex(at);
	return until([&] {
		return layout_is(panel, layout) && control_list(panel) &&
		       control_list(panel)->isVisible() == (QString::fromUtf8(layout) == "list");
	});
}

inline bool click_tree(QTreeWidget *tree, QTreeWidgetItem *item)
{
	if (!tree || !tree->isVisible() || !item || !(item->flags() & Qt::ItemIsEnabled))
		return false;
	for (auto *parent = item->parent(); parent; parent = parent->parent())
		parent->setExpanded(true);
	tree->scrollToItem(item);
	QCoreApplication::processEvents();
	const auto rect = tree->visualItemRect(item);
	if (rect.isEmpty() || !tree->viewport()->rect().intersects(rect))
		return false;
	const QPointF at = rect.center();
	QMouseEvent press(QEvent::MouseButtonPress, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
			  Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
			    Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tree->viewport(), &press);
	QApplication::sendEvent(tree->viewport(), &release);
	QCoreApplication::processEvents();
	return true;
}

inline bool key_tree(QTreeWidget *tree, QTreeWidgetItem *item, Qt::Key key)
{
	if (!tree || !tree->isVisible() || !item || !(item->flags() & Qt::ItemIsEnabled))
		return false;
	for (auto *parent = item->parent(); parent; parent = parent->parent())
		parent->setExpanded(true);
	tree->setCurrentItem(item);
	tree->scrollToItem(item);
	QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
	QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
	QApplication::sendEvent(tree, &press);
	QApplication::sendEvent(tree, &release);
	QCoreApplication::processEvents();
	return true;
}

inline bool double_click_tree(QTreeWidget *tree, QTreeWidgetItem *item)
{
	if (!click_tree(tree, item))
		return false;
	const QPointF at = tree->visualItemRect(item).center();
	QMouseEvent twice(QEvent::MouseButtonDblClick, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
			  Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
			    Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tree->viewport(), &twice);
	QApplication::sendEvent(tree->viewport(), &release);
	QCoreApplication::processEvents();
	return true;
}

inline int scene_selector_index(QComboBox *selector, obs_source_t *scene)
{
	for (int i = 0; selector && i < selector->count(); ++i)
		if (selector->itemData(i, Qt::UserRole + 1).toMap().value("sceneUuid").toString() == uuid(scene))
			return i;
	return -1;
}

inline bool select_scene(QComboBox *selector, obs_source_t *scene)
{
	const int index = scene_selector_index(selector, scene);
	if (index < 0 || !selector->isVisible() || !selector->isEnabled())
		return false;
	selector->showPopup();
	QCoreApplication::processEvents();
	auto *view = selector->view();
	const auto model_index = selector->model()->index(index, selector->modelColumn());
	view->setCurrentIndex(model_index);
	view->scrollTo(model_index);
	QCoreApplication::processEvents();
	// A freshly opened Qt combo popup blocks mouse releases briefly. Real
	// keyboard activation is deterministic and exercises the activated signal.
	QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(view, &press);
	QApplication::sendEvent(view, &release);
	QCoreApplication::processEvents();
	return selector->currentIndex() == index && !view->isVisible();
}

inline bool check_inventory(QTreeWidget *tree, const QString &id, Qt::CheckState state)
{
	auto *item = tree_item(tree, id);
	return item &&
	       (item->checkState(0) == state || (key_tree(tree, item, Qt::Key_Space) && item->checkState(0) == state));
}

inline bool scene_is(obs_source_t *scene, bool preview = false)
{
	OBSSourceAutoRelease selected = preview ? obs_frontend_get_current_preview_scene()
						: obs_frontend_get_current_scene();
	return selected.Get() == scene;
}

inline bool set_show_scene(QWidget *panel, bool enabled)
{
	if (!click(panel, "viewsPanelEdit") || !until([&] { return dialog("viewsPanelEditor"); }))
		return false;
	QPointer<QDialog> editor = dialog("viewsPanelEditor");
	auto *option = editor->findChild<QCheckBox *>("viewsPanelSwitchScene");
	if (!option) {
		editor->reject();
		return false;
	}
	option->setChecked(enabled);
	return click(editor, "viewsPanelSave") && until([&] { return !editor || !editor->isVisible(); });
}

inline bool buttons_fit_sections(QWidget *panel, QString *detail = nullptr)
{
	bool found = false;
	for (auto *candidate : panel->findChildren<QAbstractButton *>()) {
		if (!candidate->property("viewsControlId").isValid())
			continue;
		found = true;
		const auto *section = candidate->parentWidget();
		const int wrapped_height = candidate->heightForWidth(candidate->width());
		if (!section || !section->rect().contains(candidate->geometry()) ||
		    candidate->maximumHeight() < wrapped_height || candidate->height() < wrapped_height) {
			if (detail) {
				auto rect = [](const QRect &value) {
					return QString("(%1,%2 %3x%4)")
						.arg(value.x())
						.arg(value.y())
						.arg(value.width())
						.arg(value.height());
				};
				auto size = [](const QSize &value) {
					return QString("%1x%2").arg(value.width()).arg(value.height());
				};
				const auto policy = candidate->sizePolicy();
				*detail =
					QString("%1: button=%2 section=%3 hfw=%4 hint=%5 minHint=%6 minSize=%7 maxSize=%8 "
						"policy=%9/%10 widgetRect=%11")
						.arg(candidate->text(), rect(candidate->geometry()),
						     section ? rect(section->rect()) : "missing")
						.arg(wrapped_height)
						.arg(size(candidate->sizeHint()), size(candidate->minimumSizeHint()),
						     size(candidate->minimumSize()), size(candidate->maximumSize()))
						.arg(int(policy.horizontalPolicy()))
						.arg(int(policy.verticalPolicy()))
						.arg(candidate->testAttribute(Qt::WA_LayoutUsesWidgetRect));
				auto layout_detail = [&](const QLayout *layout) {
					if (!layout)
						return QStringLiteral("missing");
					const auto margins = layout->contentsMargins();
					return QString("rect=%1 hint=%2 min=%3 max=%4 hfw=%5 spacing=%6 margins=%7,%8,%9,%10")
						.arg(rect(layout->geometry()), size(layout->sizeHint()),
						     size(layout->minimumSize()), size(layout->maximumSize()))
						.arg(layout->heightForWidth(layout->geometry().width()))
						.arg(layout->spacing())
						.arg(margins.left())
						.arg(margins.top())
						.arg(margins.right())
						.arg(margins.bottom());
				};
				if (section) {
					*detail += "; layout={" + layout_detail(section->layout()) + "}; grid={" +
						   layout_detail(section->findChild<QGridLayout *>()) + "}";
					if (const auto *label =
						    section->findChild<QLabel *>("viewsPanelSectionHeading"))
						*detail +=
							QString("; heading={rect=%1 hint=%2 minHint=%3 min=%4 max=%5 hfw=%6}")
								.arg(rect(label->geometry()), size(label->sizeHint()),
								     size(label->minimumSizeHint()),
								     size(label->minimumSize()),
								     size(label->maximumSize()))
								.arg(label->heightForWidth(label->width()));
				}
			}
			return false;
		}
	}
	if (!found && detail)
		*detail = "No control buttons were present";
	return found;
}

inline void check_geometry(QWidget *panel, QStringList &issues, const char *message)
{
	if (until([&] { return buttons_fit_sections(panel); }))
		return;
	QString detail;
	buttons_fit_sections(panel, &detail);
	issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message) + "; " + detail);
}

inline bool press_control(QWidget *panel, const QString &id)
{
	if (layout_is(panel, "list"))
		return click_tree(control_list(panel), tree_item(control_list(panel), id));
	auto *found = button_id(panel, id);
	if (!found || !found->isEnabled() || !found->isVisible())
		return false;
	found->click();
	QCoreApplication::processEvents();
	return true;
}

inline int pin_row(QListWidget *pins, const QString &id)
{
	for (int i = 0; pins && i < pins->count(); ++i)
		if (pins->item(i)->data(Qt::UserRole).toString() == id)
			return i;
	return -1;
}

inline bool select_pins(QListWidget *pins, const QList<int> &rows)
{
	if (!pins || rows.isEmpty())
		return false;
	bool first = true;
	for (const int row : rows) {
		auto *item = pins->item(row);
		if (!item)
			return false;
		pins->scrollToItem(item);
		QCoreApplication::processEvents();
		const QPointF at = pins->visualItemRect(item).center();
		const auto modifiers = first ? Qt::NoModifier : Qt::ControlModifier;
		QMouseEvent press(QEvent::MouseButtonPress, at, pins->viewport()->mapToGlobal(at), Qt::LeftButton,
				  Qt::LeftButton, modifiers);
		QMouseEvent release(QEvent::MouseButtonRelease, at, pins->viewport()->mapToGlobal(at), Qt::LeftButton,
				    Qt::NoButton, modifiers);
		QApplication::sendEvent(pins->viewport(), &press);
		QApplication::sendEvent(pins->viewport(), &release);
		first = false;
	}
	QCoreApplication::processEvents();
	if (pins->selectedItems().size() != rows.size())
		return false;
	for (const int row : rows)
		if (!pins->item(row)->isSelected())
			return false;
	return true;
}

inline bool slider_key(QSlider *slider, Qt::Key key)
{
	if (slider)
		open_panel_settings(slider->window());
	if (!slider || !slider->isVisible() || !slider->isEnabled())
		return false;
	QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
	QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
	QApplication::sendEvent(slider, &press);
	QApplication::sendEvent(slider, &release);
	QCoreApplication::processEvents();
	return true;
}

inline bool compact_rows(QWidget *panel)
{
	auto *tree = control_list(panel);
	if (!tree || !tree->isVisible() || tree->horizontalScrollBar()->isVisible())
		return false;
	const auto rows = tree_controls(tree);
	if (rows.isEmpty())
		return false;
	for (auto *row : rows) {
		const int height = tree->visualItemRect(row).height();
		if (height <= 0 || height >= 40)
			return false;
	}
	return true;
}

inline QJsonObject resolved(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataAutoRelease defaults = obs_data_get_defaults(settings);
	obs_data_apply(defaults, settings);
	return json(defaults);
}

inline QString collection_name()
{
	char *raw = obs_frontend_get_current_scene_collection();
	const QString name = QString::fromUtf8(raw ? raw : "");
	bfree(raw);
	return name;
}

inline QStringList saved_labels()
{
	return {"Live game",        "Studio close-up", "Side view",
		"Camera group",     "Watermark",       "Studio title renamed",
		"Group visibility", "Spare (saved)",   "Studio scene"};
}

inline QString saved_section(int index)
{
	return index < 3 ? "Quick choices" : index < 8 ? "Visibility" : "Scenes";
}

inline bool choose_icon(QWidget *editor, const QString &path)
{
	if (!click(editor, "viewsPanelChooseIcon") || !until([&] { return dialog("viewsPanelIconDialog"); }))
		return false;
	QPointer<QFileDialog> chooser = qobject_cast<QFileDialog *>(dialog("viewsPanelIconDialog"));
	if (!chooser)
		return false;
	auto fail = [&](const char *message) {
		const auto *status = editor->findChild<QLabel *>("viewsPanelEditorStatus");
		blog(LOG_WARNING,
		     "[StageKit controls test] icon chooser: %s; file=%s; selected=%s; result=%d; visible=%d; status=%s",
		     message, path.toUtf8().constData(),
		     chooser ? chooser->selectedFiles().join(" | ").toUtf8().constData() : "closed",
		     chooser ? chooser->result() : -1, chooser && chooser->isVisible(),
		     status ? status->text().toUtf8().constData() : "missing");
		if (chooser && chooser->isVisible())
			chooser->reject();
		return false;
	};
	// selectFile() does not populate the visible nonnative dialog reliably.
	// Enter the filename through its real input, then verify the resolved file
	// before invoking the same acceptance path as the Open button.
	auto *filename = chooser->findChild<QLineEdit *>("fileNameEdit");
	if (!filename)
		return fail("filename input missing");
	filename->setText(QDir::toNativeSeparators(path));
	if (!until([&] {
		    const auto files = chooser ? chooser->selectedFiles() : QStringList();
		    return files.size() == 1 && !QFileInfo(path).canonicalFilePath().isEmpty() &&
			   QFileInfo(files.front()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
	    }))
		return fail("filename did not resolve to the selected input file");
	const bool accepted = QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
	if (!accepted || !until([&] { return !chooser || !chooser->isVisible(); }))
		return fail("selected file was not accepted");
	return true;
}

inline bool sample_icon(const QString &path, bool camera)
{
	QImage image(96, 96, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(Qt::white, 5));
	painter.setBrush(QColor(camera ? "#4b75d1" : "#7255a5"));
	if (camera) {
		painter.drawRoundedRect(QRectF(12, 28, 62, 44), 7, 7);
		painter.drawEllipse(QPointF(43, 50), 13, 13);
		QPolygonF lens{{75, 40}, {88, 32}, {88, 68}, {75, 60}};
		painter.drawPolygon(lens);
	} else {
		painter.drawRoundedRect(QRectF(10, 14, 76, 52), 5, 5);
		painter.drawLine(QPointF(48, 67), QPointF(48, 82));
		painter.drawLine(QPointF(28, 83), QPointF(68, 83));
	}
	painter.end();
	return image.save(path);
}

inline bool embedded_icon(const QVariantMap &pin)
{
	QImage image;
	return image.loadFromData(QByteArray::fromBase64(pin.value("iconPng").toString().toLatin1()), "PNG") &&
	       image.width() > 0 && image.height() > 0 && image.width() <= 128 && image.height() <= 128;
}

inline QImage library_image(const QString &id, const QString &palette)
{
	QFile catalog(":/stagekit/view-icons/catalog.json");
	if (!catalog.open(QIODevice::ReadOnly))
		return {};
	const auto icons = QJsonDocument::fromJson(catalog.readAll()).object()["icons"].toArray();
	for (const QJsonValue value : icons) {
		const auto entry = value.toObject();
		if (entry["id"].toString() != id)
			continue;
		const QImage original(entry["paths"].toObject()[palette].toString());
		return original.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)
			.convertToFormat(QImage::Format_ARGB32);
	}
	return {};
}

inline bool saved_library_icon(const QVariantMap &pin)
{
	QImage image;
	const QImage expected = library_image("source:camera", "ocean");
	return !expected.isNull() &&
	       image.loadFromData(QByteArray::fromBase64(pin.value("iconPng").toString().toLatin1()), "PNG") &&
	       image.convertToFormat(QImage::Format_ARGB32) == expected;
}

inline QListWidgetItem *library_item(QListWidget *items, const QString &id)
{
	for (int i = 0; items && i < items->count(); ++i)
		if (items->item(i)->data(Qt::UserRole).toString() == id)
			return items->item(i);
	return nullptr;
}

inline bool select_library_icon(QListWidget *items, const QString &id)
{
	auto *item = library_item(items, id);
	if (!item || item->isHidden() || !items->isVisible())
		return false;
	items->scrollToItem(item);
	QCoreApplication::processEvents();
	if (!items->viewport()->rect().intersects(items->visualItemRect(item)) || !click_row(items, id))
		return false;
	QCoreApplication::processEvents();
	return items->currentItem() == item && item->isSelected();
}

inline bool library_captions_fit(QListWidget *items)
{
	if (!items || !items->isVisible())
		return false;
	class LabelMetrics final : public QStyledItemDelegate {
	public:
		using QStyledItemDelegate::initStyleOption;
	} metrics;
	items->doItemsLayout();
	for (int i = 0; i < items->count(); ++i) {
		auto *item = items->item(i);
		if (item->isHidden())
			continue;
		QStyleOptionViewItem option;
		option.initFrom(items);
		option.widget = items;
		option.rect = items->visualItemRect(item);
		option.font = items->font();
		option.fontMetrics = QFontMetrics(option.font);
		option.decorationSize = items->iconSize();
		option.decorationPosition = QStyleOptionViewItem::Top;
		option.decorationAlignment = Qt::AlignCenter;
		option.features |= QStyleOptionViewItem::WrapText;
		option.textElideMode = items->textElideMode();
		metrics.initStyleOption(&option, items->model()->index(i, 0));
		const QRect text = items->style()->subElementRect(QStyle::SE_ItemViewItemText, &option, items);
		const QRect image = items->style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, items);
		const QRect needed =
			option.fontMetrics.boundingRect(QRect(0, 0, text.width(), 4096), Qt::TextWordWrap, option.text);
		if (text.isEmpty() || !option.rect.contains(text) || !option.rect.contains(image) ||
		    needed.width() > text.width() || needed.height() > text.height()) {
			blog(LOG_WARNING,
			     "[StageKit controls test] icon caption clipped: %s; cell=%dx%d; text=%dx%d; needed=%dx%d",
			     item->data(Qt::UserRole).toString().toUtf8().constData(), option.rect.width(),
			     option.rect.height(), text.width(), text.height(), needed.width(), needed.height());
			return false;
		}
	}
	return items->count() > 0;
}

inline bool select_combo_option(QComboBox *choice, const QString &value)
{
	const int index = choice ? choice->findData(value) : -1;
	if (index < 0 || !choice->isVisible() || !choice->isEnabled())
		return false;
	choice->showPopup();
	QCoreApplication::processEvents();
	auto *view = choice->view();
	const auto model_index = choice->model()->index(index, choice->modelColumn());
	view->setCurrentIndex(model_index);
	view->scrollTo(model_index);
	QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(view, &press);
	QApplication::sendEvent(view, &release);
	QCoreApplication::processEvents();
	return choice->currentIndex() == index && !view->isVisible();
}

inline bool add_section(QWidget *editor, const QString &name)
{
	if (!click(editor, "viewsPanelAddSection") || !until([&] { return dialog("viewsPanelNewSectionDialog"); }))
		return false;
	QPointer<QDialog> naming = dialog("viewsPanelNewSectionDialog");
	auto *input = naming->findChild<QLineEdit *>("viewsPanelNewSectionName");
	auto *save = naming->findChild<QAbstractButton *>("viewsPanelNewSectionSave");
	if (!input || !save) {
		naming->reject();
		return false;
	}
	input->setText("   ");
	const bool rejects_empty = !save->isEnabled();
	input->setText(name);
	const bool saved = rejects_empty && click(naming, "viewsPanelNewSectionSave") &&
			   until([&] { return !naming || !naming->isVisible(); });
	if (naming && naming->isVisible())
		naming->reject();
	return saved;
}

inline void check_icon_library(QWidget *editor, QListWidget *pins, QStringList &issues)
{
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	if (!pins || pins->count() < 2 || !select_pins(pins, {1})) {
		expect(false, "could not select a separate control for the built-in icon library");
		return;
	}
	const auto first = pins->item(0)->data(Qt::UserRole + 1).toMap();
	const auto previous = pins->item(1)->data(Qt::UserRole + 1).toMap();
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	expect(click(editor, "viewsPanelIconLibrary") && until([&] { return dialog("viewsPanelIconLibraryDialog"); }),
	       "the integrated icon library did not open from the panel editor");
	QPointer<QDialog> library = dialog("viewsPanelIconLibraryDialog");
	if (!library)
		return;
	auto *search = library->findChild<QLineEdit *>("viewsPanelIconLibrarySearch");
	auto *category = library->findChild<QComboBox *>("viewsPanelIconLibraryCategory");
	auto *palette = library->findChild<QComboBox *>("viewsPanelIconLibraryPalette");
	auto *items = library->findChild<QListWidget *>("viewsPanelIconLibraryItems");
	auto *use = library->findChild<QAbstractButton *>("viewsPanelIconLibraryUse");
	auto *camera = library_item(items, "source:camera");
	if (!search || !category || !palette || !items || !use || !camera) {
		expect(false, "the icon library was missing its search, categories, palettes or camera artwork");
		library->reject();
		return;
	}
	expect(palette->count() == 6 && palette->currentData().toString() == "mono" && !use->isEnabled() &&
		       items->viewMode() == QListView::IconMode && !camera->icon().isNull(),
	       "the library did not show its built-in thumbnails and six palettes with an explicit selection");
	expect(library_captions_fit(items), "an icon library caption did not fit inside its native thumbnail cell");
	expect(select_library_icon(items, "source:camera") && use->isEnabled(),
	       "selecting a visible library thumbnail did not enable Use icon");
	const QImage mono = camera->icon().pixmap(64, 64).toImage();
	expect(select_combo_option(palette, "ocean") && items->currentItem() == camera && use->isEnabled() &&
		       !camera->icon().isNull() && camera->icon().pixmap(64, 64).toImage() != mono,
	       "changing palette did not update the selected thumbnail while preserving its selection");
	search->setText("no-such-stagekit-icon");
	expect(camera->isHidden() && items->selectedItems().isEmpty() && !use->isEnabled() &&
		       pins->item(1)->data(Qt::UserRole + 1).toMap() == previous,
	       "a search with no matches retained an applicable hidden icon or changed the saved image");
	search->clear();
	expect(select_combo_option(category, "scenes") && camera->isHidden(),
	       "the Scenes category did not exclude source icons");
	int visible_scenes = 0;
	for (int i = 0; i < items->count(); ++i)
		if (!items->item(i)->isHidden()) {
			++visible_scenes;
			expect(items->item(i)->data(Qt::UserRole).toString().startsWith("scene:"),
			       "the Scenes category included a source or control icon");
		}
	expect(visible_scenes > 0 && select_combo_option(category, "sources") && !camera->isHidden(),
	       "the library categories did not expose their own available artwork");
	search->setText("CaMeRa");
	expect(!camera->isHidden() && select_library_icon(items, "source:camera") && use->isEnabled() &&
		       click(library, "viewsPanelIconLibraryCancel") &&
		       until([&] { return !library || !library->isVisible(); }) &&
		       pins->item(1)->data(Qt::UserRole + 1).toMap() == previous &&
		       pins->item(0)->data(Qt::UserRole + 1).toMap() == first,
	       "Cancel from a filtered library replaced an existing image or changed another control");
	if (library && library->isVisible()) {
		library->reject();
		until([&] { return !library || !library->isVisible(); });
	}
	expect(click(editor, "viewsPanelIconLibrary") && until([&] { return dialog("viewsPanelIconLibraryDialog"); }),
	       "the icon library could not be reopened after cancellation");
	library = dialog("viewsPanelIconLibraryDialog");
	if (!library)
		return;
	palette = library->findChild<QComboBox *>("viewsPanelIconLibraryPalette");
	items = library->findChild<QListWidget *>("viewsPanelIconLibraryItems");
	expect(select_combo_option(palette, "ocean") && select_library_icon(items, "source:camera") &&
		       click(library, "viewsPanelIconLibraryUse") &&
		       until([&] { return !library || !library->isVisible(); }) &&
		       saved_library_icon(pins->item(1)->data(Qt::UserRole + 1).toMap()) &&
		       pins->item(1)->data(Qt::UserRole + 1).toMap().value("iconPng") != previous.value("iconPng") &&
		       pins->item(0)->data(Qt::UserRole + 1).toMap() == first && scene_is(scene),
	       "Use icon did not embed the selected palette on only its control without changing scenes");
	if (library && library->isVisible())
		library->reject();
}

inline QTreeWidgetItem *section_heading(QWidget *panel, const QString &name)
{
	auto *tree = control_list(panel);
	for (int i = 0; tree && i < tree->topLevelItemCount(); ++i)
		if (tree->topLevelItem(i)->text(0) == name)
			return tree->topLevelItem(i);
	return nullptr;
}

inline bool check_saved_panel(QWidget *panel, const char *layout = "list", bool navigate = true,
			      bool require_collapsed = false, bool image_only = false)
{
	if (!click(panel, "viewsPanelEdit") || !until([&] { return dialog("viewsPanelEditor"); }))
		return false;
	QPointer<QDialog> editor = dialog("viewsPanelEditor");
	auto *pins = editor->findChild<QListWidget *>("viewsPanelPins");
	auto *show_scene = editor->findChild<QCheckBox *>("viewsPanelSwitchScene");
	auto *sections = editor->findChild<QComboBox *>("viewsPanelSectionChoice");
	auto *size = panel->findChild<QSlider *>("viewsPanelButtonSize");
	auto *only = panel->findChild<QCheckBox *>("viewsPanelImageOnly");
	const auto names = saved_labels();
	bool valid = pins && pins->count() == names.size() && layout_is(panel, layout) && show_scene &&
		     show_scene->isChecked() == navigate && size && size->value() == 64 && only &&
		     only->isChecked() == image_only && sections && sections->findData("Quick choices") >= 0 &&
		     sections->findData("Visibility") >= 0 && sections->findData("Spare shortcuts") >= 0 &&
		     sections->findData("Discarded section") < 0 && sections->findData("Uncommitted section") < 0;
	if (require_collapsed) {
		const auto *heading = section_heading(panel, "Visibility");
		if (!heading || heading->isExpanded()) {
			blog(LOG_WARNING,
			     "[StageKit controls test] saved collapsed Visibility section was missing or expanded");
			valid = false;
		}
	}
	if (!valid) {
		QStringList section_names;
		for (int i = 0; sections && i < sections->count(); ++i)
			section_names.append(sections->itemData(i).toString());
		const auto *presentation = panel->findChild<QComboBox *>("viewsPanelLayout");
		blog(LOG_WARNING,
		     "[StageKit controls test] saved panel state mismatch: pins=%d/%d; layout=%s/%s; navigation=%d/%d; size=%d/64; sections=%s; require Quick choices, Visibility and unused Spare shortcuts, reject Discarded section and Uncommitted section",
		     pins ? pins->count() : -1, int(names.size()),
		     presentation ? presentation->currentData().toString().toUtf8().constData() : "missing", layout,
		     show_scene ? int(show_scene->isChecked()) : -1, int(navigate), size ? size->value() : -1,
		     section_names.join(" | ").toUtf8().constData());
	}
	for (int i = 0; valid && i < pins->count(); ++i) {
		const auto pin = pins->item(i)->data(Qt::UserRole + 1).toMap();
		const QString id = pin.value("id").toString();
		auto *found = button_id(panel, id);
		auto *row = tree_item(control_list(panel), id);
		valid = found && row && found->text() == names[i] && row->text(0) == names[i] &&
			pin.value("section").toString() == saved_section(i) &&
			target(found).value("id").toString() == id &&
			row->data(0, Qt::UserRole + 1).toMap().value("id").toString() == id;
		if (valid && i < 2)
			valid = embedded_icon(pin) && !found->icon().isNull() && !row->icon(0).isNull();
		if (valid && i == 1)
			valid = saved_library_icon(pin);
		if (valid && i == 7) {
			OBSSourceAutoRelease replacement = obs_get_source_by_name("Studio spare");
			valid = !found->isEnabled() && !(row->flags() & Qt::ItemIsEnabled) && replacement &&
				target(found).value("sourceUuid").toString() != uuid(replacement);
		}
		if (!valid)
			blog(LOG_WARNING,
			     "[StageKit controls test] saved pin mismatch: row=%d; id=%s; button=%s; list=%s; expected=%s; section=%s/%s; embedded=%d; buttonIcon=%d; listIcon=%d; buttonEnabled=%d; listEnabled=%d",
			     i, id.toUtf8().constData(), found ? found->text().toUtf8().constData() : "missing",
			     row ? row->text(0).toUtf8().constData() : "missing", names[i].toUtf8().constData(),
			     pin.value("section").toString().toUtf8().constData(),
			     saved_section(i).toUtf8().constData(), int(embedded_icon(pin)),
			     int(found && !found->icon().isNull()), int(row && !row->icon(0).isNull()),
			     int(found && found->isEnabled()), int(row && (row->flags() & Qt::ItemIsEnabled)));
	}
	editor->reject();
	until([&] { return !editor || !editor->isVisible(); });
	return valid;
}

inline void check_collection_isolation(QWidget *panel, QStringList &issues)
{
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	const QString original = collection_name();
	if (original.isEmpty()) {
		expect(false, "could not identify the current scene collection");
		return;
	}
	// An unsaved draft must not follow the user to another collection or be
	// accidentally committed by the outgoing collection's save callback.
	expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
	       "could not open a draft before changing collections");
	QPointer<QDialog> draft = dialog("viewsPanelEditor");
	if (draft)
		if (auto *label = draft->findChild<QLineEdit *>("viewsPanelLabel"))
			label->setText("Uncommitted draft");
	expect(draft && add_section(draft, "Uncommitted section"),
	       "could not create a draft section before changing collections");
	expect(draft && click(draft, "viewsPanelIconLibrary") &&
		       until([&] { return dialog("viewsPanelIconLibraryDialog"); }),
	       "could not open an icon library draft before changing collections");
	QPointer<QDialog> library = dialog("viewsPanelIconLibraryDialog");
	if (library)
		expect(select_library_icon(library->findChild<QListWidget *>("viewsPanelIconLibraryItems"),
					   "source:camera"),
		       "could not select an uncommitted icon before changing collections");
	const QString temporary = "Panel empty collection";
	const bool created = obs_frontend_add_scene_collection(temporary.toUtf8().constData());
	expect(created && until([&] { return collection_name() == temporary; }),
	       "could not open the second scene collection");
	if (!created)
		return;
	expect(until([&] {
		       return (!draft || !draft->isVisible()) && (!library || !library->isVisible()) &&
			      panel->findChild<QWidget *>("viewsOperatorBoard")->isVisible() && !button(panel, {}) &&
			      panel_ready(panel);
	       }),
	       "collection change retained a panel or icon draft, management page or another collection's pins");
	if (click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); })) {
		QPointer<QDialog> editor = dialog("viewsPanelEditor");
		auto *navigate = editor->findChild<QCheckBox *>("viewsPanelSwitchScene");
		auto *size = panel->findChild<QSlider *>("viewsPanelButtonSize");
		auto *sections = editor->findChild<QComboBox *>("viewsPanelSectionChoice");
		expect(layout_is(panel, "list") && navigate && navigate->isChecked() && size && size->value() == 48,
		       "another collection inherited the first collection's layout, navigation or button size");
		expect(sections && sections->findData("Spare shortcuts") < 0 &&
			       sections->findData("Quick choices") < 0 && sections->findData("Uncommitted section") < 0,
		       "another collection inherited saved or uncommitted custom sections");
		expect(click(editor, "viewsPanelSave") && until([&] { return !editor || !editor->isVisible(); }),
		       "could not save an intentionally empty panel");
	} else {
		expect(false, "empty panel editor did not open");
	}
	{
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		OBSSourceAutoRelease source =
			obs_source_create("stagekit_test_controller_item", "Empty collection sample", nullptr, nullptr);
		if (scene && obs_scene_from_source(scene))
			obs_scene_add(obs_scene_from_source(scene), source);
		OBSSourceAutoRelease filter =
			obs_source_create("stagekit_show_in_scenes", "Empty collection visibility", nullptr, nullptr);
		obs_source_filter_add(source, filter);
		// Opening Edit panel forces a fresh inventory. The new control must be
		// available there while the deliberately empty operator board stays empty.
		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "empty panel could not be edited after adding a control");
		QPointer<QDialog> editor = dialog("viewsPanelEditor");
		if (editor) {
			auto *available = editor->findChild<QTreeWidget *>("viewsPanelAvailable");
			bool discovered = false;
			for (auto *item : tree_controls(available))
				discovered = discovered ||
					     matches(item->data(0, Qt::UserRole + 1).toMap().value("target").toMap(),
						     {{"kind", "show-in-scenes"}, {"filterUuid", uuid(filter)}});
			expect(discovered,
			       "panel editor opened before the new collection's control inventory was available");
			expect(!button(panel, {}), "automatic discovery replaced an intentionally empty panel");
			editor->reject();
		}
	}
	obs_frontend_save();
	// Give OBS time to finish writing the second collection's intentionally
	// empty panel before opening another collection. The native catalog is
	// updated asynchronously while its new source filters are discovered.
	settle(200);
	obs_frontend_set_current_scene_collection(original.toUtf8().constData());
	expect(until([&] { return collection_name() == original && panel_ready(panel) && button(panel, {}); }) &&
		       check_saved_panel(panel, "buttons", false, true),
	       "original pins, labels or order did not return after switching collections");
	obs_frontend_set_current_scene_collection(temporary.toUtf8().constData());
	expect(until([&] { return collection_name() == temporary && panel_ready(panel) && !button(panel, {}); }),
	       "the second collection did not retain its intentionally empty panel");
	obs_frontend_set_current_scene_collection(original.toUtf8().constData());
	expect(until([&] { return collection_name() == original && panel_ready(panel) && button(panel, {}); }) &&
		       check_saved_panel(panel, "buttons", false, true),
	       "collection round trip changed the saved panel");
	obs_frontend_save();
}

inline void check_other_scene_scope(QWidget *panel, const std::string &controller, obs_source_t *owner,
				    QStringList &issues)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	OBSSourceAutoRelease previous = obs_frontend_get_current_scene();
	OBSDataAutoRelease original_state = get(controller);
	OBSDataAutoRelease original_config = obs_data_get_obj(original_state, "config");
	QPointer<QDialog> importing, editor, form;
	QStringList temporary_pins;
	OBSSceneAutoRelease secondary = obs_scene_create("Control panel other scene");
	OBSSceneAutoRelease unrelated = obs_scene_create("Control panel unrelated scene");
	auto *secondary_source = obs_scene_get_source(secondary);
	OBSSourceAutoRelease first =
		obs_source_create("stagekit_test_controller_item", "Scope first camera", nullptr, nullptr);
	OBSSourceAutoRelease second =
		obs_source_create("stagekit_test_controller_item", "Scope second camera", nullptr, nullptr);
	auto *home_first = obs_scene_add(obs_scene_from_source(owner), first);
	auto *home_second = obs_scene_add(obs_scene_from_source(owner), second);
	auto *other_first = obs_scene_add(secondary, first);
	auto *other_second = obs_scene_add(secondary, second);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "mode", 0);
	list(settings, "items", {"Scope first camera", "Scope second camera"});
	obs_data_set_string(settings, "transition", "none");
	OBSSourceAutoRelease link =
		obs_source_create("stagekit_visibility_link", "Scoped camera selection", settings, nullptr);
	obs_source_filter_add(owner, link);
	// Retain every fixture object until cleanup. Scenes created by the
	// frontend also belong to OBS's canvas and need explicit removal.
	struct Restore {
		QWidget *panel;
		obs_source_t *scene, *owner, *secondary, *unrelated, *first, *second, *link;
		obs_sceneitem_t *home_first, *home_second;
		const std::string &controller;
		obs_data_t *config;
		QStringList &issues;
		QPointer<QDialog> &importing, &editor, &form;
		QStringList &pins;
		bool enabled;
		std::string group;
		bool remove_pins()
		{
			if (pins.isEmpty())
				return true;
			if (!click(panel, "viewsBackToControls") || !click(panel, "viewsPanelEdit") ||
			    !until([&] { return dialog("viewsPanelEditor"); }))
				return false;
			editor = dialog("viewsPanelEditor");
			auto *saved = editor->findChild<QListWidget *>("viewsPanelPins");
			QList<int> rows;
			for (const auto &id : pins) {
				const int at = pin_row(saved, id);
				if (at >= 0)
					rows.append(at);
			}
			if ((!rows.isEmpty() && (!select_pins(saved, rows) || !click(editor, "viewsPanelRemove"))) ||
			    !click(editor, "viewsPanelSave") || !until([&] { return !editor || !editor->isVisible(); }))
				return false;
			pins.clear();
			return true;
		}
		~Restore()
		{
			for (const auto &dialog : {form, editor, importing})
				if (dialog && dialog->isVisible())
					dialog->reject();
			if (!remove_pins())
				issues.append("StageKit controls: temporary shared pins could not be removed");
			if (scene)
				obs_frontend_set_current_scene(scene);
			OBSSourceAutoRelease source = obs_get_source_by_uuid(controller.c_str());
			if (source)
				obs_source_set_enabled(source, enabled);
			if (!config || !configure(controller, config) || !until([&] {
				    OBSDataAutoRelease state = get(controller);
				    return !imported_group(controller, link) &&
					   (group.empty() || !row(state, "groups", group));
			    }))
				issues.append("StageKit controls: temporary shared group could not be removed");
			obs_source_filter_remove(owner, link);
			if (home_first)
				obs_sceneitem_remove(home_first);
			if (home_second)
				obs_sceneitem_remove(home_second);
			obs_source_remove(secondary);
			obs_source_remove(unrelated);
			obs_source_remove(first);
			obs_source_remove(second);
			click(panel, "viewsBackToControls");
		}
	} restore{panel,
		  previous,
		  owner,
		  secondary_source,
		  obs_scene_get_source(unrelated),
		  first,
		  second,
		  link,
		  home_first,
		  home_second,
		  controller,
		  original_config,
		  issues,
		  importing,
		  editor,
		  form,
		  temporary_pins,
		  obs_data_get_bool(original_state, "enabled"),
		  {}};
	obs_frontend_set_current_scene(secondary_source);
	if (!home_first || !home_second || !other_first || !other_second || !click(panel, "viewsPanelEdit") ||
	    !until([&] { return dialog("viewsPanelEditor"); })) {
		expect(false, "could not pin original Link Visibility choices before importing the shared fixture");
		return;
	}
	editor = dialog("viewsPanelEditor");
	auto *available = editor->findChild<QTreeWidget *>("viewsPanelAvailable");
	QStringList originals;
	for (auto *source : {first.Get(), second.Get()}) {
		const QVariantMap wanted{{"kind", "link"}, {"filterUuid", uuid(link)}, {"sourceUuid", uuid(source)}};
		for (auto *candidate : tree_controls(available))
			if (matches(candidate->data(0, Qt::UserRole + 1).toMap().value("target").toMap(), wanted)) {
				originals.append(candidate->data(0, Qt::UserRole).toString());
				break;
			}
	}
	temporary_pins = originals;
	if (originals.size() != 2 || !check_inventory(available, originals[0], Qt::Checked) ||
	    !check_inventory(available, originals[1], Qt::Checked) || !click(editor, "viewsPanelSave") ||
	    !until([&] { return !editor || !editor->isVisible(); })) {
		expect(false, "the original Link Visibility choices were not exposed and saved before import");
		return;
	}
	auto *scenes = panel->findChild<QComboBox *>("viewsScene");
	auto *controllers = panel->findChild<QComboBox *>("viewsController");
	auto *scope = panel->findChild<QComboBox *>("viewsScope");
	const QString controller_id = QString::fromStdString(controller);
	if (!scenes || !controllers || !scope || !click(panel, "viewsManageControllers") ||
	    !select_combo_option(scenes, uuid(secondary_source))) {
		expect(false, "could not browse the unrelated scene before configuring shared access");
		return;
	}
	expect(until([&] { return controllers->findData(controller_id) < 0 && scene_is(secondary_source); }),
	       "a controller appeared in a scene outside its configured scope");
	auto open_group = [&](const QString &selected_group) {
		if (!click(panel, "viewsConfigure") || !until([&] { return dialog("viewsEditor"); }))
			return false;
		editor = dialog("viewsEditor");
		if (!click_row(editor->findChild<QListWidget *>("viewsGroups"), selected_group) ||
		    !click(editor, "viewsEditGroup") || !until([&] { return dialog("viewsGroupEditor"); }))
			return false;
		form = dialog("viewsGroupEditor");
		auto *tabs = form->findChild<QTabWidget *>("viewsGroupTabs");
		if (!tabs)
			return false;
		tabs->setCurrentIndex(1);
		return true;
	};
	// Older named-mode definitions keep their shared scenes in settings.
	// Browsing and an untouched editor round trip must honor that saved form.
	OBSDataAutoRelease inherited_config = obs_data_create_from_json(obs_data_get_json(original_config));
	OBSDataAutoRelease inherited_group = row(inherited_config, "groups", "camera");
	OBSDataAutoRelease inherited_settings = obs_data_get_obj(inherited_group, "settings");
	list(inherited_settings, "share_scenes", {"Control panel other scene"});
	if (!configure(controller, inherited_config) ||
	    !until([&] { return controllers->findData(controller_id) >= 0; }) ||
	    !select_combo_option(controllers, controller_id) || !select_combo_option(scope, "camera") ||
	    !open_group("camera")) {
		expect(false, "legacy named-mode shared scenes did not expose the controller and its group");
		return;
	}
	bool inherited_checked = false;
	if (auto *saved_scope = form->findChild<QListWidget *>("viewsGroupScope"))
		for (int i = 0; i < saved_scope->count(); ++i)
			if (saved_scope->item(i)->data(Qt::UserRole).toString() == "Control panel other scene")
				inherited_checked = saved_scope->item(i)->checkState() == Qt::Checked;
	expect(inherited_checked, "the group editor omitted the named mode's inherited shared scene");
	if (!click(form, "viewsGroupSave") || !until([&] { return !form || !form->isVisible(); }) ||
	    !click(editor, "viewsEditorSave") || !until([&] { return !editor || !editor->isVisible(); })) {
		expect(false, "the inherited-scope group could not complete an unchanged editor round trip");
		return;
	}
	expect(until([&] {
		       OBSDataAutoRelease state = get(controller);
		       OBSDataAutoRelease config = obs_data_get_obj(state, "config");
		       const auto saved = json(row(config, "groups", "camera"));
		       const auto original = json(inherited_group);
		       return saved["scope"] == original["scope"] &&
			      saved["settings"].toObject()["share_scenes"] ==
				      original["settings"].toObject()["share_scenes"] &&
			      controllers->currentData().toString() == controller_id && scene_is(secondary_source);
	       }),
	       "an untouched inherited-scope editor changed its saved fields or lost shared access");
	if (!click(panel, "viewsImportLegacy") || !until([&] { return dialog("viewsImportDialog"); })) {
		expect(false, "import could not open while browsing a shared controller from another scene");
		return;
	}
	importing = dialog("viewsImportDialog");
	auto *import_filters = importing->findChild<QComboBox *>("viewsImportFilter");
	auto *import_help = importing->findChild<QLabel *>("viewsImportHelp");
	expect(import_filters && import_filters->findData(uuid(link)) >= 0 && import_help &&
		       import_help->text().contains(QString::fromUtf8(obs_source_get_name(owner))),
	       "shared controller import did not enumerate or identify the actual owner scene");
	importing->reject();
	until([&] { return !importing || !importing->isVisible(); });
	if (!open_group("camera")) {
		expect(false, "the inherited shared scene could not be edited from its shared manager entry");
		return;
	}
	if (auto *saved_scope = form->findChild<QListWidget *>("viewsGroupScope"))
		for (int i = 0; i < saved_scope->count(); ++i)
			if (saved_scope->item(i)->data(Qt::UserRole).toString() == "Control panel other scene")
				saved_scope->item(i)->setCheckState(Qt::Unchecked);
	expect(click(form, "viewsGroupSave") && until([&] { return !form || !form->isVisible(); }) &&
		       click(editor, "viewsEditorSave") && until([&] { return !editor || !editor->isVisible(); }) &&
		       until([&] {
			       OBSDataAutoRelease state = get(controller);
			       OBSDataAutoRelease config = obs_data_get_obj(state, "config");
			       const auto saved = json(row(config, "groups", "camera"));
			       return saved["scope"].toArray().isEmpty() &&
				      saved["settings"].toObject()["share_scenes"].toArray().isEmpty() &&
				      controllers->findData(controller_id) < 0 && scene_is(secondary_source);
		       }),
	       "removing an inherited shared scene left its legacy setting or manager access active");
	if (!configure(controller, original_config) ||
	    !until([&] { return controllers->findData(controller_id) < 0; })) {
		expect(false, "removing inherited shared scope did not remove the secondary manager entry");
		return;
	}
	if (!select_combo_option(scenes, uuid(owner)) ||
	    !select_combo_option(controllers, QString::fromStdString(controller)) ||
	    !click(panel, "viewsImportLegacy") || !until([&] { return dialog("viewsImportDialog"); })) {
		expect(false, "could not prepare the real imported multi-scene control fixture");
		return;
	}
	importing = dialog("viewsImportDialog");
	if (!select_combo_option(importing->findChild<QComboBox *>("viewsImportFilter"), uuid(link)) ||
	    !click(importing, "viewsImportConfirm") ||
	    !until([&] { return (!importing || !importing->isVisible()) && bool(imported_group(controller, link)); })) {
		expect(false, "the multi-scene fixture's existing Link Visibility control was not imported");
		return;
	}
	OBSDataAutoRelease imported = imported_group(controller, link);
	const QString group_id = QString::fromUtf8(obs_data_get_string(imported, "id"));
	restore.group = group_id.toStdString();
	auto scope_saved = [&] {
		OBSDataAutoRelease group = imported_group(controller, link);
		for (const QJsonValue value : json(group)["scope"].toArray())
			if (value.toObject()["value"].toString() ==
			    QString::fromUtf8(obs_source_get_name(secondary_source)))
				return true;
		return false;
	};
	if (!open_group(group_id) ||
	    !check_item(form->findChild<QListWidget *>("viewsGroupScope"), "Control panel other scene")) {
		expect(false, "Other scenes could not be selected in the actual imported group editor");
		return;
	}
	auto *owner_scene = form->findChild<QLabel *>("viewsGroupOwnerScene");
	auto *applies = form->findChild<QLabel *>("viewsGroupAppliesTo");
	expect(owner_scene && owner_scene->isVisible() &&
		       owner_scene->text().contains(QString::fromUtf8(obs_source_get_name(owner))) && applies &&
		       applies->text().contains(QString::fromUtf8(obs_source_get_name(owner))) &&
		       applies->text().contains("Control panel other scene"),
	       "the group editor did not show its always-included owner and selected additional scene");
	if (!click(form, "viewsGroupSave") || !until([&] { return !form || !form->isVisible(); })) {
		expect(false, "the shared group draft could not be returned to the outer configuration editor");
		return;
	}
	auto *pending = editor->findChild<QLabel *>("viewsEditorPending");
	expect(pending && pending->isVisible() && !scope_saved() && scene_is(secondary_source),
	       "inner Done did not identify an unapplied draft or changed native scope or Program early");
	if (!click(editor, "viewsEditorSave") || !until([&] { return !editor || !editor->isVisible(); }) ||
	    !until(scope_saved)) {
		expect(false, "Apply changes did not persist the selected Other scenes scope");
		return;
	}
	if (!open_group(group_id)) {
		expect(false, "the saved multi-scene group could not be reopened");
		return;
	}
	bool retained = false;
	if (auto *saved_scope = form->findChild<QListWidget *>("viewsGroupScope"))
		for (int i = 0; i < saved_scope->count(); ++i)
			if (saved_scope->item(i)->data(Qt::UserRole).toString() == "Control panel other scene")
				retained = saved_scope->item(i)->checkState() == Qt::Checked;
	expect(retained, "reopening the imported group lost its saved Other scenes selection");
	form->reject();
	until([&] { return !form || !form->isVisible(); });
	editor->reject();
	until([&] { return !editor || !editor->isVisible(); });
	const std::string first_choice = choice(controller, group_id.toUtf8().constData(), "Scope first camera");
	const std::string second_choice = choice(controller, group_id.toUtf8().constData(), "Scope second camera");
	if (first_choice.empty() || second_choice.empty() || !select_combo_option(scope, group_id) ||
	    !select_combo_option(scenes, uuid(secondary_source)) || !until([&] {
		    return controllers->currentData().toString() == controller_id &&
			   scope->currentData().toString() == group_id && scene_is(secondary_source) &&
			   !obs_source_showing(owner);
	    })) {
		expect(false,
		       "browsing the additional scene did not expose the same controller and preserve its group");
		return;
	}
	OBSDataAutoRelease shared_state = get(controller);
	auto *summary = panel->findChild<QLabel *>("viewsAppliesTo");
	expect(QString::fromUtf8(obs_data_get_string(shared_state, "sceneUuid")) == uuid(owner) &&
		       controllers->currentText().contains(QString::fromUtf8(obs_source_get_name(owner))) &&
		       scope->findData("camera") < 0 && summary && summary->isVisible() &&
		       summary->text().contains(QString::fromUtf8(obs_source_get_name(owner))) &&
		       summary->text().contains("Control panel other scene"),
	       "shared manager access lost its owner identity, exposed an unrelated group or omitted effective scenes");
	auto *choices = panel->findChild<QListWidget *>("viewsList");
	expect(click_row(choices, QString::fromStdString(second_choice)) && until([&] {
		       return !obs_sceneitem_visible(home_first) && obs_sceneitem_visible(home_second) &&
			      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
			      current(controller, group_id.toUtf8().constData()) == second_choice;
	       }),
	       "an imported choice did not update both scenes while Program was outside its owner");
	expect(click_row(choices, QString::fromStdString(first_choice)) && until([&] {
		       return obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
			      obs_sceneitem_visible(other_first) && !obs_sceneitem_visible(other_second) &&
			      current(controller, group_id.toUtf8().constData()) == first_choice;
	       }),
	       "the opposite imported choice did not update both scoped scenes");
	settle(300);
	expect(scene_is(secondary_source) && obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
		       obs_sceneitem_visible(other_first) && !obs_sceneitem_visible(other_second),
	       "the scoped visibility choice changed Program or was undone after settling");
	const QString capture = qEnvironmentVariable("STAGEKIT_VIEWS_CAPTURE");
	if (!capture.isEmpty()) {
		const QDir directory = QFileInfo(capture).isDir() ? QDir(capture) : QFileInfo(capture).absoluteDir();
		QDockWidget *dock = nullptr;
		for (auto *parent = panel->parentWidget(); !dock && parent; parent = parent->parentWidget())
			dock = qobject_cast<QDockWidget *>(parent);
		if (dock)
			dock->resize(520, 600);
		QCoreApplication::processEvents();
		expect(panel->grab().save(directory.filePath("stagekit-shared-controls.png")),
		       "could not capture controller access from its additional scene");
	}
	expect(select_combo_option(scenes, uuid(obs_scene_get_source(unrelated))) &&
		       until([&] { return controllers->findData(controller_id) < 0 && scene_is(secondary_source); }) &&
		       select_combo_option(scenes, uuid(secondary_source)) &&
		       until([&] { return controllers->findData(controller_id) >= 0; }) &&
		       select_combo_option(controllers, controller_id) && select_combo_option(scope, group_id),
	       "shared management access leaked to an unrelated scene or could not be reopened");

	if (!click(panel, "viewsBackToControls") || !click(panel, "viewsPanelEdit") ||
	    !until([&] { return dialog("viewsPanelEditor"); })) {
		expect(false, "the imported shared controls could not be pinned on the operator panel");
		return;
	}
	editor = dialog("viewsPanelEditor");
	available = editor->findChild<QTreeWidget *>("viewsPanelAvailable");
	QStringList pinned;
	for (const auto &choice_id : {first_choice, second_choice}) {
		const QVariantMap wanted{{"kind", "controller"},
					 {"controllerId", QString::fromStdString(controller)},
					 {"groupId", group_id},
					 {"choiceId", QString::fromStdString(choice_id)}};
		for (auto *candidate : tree_controls(available))
			if (!originals.contains(candidate->data(0, Qt::UserRole).toString()) &&
			    matches(candidate->data(0, Qt::UserRole + 1).toMap().value("target").toMap(), wanted)) {
				pinned.append(candidate->data(0, Qt::UserRole).toString());
				break;
			}
	}
	temporary_pins.append(pinned);
	if (pinned.size() != 2 || !check_inventory(available, pinned[0], Qt::Checked) ||
	    !check_inventory(available, pinned[1], Qt::Checked) || !click(editor, "viewsPanelSave") ||
	    !until([&] { return !editor || !editor->isVisible(); })) {
		expect(false, "the operator editor did not expose the imported controller choices as distinct targets");
		return;
	}
	auto panel_selected = [&](bool want_first) {
		return scene_is(secondary_source) && !obs_source_showing(owner) &&
		       obs_sceneitem_visible(home_first) == want_first &&
		       obs_sceneitem_visible(other_first) == want_first &&
		       obs_sceneitem_visible(home_second) != want_first &&
		       obs_sceneitem_visible(other_second) != want_first &&
		       checked(panel, pinned[want_first ? 0 : 1]) && !checked(panel, pinned[want_first ? 1 : 0]) &&
		       checked(panel, originals[want_first ? 0 : 1]) && !checked(panel, originals[want_first ? 1 : 0]);
	};
	expect(until([&] {
		       for (int i = 0; i < originals.size(); ++i) {
			       auto *original = button_id(panel, originals[i]);
			       const QVariantMap wanted{{"kind", "link"},
							{"filterUuid", uuid(link)},
							{"sourceUuid", uuid(i == 0 ? first.Get() : second.Get())}};
			       if (!original || !matches(target(original), wanted))
				       return false;
		       }
		       return true;
	       }),
	       "original Link Visibility pins did not retain their saved identities and metadata after import");
	expect(until([&] { return button_id(panel, pinned[0]) && button_id(panel, pinned[1]); }) &&
		       press_control(panel, pinned[1]) && until([&] { return panel_selected(false); }) &&
		       press_control(panel, pinned[0]) && until([&] { return panel_selected(true); }),
	       "pinned controller choices did not update both layouts while preserving the other Program scene");
	expect(press_control(panel, originals[1]) && until([&] { return panel_selected(false); }) &&
		       press_control(panel, originals[0]) && until([&] { return panel_selected(true); }),
	       "original pins still used only their owner's compatibility binding after importing shared scope");
	obs_sceneitem_set_visible(other_second, true);
	expect(until([&] {
		       return obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
			      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
			      !checked(panel, originals[0]) && !checked(panel, originals[1]) &&
			      !checked(panel, pinned[0]) && !checked(panel, pinned[1]);
	       }) && press_control(panel, originals[0]) &&
		       until([&] { return panel_selected(true); }),
	       "original pins did not report a mixed shared state or repair the other scene when reselected");

	if (!click(panel, "viewsManageControllers") || !click(panel, "viewsEnabled") || !until([&] {
		    OBSDataAutoRelease state = get(controller);
		    return !obs_data_get_bool(state, "enabled");
	    }) ||
	    !click(panel, "viewsBackToControls")) {
		expect(false, "the shared controller could not be disabled from its additional scene");
		return;
	}
	auto controls_enabled = [&](const QStringList &ids, bool enabled) {
		for (const auto &id : ids) {
			const auto *found = button_id(panel, id);
			const auto *item = tree_item(control_list(panel), id);
			if (!found || found->isEnabled() != enabled || !item ||
			    bool(item->flags() & Qt::ItemIsEnabled) != enabled)
				return false;
		}
		return true;
	};
	expect(until([&] { return controls_enabled(temporary_pins, false); }) && !press_control(panel, originals[1]) &&
		       !press_control(panel, pinned[1]) && scene_is(secondary_source),
	       "disabling a shared controller left original or controller pins actionable");
	if (!click(panel, "viewsManageControllers") || !click(panel, "viewsEnabled") ||
	    !click(panel, "viewsBackToControls") || !until([&] { return controls_enabled(temporary_pins, true); })) {
		expect(false, "reenabling the shared controller did not restore both kinds of pinned control");
		return;
	}
	obs_sceneitem_remove(other_second);
	other_second = nullptr;
	expect(until([&] {
		       return controls_enabled({originals[1], pinned[1]}, false) &&
			      controls_enabled({originals[0], pinned[0]}, true);
	       }) && !press_control(panel, originals[1]) &&
		       !press_control(panel, pinned[1]) && scene_is(secondary_source),
	       "a member missing only in the additional scene did not disable its original and controller pins");
	other_second = obs_scene_add(secondary, second);
	expect(other_second && until([&] { return controls_enabled(temporary_pins, true); }) &&
		       press_control(panel, originals[1]) && until([&] { return panel_selected(false); }),
	       "restoring a shared member did not restore both pin kinds and full-scope selection");
	{
		// Hold the video queue without retaining any OBS or graphics locks so
		// this real panel press is accepted before its original is detached,
		// then executed after detachment. A bounded wait cannot hang OBS.
		struct Barrier {
			std::mutex mutex;
			std::condition_variable changed;
			std::atomic_bool entered{false}, finished{false}, expired{false};
			bool released = false;
			void release()
			{
				{
					std::lock_guard lock(mutex);
					released = true;
				}
				changed.notify_all();
			}
		};
		auto barrier = std::make_shared<Barrier>();
		struct Release {
			std::shared_ptr<Barrier> barrier;
			~Release() { barrier->release(); }
		} release{barrier};
		obs_queue_task(
			OBS_TASK_GRAPHICS,
			[](void *opaque) {
				std::unique_ptr<std::shared_ptr<Barrier>> held(
					static_cast<std::shared_ptr<Barrier> *>(opaque));
				auto &barrier = **held;
				std::unique_lock lock(barrier.mutex);
				barrier.entered = true;
				barrier.expired = !barrier.changed.wait_for(lock, std::chrono::seconds(8),
									    [&] { return barrier.released; });
				barrier.finished = true;
			},
			new std::shared_ptr<Barrier>(barrier), false);
		const bool queued = until([&] { return barrier->entered.load(); }) &&
				    press_control(panel, originals[0]);
		obs_source_filter_remove(owner, link);
		barrier->release();
		expect(queued && until([&] {
			       OBSDataAutoRelease state = get(controller);
			       return barrier->finished.load() && !barrier->expired.load() &&
				      QString::fromUtf8(obs_data_get_string(state, "error"))
					      .contains("original panel control") &&
				      !obs_sceneitem_visible(home_first) && obs_sceneitem_visible(home_second) &&
				      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
				      scene_is(secondary_source);
		       }),
		       "a queued original pin changed a scoped scene after its original filter was detached");
		expect(press_control(panel, pinned[0]) && until([&] {
			       return obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
				      obs_sceneitem_visible(other_first) && !obs_sceneitem_visible(other_second) &&
				      current(controller, group_id.toUtf8().constData()) == first_choice &&
				      scene_is(secondary_source);
		       }),
		       "detaching the original prevented its retained controller group from controlling both scenes");
		obs_source_filter_add(owner, link);
		expect(until([&] { return controls_enabled(temporary_pins, true); }) &&
			       press_control(panel, originals[1]) && until([&] { return panel_selected(false); }),
		       "reattaching the same original did not restore its stable shared pins");
	}

	if (!click(panel, "viewsManageControllers") || !select_combo_option(scenes, uuid(secondary_source)) ||
	    !select_combo_option(controllers, controller_id) || !select_combo_option(scope, group_id)) {
		expect(false, "the shared controller could not be reopened before removing its additional scene");
		return;
	}
	obs_source_set_name(secondary_source, "Control panel other scene renamed");
	expect(until([&] {
		       return scope_saved() && scenes->currentData().toString() == uuid(secondary_source) &&
			      controllers->currentData().toString() == controller_id &&
			      scope->currentData().toString() == group_id && summary &&
			      summary->text().contains("Control panel other scene renamed") &&
			      choice(controller, group_id.toUtf8().constData(), "Scope first camera") == first_choice &&
			      choice(controller, group_id.toUtf8().constData(), "Scope second camera") == second_choice;
	       }),
	       "renaming an additional scene lost shared manager access, its summary or choice identities");
	if (!open_group(group_id)) {
		expect(false, "the additional scene could not be edited from its shared manager entry");
		return;
	}
	bool removed = false;
	if (auto *saved_scope = form->findChild<QListWidget *>("viewsGroupScope"))
		for (int i = 0; i < saved_scope->count(); ++i)
			if (saved_scope->item(i)->data(Qt::UserRole).toString() ==
			    "Control panel other scene renamed") {
				saved_scope->item(i)->setCheckState(Qt::Unchecked);
				removed = true;
			}
	if (!removed || !click(form, "viewsGroupSave") || !until([&] { return !form || !form->isVisible(); }) ||
	    !click(editor, "viewsEditorSave") || !until([&] { return !editor || !editor->isVisible(); }) ||
	    !until([&] { return !scope_saved() && controllers->findData(controller_id) < 0; })) {
		expect(false, "removing an additional scene did not remove shared manager access after Apply changes");
		return;
	}
	expect(scene_is(secondary_source) && select_combo_option(scenes, uuid(owner)) &&
		       select_combo_option(controllers, controller_id) && select_combo_option(scope, group_id) &&
		       click_row(choices, QString::fromStdString(first_choice)) && until([&] {
			       return obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
				      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
				      scene_is(secondary_source);
		       }),
	       "removing shared access deleted its original group, changed Program or kept controlling the removed scene");
	expect(configure(controller, original_config) && until([&] { return !imported_group(controller, link); }) &&
		       click(panel, "viewsBackToControls") && until([&] {
			       return matches(target(button_id(panel, originals[0])), {{"kind", "link"},
										       {"filterUuid", uuid(link)},
										       {"sourceUuid", uuid(first)}}) &&
				      matches(target(button_id(panel, originals[1])), {{"kind", "link"},
										       {"filterUuid", uuid(link)},
										       {"sourceUuid", uuid(second)}});
		       }),
	       "removing the imported group did not restore the same original pins to their native controls");
	expect(until([&] { return controls_enabled(originals, true); }) && press_control(panel, originals[1]) &&
		       until([&] {
			       return !obs_sceneitem_visible(home_first) && obs_sceneitem_visible(home_second) &&
				      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
				      scene_is(secondary_source);
		       }) &&
		       press_control(panel, originals[0]) && until([&] {
			       return obs_sceneitem_visible(home_first) && !obs_sceneitem_visible(home_second) &&
				      !obs_sceneitem_visible(other_first) && obs_sceneitem_visible(other_second) &&
				      checked(panel, originals[0]) && !checked(panel, originals[1]) &&
				      scene_is(secondary_source);
		       }),
	       "original pins did not resume native owner-only selection after the imported group was removed");
	expect(restore.remove_pins(), "removing the temporary scoped controls did not restore the saved panel");
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[StageKit controls test] PASS: imported shared scope stays draft until Apply changes; the same controller is accessible from covered scenes only; original and controller pins drive both layouts, reflect mixed and unavailable state, survive scene rename, and restore native ownership when shared access is removed without changing Program");
}

inline void check(QStringList &issues, bool restart)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = main ? main->findChild<QWidget *>("stagekitViewsDock") : nullptr;
	if (!panel || !click(panel, "viewsBackToControls")) {
		expect(false, "operator panel was not available from controller management");
		return;
	}
	auto *board = panel->findChild<QWidget *>("viewsOperatorBoard");
	expect(board && board->isVisible(), "operator panel was not visible");
	if (restart) {
		expect(until([&] { return panel_ready(panel) && button(panel, {}); }) &&
			       check_saved_panel(panel, "buttons", false, true, true),
		       "saved pins, target identities, labels, sections or order did not survive restart");
		expect(select_layout(panel, "list") && set_show_scene(panel, true) &&
			       check_saved_panel(panel, "list", true, true, true),
		       "restored layout or scene navigation could not be changed after OBS restart");
		if (issues.size() == before)
			blog(LOG_INFO,
			     "[StageKit controls test] PASS: saved nondefault layout and navigation, custom and collapsed sections, embedded icons, button size, renamed source and disabled missing target restored after OBS restart");
		return;
	}

	// Keep every native reference inside this scope so the later real scene
	// collection switch can release the old collection normally.
	{
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		const bool was_studio = obs_frontend_preview_program_mode_active();
		OBSSourceAutoRelease old_preview = was_studio ? obs_frontend_get_current_preview_scene() : nullptr;
		struct RestoreScenes {
			bool studio;
			obs_source_t *program, *preview;
			~RestoreScenes()
			{
				obs_frontend_set_preview_program_mode(false);
				if (program)
					obs_frontend_set_current_scene(program);
				if (studio) {
					obs_frontend_set_preview_program_mode(true);
					if (preview)
						obs_frontend_set_current_preview_scene(preview);
				}
			}
		} restore{was_studio, program, old_preview};
		obs_frontend_set_preview_program_mode(false);
		auto *size = panel->findChild<QSlider *>("viewsPanelButtonSize");
		expect(layout_is(panel, "list") && control_list(panel) && control_list(panel)->isVisible() && size &&
			       size->minimum() == 24 && size->maximum() == 112 && size->value() == 48,
		       "new panel did not default to a compact list with the expected button-size range");
		auto *scenes = panel->findChild<QComboBox *>("viewsScene");
		auto *controllers = panel->findChild<QComboBox *>("viewsController");
		auto *scope = panel->findChild<QComboBox *>("viewsScope");
		if (!scenes || !controllers || !scope)
			return;
		const auto old_scene = scenes->currentData(), old_controller = controllers->currentData(),
			   old_scope = scope->currentData();
		OBSDataAutoRelease existing = find_controller("Stream views");
		const QString first_id = QString::fromUtf8(obs_data_get_string(existing, "controllerId"));
		OBSDataArrayAutoRelease old_views = obs_data_get_array(existing, "views");
		QString first_view, destination_view;
		for (size_t i = 0; i < obs_data_array_count(old_views); ++i) {
			OBSDataAutoRelease view = obs_data_array_item(old_views, i);
			if (std::strcmp(obs_data_get_string(view, "name"), "Game and camera") == 0)
				first_view = QString::fromUtf8(obs_data_get_string(view, "id"));
			if (std::strcmp(obs_data_get_string(view, "name"), "Second layout") == 0)
				destination_view = QString::fromUtf8(obs_data_get_string(view, "id"));
		}
		OBSSourceAutoRelease first_scene = obs_get_source_by_name("Views demo layout");
		OBSSourceAutoRelease destination_scene = obs_get_source_by_name("Views demo second layout");
		OBSSceneAutoRelease studio = obs_scene_create("Control panel studio");
		auto *scene = obs_scene_get_source(studio);
		auto add_item = [&](const char *name) {
			OBSSourceAutoRelease source =
				obs_source_create("stagekit_test_controller_item", name, nullptr, nullptr);
			return obs_scene_add(studio, source);
		};
		auto *main_camera = add_item("Studio main camera");
		auto *detail_camera = add_item("Studio detail camera");
		auto *legacy_main = add_item("Studio front view");
		auto *legacy_detail = add_item("Studio side view");
		auto *nested = obs_scene_add_group(studio, "Studio camera group");
		auto *nested_camera = add_item("Studio grouped camera");
		obs_sceneitem_group_add_item(nested, nested_camera);
		auto *wide_camera = add_item("Studio wide camera");
		auto *watermark = add_item("Studio watermark");
		auto *title = add_item("Studio title");
		auto *spare = add_item("Studio spare");
		add_item("Studio overlay note");
		add_item("Studio overlay badge");
		expect(click(panel, "viewsManageControllers") &&
			       until([&] { return scenes->findData(uuid(scene)) >= 0; }),
		       "new scene was not available for controller setup");
		scenes->setCurrentIndex(scenes->findData(uuid(scene)));
		expect(click(panel, "viewsCreate") && until([&] { return dialog("viewsCreateDialog"); }),
		       "Add controller did not open its name dialog");
		QPointer<QDialog> naming = dialog("viewsCreateDialog");
		if (!naming)
			return;
		auto *controller_name = naming->findChild<QLineEdit *>("viewsControllerName");
		if (controller_name)
			controller_name->setText("Studio controls");
		expect(controller_name && click(naming, "viewsControllerNameSave") &&
			       until([&] { return !naming || !naming->isVisible(); }) && !dialog("viewsEditor"),
		       "Add controller did not leave Import available directly in management");
		OBSDataAutoRelease created = find_controller("Studio controls");
		const std::string id = obs_data_get_string(created, "controllerId");
		expect(!id.empty() && controllers->currentData().toString() == QString::fromStdString(id),
		       "newly added controller was not selected");
		expect(click(panel, "viewsBackToControls"), "could not return to the operator panel after setup");
		OBSDataAutoRelease config = obs_data_create();
		obs_data_set_int(config, "schema", 1);
		OBSDataArrayAutoRelease groups = obs_data_array_create();
		OBSDataAutoRelease camera_settings = modes("Studio main camera", "Studio detail camera");
		add_group(groups, "camera", "Studio camera", "modes", camera_settings);
		obs_data_set_array(config, "groups", groups);
		expect(!id.empty() && configure(id, config), "could not configure the second scene controller");
		OBSDataAutoRelease legacy_settings = modes("Studio front view", "Studio side view");
		obs_data_set_int(legacy_settings, "mode|detail|place_ms", 725);
		obs_data_set_string(legacy_settings, "for|Studio side view|hide_transition", "fade_transition");
		obs_data_set_int(legacy_settings, "for|Studio side view|hide_ms", 110);
		OBSSourceAutoRelease legacy =
			obs_source_create("stagekit_scene_modes", "Studio angles", legacy_settings, nullptr);
		obs_source_filter_add(scene, legacy);
		OBSDataAutoRelease link_settings = obs_data_create();
		obs_data_set_int(link_settings, "mode", 0);
		list(link_settings, "items", {"Studio camera group", "Studio wide camera"});
		obs_data_set_string(link_settings, "transition", "none");
		OBSSourceAutoRelease link = obs_source_create("stagekit_visibility_link", "Studio camera selection",
							      link_settings, nullptr);
		obs_source_filter_add(scene, link);
		OBSDataAutoRelease disabled_settings = obs_data_create();
		obs_data_set_int(disabled_settings, "mode", 1);
		list(disabled_settings, "items", {"Studio overlay note", "Studio overlay badge"});
		obs_data_set_int(disabled_settings, "transition_ms", 312);
		OBSSourceAutoRelease disabled =
			obs_source_create("stagekit_visibility_link", "Studio overlays", disabled_settings, nullptr);
		obs_source_filter_add(scene, disabled);
		obs_source_set_enabled(disabled, false);
		OBSDataAutoRelease show_settings = obs_data_create();
		list(show_settings, "sources", {"Studio watermark"});
		OBSSourceAutoRelease show =
			obs_source_create("stagekit_show_in_scenes", "Watermark visibility", show_settings, nullptr);
		obs_source_filter_add(obs_sceneitem_get_source(watermark), show);

		const QVariantMap first_target{{"kind", "controller"},
					       {"controllerId", first_id},
					       {"viewId", first_view}};
		const QVariantMap second_target{{"kind", "controller"},
						{"controllerId", QString::fromStdString(id)},
						{"groupId", "camera"},
						{"choiceId", "detail"}};
		const QVariantMap mode_target{{"kind", "mode"}, {"filterUuid", uuid(legacy)}, {"modeId", "detail"}};
		const QVariantMap link_target{{"kind", "link"},
					      {"filterUuid", uuid(link)},
					      {"sourceUuid", uuid(obs_sceneitem_get_source(nested))}};
		const QVariantMap show_target{{"kind", "show-in-scenes"}, {"filterUuid", uuid(show)}};
		expect(!first_id.isEmpty() && !first_view.isEmpty() && until([&] {
			return button(panel, first_target) && button(panel, second_target) &&
			       button(panel, mode_target) && button(panel, link_target) && button(panel, show_target);
		}),
		       "automatic panel did not show both controller scenes and standalone controls together");
		const QString first = control_id(panel, first_target), second = control_id(panel, second_target),
			      mode = control_id(panel, mode_target), linked = control_id(panel, link_target),
			      shown = control_id(panel, show_target);
		if (first.isEmpty() || second.isEmpty() || mode.isEmpty() || linked.isEmpty() || shown.isEmpty())
			return;
		expect(button_id(panel, shown)->text() == "Studio watermark",
		       "a Show in Scenes control did not use its owning source name");
		{
			OBSSourceAutoRelease other = obs_source_create("stagekit_show_in_scenes",
								       "Alternate visibility", show_settings, nullptr);
			obs_source_set_enabled(other, false);
			obs_source_filter_add(obs_sceneitem_get_source(watermark), other);
			const QVariantMap other_target{{"kind", "show-in-scenes"}, {"filterUuid", uuid(other)}};
			expect(until([&] {
				       auto *original = button_id(panel, shown);
				       auto *additional = button(panel, other_target);
				       return original && additional &&
					      original->text() == "Studio watermark / Watermark visibility" &&
					      additional->text() == "Studio watermark / Alternate visibility";
			       }),
			       "several Show in Scenes controls on one source did not include their distinct filter names");
			const QString other_id = control_id(panel, other_target);
			obs_source_set_name(show, "Primary visibility");
			obs_source_set_name(obs_sceneitem_get_source(watermark), "Studio watermark renamed");
			expect(!other_id.isEmpty() && until([&] {
				auto *original = button_id(panel, shown);
				auto *additional = button_id(panel, other_id);
				return original && additional && control_id(panel, show_target) == shown &&
				       control_id(panel, other_target) == other_id &&
				       original->text() == "Studio watermark renamed / Primary visibility" &&
				       additional->text() == "Studio watermark renamed / Alternate visibility";
			}),
			       "renaming a Show in Scenes source or filter changed its control identity or kept an old label");
			obs_source_filter_remove(obs_sceneitem_get_source(watermark), other);
			expect(until([&] {
				       auto *original = button_id(panel, shown);
				       return original && original->text() == "Studio watermark renamed" &&
					      !button(panel, other_target);
			       }),
			       "removing a second Show in Scenes filter did not restore the source-only label");
			obs_source_set_name(show, "Watermark visibility");
			obs_source_set_name(obs_sceneitem_get_source(watermark), "Studio watermark");
			expect(until([&] {
				       return button_id(panel, shown) &&
					      button_id(panel, shown)->text() == "Studio watermark";
			       }),
			       "restoring the source name did not update the same Show in Scenes control");
		}
		expect(first_scene && destination_scene && !destination_view.isEmpty(),
		       "navigation fixture scenes or the explicit combined view were missing");
		if (!first_scene || !destination_scene || destination_view.isEmpty())
			return;
		obs_frontend_set_current_scene(scene);
		expect(until([&] { return checked(panel, first); }) && press_control(panel, first) &&
			       until([&] { return scene_is(first_scene); }),
		       "an already-active combined view did not show its own scene from the compact list");
		expect(press_control(panel, second) && until([&] {
			       return current(id, "camera") == "detail" && obs_sceneitem_visible(detail_camera) &&
				      !obs_sceneitem_visible(main_camera) && checked(panel, second) && scene_is(scene);
		       }),
		       "a second scene's mode was not applied and displayed from the same panel");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, second) && until([&] { return scene_is(scene); }),
		       "clicking the already-current list row did not return to its scene");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, mode) && until([&] {
			       return obs_sceneitem_visible(legacy_detail) && !obs_sceneitem_visible(legacy_main) &&
				      checked(panel, mode) && scene_is(scene);
		       }),
		       "standalone Scene Modes did not apply and show their own scene");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, mode) && until([&] { return scene_is(scene); }),
		       "an already-active standalone mode did not return to its scene");
		OBSDataArrayAutoRelease first_groups = obs_data_get_array(existing, "groups");
		std::string visibility_group;
		for (size_t i = 0; i < obs_data_array_count(first_groups); ++i) {
			OBSDataAutoRelease group = obs_data_array_item(first_groups, i);
			if (std::strcmp(obs_data_get_string(group, "name"), "Camera") == 0)
				visibility_group = obs_data_get_string(group, "id");
		}
		const std::string close_choice =
			choice(first_id.toStdString(), visibility_group.c_str(), "Views demo close camera");
		const QString close = control_id(panel, {{"kind", "controller"},
							 {"controllerId", first_id},
							 {"groupId", QString::fromStdString(visibility_group)},
							 {"choiceId", QString::fromStdString(close_choice)}});
		expect(!visibility_group.empty() && !close_choice.empty() && !close.isEmpty() &&
			       press_control(panel, close) && until([&] {
				       return current(first_id.toStdString(), visibility_group.c_str()) ==
						      close_choice &&
					      checked(panel, close);
			       }) &&
			       scene_is(scene),
		       "a controller visibility choice added an unexpected parent-scene switch");

		const QString destination = control_id(
			panel, {{"kind", "controller"}, {"controllerId", first_id}, {"viewId", destination_view}});
		expect(!destination.isEmpty() && press_control(panel, destination) &&
			       until([&] { return scene_is(destination_scene); }),
		       "a combined view's explicit destination did not override its parent scene");
		obs_data_set_string(legacy_settings, "mode|detail|go_to", "Views demo second layout");
		obs_source_update(legacy, legacy_settings);
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, mode) && until([&] { return scene_is(destination_scene); }),
		       "a standalone mode's explicit destination did not override its parent scene");
		obs_data_set_string(legacy_settings, "mode|detail|go_to", "");
		obs_source_update(legacy, legacy_settings);
		obs_data_set_string(camera_settings, "mode|detail|go_to", "Views demo second layout");
		expect(configure(id, config) && until([&] {
			       OBSDataAutoRelease state = get(id);
			       OBSDataAutoRelease definition = obs_data_get_obj(state, "config");
			       OBSDataAutoRelease selected = row(definition, "groups", "camera");
			       OBSDataAutoRelease settings = obs_data_get_obj(selected, "settings");
			       return std::strcmp(obs_data_get_string(settings, "mode|detail|go_to"),
						  "Views demo second layout") == 0;
		       }),
		       "could not apply the controller mode destination before selecting it");
		settle(150);
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, second) && until([&] { return scene_is(destination_scene); }),
		       "a controller mode's explicit destination did not override its parent scene");
		obs_data_set_string(camera_settings, "mode|detail|go_to", "");
		expect(configure(id, config) && until([&] {
			       OBSDataAutoRelease state = get(id);
			       OBSDataAutoRelease definition = obs_data_get_obj(state, "config");
			       OBSDataAutoRelease selected = row(definition, "groups", "camera");
			       OBSDataAutoRelease settings = obs_data_get_obj(selected, "settings");
			       return std::strlen(obs_data_get_string(settings, "mode|detail|go_to")) == 0;
		       }),
		       "could not restore the controller mode's parent-scene fallback");
		settle(150);

		expect(set_show_scene(panel, false), "could not disable automatic panel scene navigation");
		obs_frontend_set_current_scene(first_scene);
		expect(press(id, "camera", "front") && until([&] { return current(id, "camera") == "front"; }) &&
			       press_control(panel, second) &&
			       until([&] { return current(id, "camera") == "detail" && checked(panel, second); }) &&
			       scene_is(first_scene),
		       "disabling panel navigation did not preserve Program while applying a selection");
		expect(press_control(panel, destination) && until([&] { return scene_is(destination_scene); }),
		       "panel navigation opt-out suppressed a view's configured explicit destination");
		expect(set_show_scene(panel, true), "could not restore automatic panel scene navigation");
		obs_frontend_set_current_scene(first_scene);
		obs_frontend_set_preview_program_mode(true);
		obs_frontend_set_current_preview_scene(destination_scene);
		expect(press_control(panel, second) &&
			       until([&] { return scene_is(scene, true) && scene_is(first_scene); }),
		       "a panel selection in Studio Mode did not use Preview while preserving Program");
		expect(press_control(panel, destination) &&
			       until([&] { return scene_is(destination_scene, true) && scene_is(first_scene); }),
		       "an explicit panel destination in Studio Mode changed Program instead of Preview");
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(first_scene);
		expect(key_tree(control_list(panel), tree_item(control_list(panel), second), Qt::Key_Return) &&
			       until([&] { return scene_is(scene); }),
		       "keyboard Enter did not activate the current compact-list row");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, linked) && until([&] {
			       return obs_sceneitem_visible(nested) && !obs_sceneitem_visible(wide_camera) &&
				      checked(panel, linked) && scene_is(first_scene);
		       }),
		       "Link Visibility did not choose its OBS group without adding a scene switch");
		QPointer<QAbstractButton> unchanged = button_id(panel, linked);
		auto *unchanged_row = tree_item(control_list(panel), linked);
		auto *collapsed_section = unchanged_row ? unchanged_row->parent() : nullptr;
		if (collapsed_section)
			collapsed_section->setExpanded(false);
		obs_sceneitem_set_visible(wide_camera, true);
		expect(until([&] {
			       return !obs_sceneitem_visible(nested) && obs_sceneitem_visible(wide_camera) &&
				      unchanged && unchanged == button_id(panel, linked) && !unchanged->isChecked() &&
				      unchanged_row && unchanged_row == tree_item(control_list(panel), linked) &&
				      !unchanged_row->data(0, Qt::UserRole + 2).toBool() && collapsed_section &&
				      !collapsed_section->isExpanded();
		       }),
		       "external state rebuilt a control, reset a collapsed section or left its active highlight stale");
		obs_source_set_enabled(show, false);
		control_list(panel)->setCurrentItem(tree_item(control_list(panel), shown));
		QCoreApplication::processEvents();
		expect(!obs_source_enabled(show) && scene_is(first_scene),
		       "restoring the compact list's current row invoked its visibility control");
		expect(until([&] { return button_id(panel, shown) && !button_id(panel, shown)->isChecked(); }) &&
			       key_tree(control_list(panel), tree_item(control_list(panel), shown), Qt::Key_Space) &&
			       until([&] { return obs_source_enabled(show) && checked(panel, shown); }) &&
			       scene_is(first_scene),
		       "Show in Scenes did not display external state or toggle its native filter");
		expect(double_click_tree(control_list(panel), tree_item(control_list(panel), shown)) &&
			       until([&] { return !obs_source_enabled(show) && !checked(panel, shown); }),
		       "a compact-list double click dispatched a visibility toggle twice");

		auto *scene_selector = panel->findChild<QComboBox *>("viewsPanelScene");
		expect(until([&] {
			       return scene_selector && scene_selector_index(scene_selector, scene) >= 0 &&
				      scene_selector_index(scene_selector, first_scene) >= 0 &&
				      scene_selector->currentIndex() ==
					      scene_selector_index(scene_selector, first_scene);
		       }),
		       "the direct scene selector did not list scenes or synchronize to Program");
		const std::string selected_camera = current(id, "camera");
		expect(select_scene(scene_selector, scene) && until([&] {
			       return scene_is(scene) &&
				      scene_selector->currentIndex() == scene_selector_index(scene_selector, scene);
		       }) && current(id, "camera") == selected_camera,
		       "the direct scene selector did not change scenes independently of a mode");
		obs_frontend_set_current_scene(first_scene);
		expect(until([&] {
			       return scene_selector && scene_selector->currentIndex() ==
								scene_selector_index(scene_selector, first_scene);
		       }) && scene_is(first_scene),
		       "an external scene change did not update the selector without navigating back");
		obs_frontend_set_preview_program_mode(true);
		obs_frontend_set_current_preview_scene(destination_scene);
		expect(select_scene(scene_selector, scene) && until([&] {
			       return scene_is(scene, true) && scene_is(first_scene) &&
				      scene_selector->currentIndex() == scene_selector_index(scene_selector, scene);
		       }),
		       "the direct scene selector changed Program instead of Preview in Studio Mode");
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(first_scene);

		// The remainder of this fixture customizes and saves these same controls,
		// then imports their originals without changing their pinned identities.
		QStringList ids{first, second, mode, linked, shown};
		QPointer<QDialog> editor;
		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "panel editor did not open");
		editor = dialog("viewsPanelEditor");
		if (!editor)
			return;
		auto *pins = editor->findChild<QListWidget *>("viewsPanelPins");
		auto *available = editor->findChild<QTreeWidget *>("viewsPanelAvailable");
		auto *show_scene = editor->findChild<QCheckBox *>("viewsPanelSwitchScene");
		if (!pins || !available || !show_scene) {
			expect(false, "panel editor lists were missing");
			editor->reject();
			return;
		}
		expect(show_scene->isChecked() && pins->selectionMode() == QAbstractItemView::ExtendedSelection,
		       "panel editing did not retain navigation or allow several selected controls");
		auto *first_available = tree_item(available, first);
		auto *first_group = first_available ? first_available->parent() : nullptr;
		const int automatic_count = pins->count();
		const int first_group_count = first_group ? first_group->childCount() : 0;
		expect(first_group && first_group->checkState(0) == Qt::Checked && first_group_count > 1 &&
			       key_tree(available, first_group, Qt::Key_Space) &&
			       first_group->checkState(0) == Qt::Unchecked &&
			       pins->count() == automatic_count - first_group_count &&
			       key_tree(available, first_group, Qt::Key_Space) &&
			       first_group->checkState(0) == Qt::Checked && pins->count() == automatic_count,
		       "a group checkbox did not remove and restore all of its available controls");
		expect(click(editor, "viewsPanelRemoveAll") && pins->count() == 0,
		       "Remove all did not clear the selected panel controls");
		for (auto *item : tree_controls(available))
			expect(item->checkState(0) == Qt::Unchecked,
			       "Remove all left an available-control checkbox selected");
		for (obs_sceneitem_t *item : {title, nested, spare}) {
			const QVariantMap wanted{{"kind", "source"},
						 {"sourceUuid", uuid(obs_sceneitem_get_source(item))}};
			for (auto *candidate : tree_controls(available)) {
				const auto control = candidate->data(0, Qt::UserRole + 1).toMap();
				if (matches(control.value("target").toMap(), wanted)) {
					ids.append(candidate->data(0, Qt::UserRole).toString());
					break;
				}
			}
		}
		for (auto *candidate : tree_controls(available))
			if (matches(candidate->data(0, Qt::UserRole + 1).toMap().value("target").toMap(),
				    {{"kind", "scene"}, {"sceneUuid", uuid(scene)}})) {
				ids.append(candidate->data(0, Qt::UserRole).toString());
				break;
			}
		expect(ids.size() == 9 && !ids.contains(QString()),
		       "source and OBS group controls were not available to pin");
		if (ids.size() != 9) {
			editor->reject();
			return;
		}
		for (const auto &id : ids)
			expect(check_inventory(available, id, Qt::Checked),
			       "checking an available control did not directly add it to the panel");
		expect(pins->count() == ids.size() && select_pins(pins, {1, 2}) && click(editor, "viewsPanelRemove") &&
			       pins->count() == ids.size() - 2 &&
			       tree_item(available, second)->checkState(0) == Qt::Unchecked &&
			       tree_item(available, mode)->checkState(0) == Qt::Unchecked &&
			       check_inventory(available, second, Qt::Checked) &&
			       check_inventory(available, mode, Qt::Checked) && pins->count() == ids.size(),
		       "Remove selected did not remove every selected pin and synchronize its checkbox");
		if (pins->count() != ids.size()) {
			editor->reject();
			return;
		}
		auto *search = editor->findChild<QLineEdit *>("viewsPanelSearch");
		if (search)
			search->setText("Studio title");
		expect(search && !tree_item(available, ids[5])->isHidden() &&
			       !tree_item(available, ids[5])->parent()->isHidden() &&
			       tree_item(available, first)->isHidden() && pins->count() == ids.size() &&
			       tree_item(available, first)->checkState(0) == Qt::Checked,
		       "search did not narrow the grouped inventory while preserving hidden selections");
		if (search)
			search->clear();
		expect(!tree_item(available, first)->isHidden(), "clearing search did not restore available controls");
		auto *label = editor->findChild<QLineEdit *>("viewsPanelLabel");
		auto *section = editor->findChild<QLineEdit *>("viewsPanelSection");
		auto *sections = editor->findChild<QComboBox *>("viewsPanelSectionChoice");
		expect(sections && sections->isEditable() && sections->lineEdit() == section,
		       "Section did not expose the editable preset dropdown");
		for (const auto *preset :
		     {"Scenes", "Views", "Cameras", "Captures", "Backgrounds", "Overlays", "Audio"})
			expect(sections && sections->findData(preset) >= 0, "a built-in section preset was missing");
		const auto names = saved_labels();
		for (int i = 0; label && section && i < ids.size(); ++i) {
			int from = pin_row(pins, ids[i]);
			expect(select_pins(pins, {from}), "could not select one pin for its label and position");
			while (from > i) {
				expect(click(editor, "viewsPanelUp"), "could not move a selected control upward");
				--from;
			}
			label->setText(i == 5 ? QString() : names[i]);
			expect(pin_row(pins, ids[i]) == i, "moving a pin changed its identity or saved order");
		}
		expect(label && section && select_pins(pins, {0, 1, 2}) && !label->isEnabled() && section->isEnabled(),
		       "multi-selection did not enable bulk section editing and protect individual labels");
		if (section)
			section->setText("Quick choices");
		expect(select_pins(pins, {3, 4, 5, 6, 7}), "could not select the visibility controls together");
		if (section)
			section->setText("Visibility");
		expect(select_pins(pins, {0, 3}) && click(editor, "viewsPanelResetSection") &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("section").toString().isEmpty() &&
			       pins->item(3)->data(Qt::UserRole + 1).toMap().value("section").toString().isEmpty(),
		       "Reset did not clear custom sections for a mixed selection");
		expect(select_pins(pins, {0, 1, 2}), "could not restore the first custom section");
		if (section)
			section->setText("Quick choices");
		expect(select_pins(pins, {3, 4, 5, 6, 7}), "could not restore the visibility section");
		if (section)
			section->setText("Visibility");
		expect(select_pins(pins, {8}), "could not select the direct scene pin");
		if (section)
			section->setText("Scenes");
		expect(select_pins(pins, {0, 1}) && add_section(editor, "Spare shortcuts") && sections &&
			       sections->findData("Spare shortcuts") >= 0 &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("section").toString() ==
				       "Spare shortcuts" &&
			       pins->item(1)->data(Qt::UserRole + 1).toMap().value("section").toString() ==
				       "Spare shortcuts",
		       "Add section did not retain its name and apply it to every selected control");
		expect(select_pins(pins, {3}) && select_combo_option(sections, "Spare shortcuts") &&
			       pins->item(3)->data(Qt::UserRole + 1).toMap().value("section").toString() ==
				       "Spare shortcuts",
		       "a custom section could not be reused on another control from the dropdown");
		expect(select_pins(pins, {0, 1, 3}) && select_combo_option(sections, "Cameras") &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("section").toString() == "Cameras" &&
			       pins->item(1)->data(Qt::UserRole + 1).toMap().value("section").toString() == "Cameras" &&
			       pins->item(3)->data(Qt::UserRole + 1).toMap().value("section").toString() == "Cameras",
		       "a section preset did not apply to every selected control");
		expect(select_pins(pins, {0, 1}) && select_combo_option(sections, "") &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("section").toString().isEmpty() &&
			       pins->item(1)->data(Qt::UserRole + 1).toMap().value("section").toString().isEmpty(),
		       "the original-section choice stored its placeholder instead of clearing custom sections");
		expect(select_pins(pins, {0, 1, 2}) && select_combo_option(sections, "Quick choices") &&
			       select_pins(pins, {3, 4, 5, 6, 7}) && select_combo_option(sections, "Visibility") &&
			       select_pins(pins, {8}) && select_combo_option(sections, "Scenes") &&
			       sections->findData("Spare shortcuts") >= 0,
		       "existing section names were not reusable or removing a section's last assignment lost its name");
		for (int i = 0; i < pins->count(); ++i)
			expect(pins->item(i)->data(Qt::UserRole + 1).toMap().value("section").toString() ==
				       saved_section(i),
			       "bulk section editing changed only part of the selected controls");
		QTemporaryDir icon_files;
		const QString monitor_icon = icon_files.filePath("monitor.png"),
			      camera_icon = icon_files.filePath("camera.png"),
			      invalid_icon = icon_files.filePath("invalid.png");
		expect(icon_files.isValid() && sample_icon(monitor_icon, false) && sample_icon(camera_icon, true),
		       "could not create input image files for the real icon chooser");
		expect(select_pins(pins, {0}) && choose_icon(editor, monitor_icon) &&
			       embedded_icon(pins->item(0)->data(Qt::UserRole + 1).toMap()),
		       "the image chooser did not embed an icon on the selected control");
		const auto saved_icon = pins->item(0)->data(Qt::UserRole + 1).toMap().value("iconPng");
		{
			QFile invalid(invalid_icon);
			expect(invalid.open(QIODevice::WriteOnly) && invalid.write("invalid image") > 0,
			       "could not create an invalid image fixture");
		}
		expect(choose_icon(editor, invalid_icon) &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("iconPng") == saved_icon &&
			       editor->findChild<QLabel *>("viewsPanelEditorStatus") &&
			       !editor->findChild<QLabel *>("viewsPanelEditorStatus")->text().isEmpty(),
		       "rejecting an invalid image discarded the existing embedded icon or omitted its error");
		expect(click(editor, "viewsPanelRemoveIcon") &&
			       pins->item(0)->data(Qt::UserRole + 1).toMap().value("iconPng").toString().isEmpty() &&
			       choose_icon(editor, monitor_icon) && select_pins(pins, {1}) &&
			       choose_icon(editor, camera_icon) &&
			       embedded_icon(pins->item(0)->data(Qt::UserRole + 1).toMap()) &&
			       embedded_icon(pins->item(1)->data(Qt::UserRole + 1).toMap()),
		       "removing and reapplying an icon changed the wrong pin or lost its embedded image");
		check_icon_library(editor, pins, issues);
		expect(QFile::remove(monitor_icon) && QFile::remove(camera_icon),
		       "could not remove original image files before verifying embedded-icon persistence");
		expect(label && section && click(editor, "viewsPanelSave") &&
			       until([&] { return !editor || !editor->isVisible(); }),
		       "panel customization was not saved");
		obs_source_set_name(show, "Renamed filter");
		obs_source_set_name(obs_sceneitem_get_source(watermark), "Renamed watermark");
		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "could not reopen the panel editor after a Show in Scenes source rename");
		editor = dialog("viewsPanelEditor");
		if (editor) {
			auto *renamed = tree_item(editor->findChild<QTreeWidget *>("viewsPanelAvailable"), shown);
			expect(renamed && renamed->text(0) == "Renamed watermark",
			       "the available Show in Scenes control did not reflect its renamed source");
			editor->reject();
			until([&] { return !editor || !editor->isVisible(); });
		}
		expect(until([&] {
			       auto *control = button_id(panel, shown);
			       auto *row = tree_item(control_list(panel), shown);
			       return control && row && control_id(panel, show_target) == shown &&
				      control->text() == "Watermark" && row->text(0) == "Watermark";
		       }),
		       "renaming a Show in Scenes source or filter replaced its explicit panel label");
		obs_source_set_name(show, "Watermark visibility");
		obs_source_set_name(obs_sceneitem_get_source(watermark), "Studio watermark");
		const QString title_id = ids[5], group_id = ids[6], spare_id = ids[7];
		const QString scene_id = ids[8];
		obs_source_set_name(obs_sceneitem_get_source(title), "Studio title renamed");
		expect(until([&] {
			       auto *renamed = button_id(panel, title_id);
			       return renamed && renamed->text() == "Studio title renamed" &&
				      button_id(panel, second) && button_id(panel, second)->text() == "Studio close-up";
		       }),
		       "source rename changed its identity or replaced a custom label");
		obs_source_set_enabled(link, false);
		expect(press_control(panel, group_id) && until([&] { return obs_sceneitem_visible(nested); }) &&
			       press_control(panel, group_id) && until([&] { return !obs_sceneitem_visible(nested); }),
		       "ordinary OBS group visibility could not be toggled from a pin");
		obs_source_set_enabled(link, true);
		{
			OBSSourceAutoRelease removed = obs_source_get_ref(obs_sceneitem_get_source(spare));
			obs_sceneitem_remove(spare);
			obs_source_remove(removed);
		}
		add_item("Studio spare");
		expect(until([&] {
			       auto *missing = button_id(panel, spare_id);
			       return missing && !missing->isEnabled() && missing->text() == "Spare (saved)";
		       }),
		       "missing pin was removed or rebound to a same-named replacement");
		control_list(panel)->expandAll();
		expect(until([&] { return compact_rows(panel); }) &&
			       tree_controls(control_list(panel)).size() == ids.size() && !size->isVisible(),
		       "the customized compact list used button-sized rows, lost controls or showed button-only sizing");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, scene_id) && until([&] {
			       return scene_is(scene) && checked(panel, scene_id) &&
				      tree_item(control_list(panel), scene_id)->data(0, Qt::UserRole + 2).toBool();
		       }) && press_control(panel, scene_id) &&
			       scene_is(scene),
		       "a pinned scene row did not switch directly or remain available when already active");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, scene_id) && until([&] { return scene_is(scene); }),
		       "reselecting the current scene row did not return after an external scene change");
		QPointer<QAbstractButton> retained_button = button_id(panel, first);
		auto *retained_row = tree_item(control_list(panel), first);
		expect(select_layout(panel, "buttons") && size->isVisible() && retained_button &&
			       retained_button == button_id(panel, first) &&
			       retained_row == tree_item(control_list(panel), first),
		       "switching to Buttons replaced the shared saved controls or omitted the size slider");
		for (int i = 0; i < ids.size(); ++i) {
			auto *pin_button = button_id(panel, ids[i]);
			expect(pin_button && pin_button->text() == names[i] &&
				       pin_button->property("viewsControlContext").toString() == saved_section(i) &&
				       (i >= 2 || !pin_button->icon().isNull()),
			       "Buttons did not retain the list's names, sections, targets and embedded icons");
		}
		expect(slider_key(size, Qt::Key_Home) && size->value() == 24,
		       "the live size slider did not reach its compact button size");
		check_geometry(panel, issues, "a compact control button extended outside its section");
		const QSize small_button = retained_button ? retained_button->size() : QSize();
		expect(slider_key(size, Qt::Key_End) && size->value() == 112 && until([&] {
			       return retained_button && retained_button->height() > small_button.height() &&
				      retained_button->width() > small_button.width();
		       }),
		       "increasing button size did not change both height and grid density live");
		check_geometry(panel, issues, "a large control button extended outside its section");
		expect(slider_key(size, Qt::Key_Home), "could not reset the button-size slider");
		for (int i = 0; i < 10; ++i)
			expect(slider_key(size, Qt::Key_Right), "could not adjust the button-size slider by keyboard");
		expect(size->value() == 64, "the button-size slider did not retain its chosen intermediate size");
		check_geometry(panel, issues, "an icon button extended outside its section at the normal dock width");
		auto *image_only = panel->findChild<QCheckBox *>("viewsPanelImageOnly");
		expect(image_only && !image_only->isChecked(), "existing panels did not default to labeled buttons");
		if (image_only)
			image_only->setChecked(true);
		auto square_at = [&](int pixels, int min_icon) {
			auto *key = button_id(panel, first);
			auto *no_image = button_id(panel, ids[8]);
			auto *scroll = panel->findChild<QScrollArea *>("viewsPanelScroll");
			return image_only && image_only->isChecked() && key && no_image && scroll &&
			       key->width() == pixels && key->height() == pixels &&
			       key->sizeHint() == QSize(pixels, pixels) && key->iconSize().width() >= min_icon &&
			       no_image->size() == QSize(pixels, pixels) && !no_image->toolTip().isEmpty() &&
			       no_image->toolTip().contains(no_image->text()) &&
			       !scroll->horizontalScrollBar()->isVisible();
		};
		expect(until([&] { return square_at(64, 54); }),
		       "Image only did not produce square tiles with large images and usable tooltips");
		auto *title_choice = panel->findChild<QComboBox *>("viewsPanelTitle");
		auto *title_size = panel->findChild<QComboBox *>("viewsPanelTitleSize");
		expect(title_choice && title_size && title_choice->isVisible() &&
			       title_choice->currentData().toString() == "bottom" &&
			       title_size->currentData().toString() == "medium",
		       "Image only did not offer its title, at the bottom and medium by default");
		expect(select_combo_option(title_choice, "top") && select_combo_option(title_size, "large") &&
			       until([&] { return title_choice->currentData().toString() == "top"; }),
		       "the image-only title position and size could not be chosen");
		expect(select_combo_option(title_choice, "bottom") && select_combo_option(title_size, "medium"),
		       "the image-only title could not return to its default");
		expect(slider_key(size, Qt::Key_Home) && size->value() == 24 &&
			       until([&] { return square_at(24, 20); }),
		       "compact image-only buttons did not reach list-row scale with visible artwork");
		check_geometry(panel, issues, "a compact square control extended outside its section");
		expect(press_control(panel, second) &&
			       until([&] { return current(id, "camera") == "detail" && checked(panel, second); }),
		       "an image-only button did not dispatch its native controller action");
		expect(slider_key(size, Qt::Key_End) && until([&] { return square_at(112, 96); }),
		       "a large image-only button did not scale its artwork with the tile");
		check_geometry(panel, issues, "a large square control extended outside its section");
		expect(slider_key(size, Qt::Key_Home), "could not return to compact image-only size");
		for (int i = 0; i < 10; ++i)
			expect(slider_key(size, Qt::Key_Right), "could not restore image-only button size");
		expect(size->value() == 64 && select_layout(panel, "list") && select_layout(panel, "buttons") &&
			       until([&] { return square_at(64, 54); }),
		       "switching presentations lost image-only mode or square image sizing");
		if (image_only)
			image_only->setChecked(false);
		expect(until([&] { return image_only && !image_only->isChecked(); }) &&
			       check_saved_panel(panel, "buttons"),
		       "turning Image only off did not restore existing labeled-button settings");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, second) && until([&] { return scene_is(scene); }),
		       "an already-active mode button did not show its scene in Buttons layout");
		obs_frontend_set_current_scene(first_scene);
		expect(press_control(panel, scene_id) && until([&] { return scene_is(scene); }),
		       "a pinned scene button did not switch directly to its scene");
		obs_frontend_set_current_scene(first_scene);
		obs_frontend_set_preview_program_mode(true);
		obs_frontend_set_current_preview_scene(destination_scene);
		expect(press_control(panel, scene_id) &&
			       until([&] { return scene_is(scene, true) && scene_is(first_scene); }),
		       "a pinned scene button changed Program instead of Preview in Studio Mode");
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(first_scene);
		expect(select_layout(panel, "list") && retained_button == button_id(panel, first) &&
			       retained_row == tree_item(control_list(panel), first) && check_saved_panel(panel),
		       "returning to List changed the saved pins, sections, icons, size or navigation preference");

		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "could not open a draft to verify Cancel");
		editor = dialog("viewsPanelEditor");
		if (editor) {
			pins = editor->findChild<QListWidget *>("viewsPanelPins");
			label = editor->findChild<QLineEdit *>("viewsPanelLabel");
			section = editor->findChild<QLineEdit *>("viewsPanelSection");
			sections = editor->findChild<QComboBox *>("viewsPanelSectionChoice");
			show_scene = editor->findChild<QCheckBox *>("viewsPanelSwitchScene");
			expect(select_pins(pins, {0}) && label && section && show_scene,
			       "a saved pin could not be edited in a new draft");
			if (label)
				label->setText("Discarded label");
			expect(add_section(editor, "Discarded section") && sections &&
				       sections->findData("Discarded section") >= 0,
			       "a draft custom section was not available before Cancel");
			if (show_scene)
				show_scene->setChecked(false);
			expect(click(editor, "viewsPanelRemoveIcon") && click(editor, "viewsPanelRemoveAll") && pins &&
				       pins->count() == 0 && sections && sections->findData("Spare shortcuts") >= 0 &&
				       click(editor, "viewsPanelAutomatic") && pins->count() > 0 &&
				       sections->findData("Spare shortcuts") >= 0 &&
				       sections->findData("Discarded section") >= 0,
			       "draft icon removal, clear or Automatic reset did not update its controls");
			editor->reject();
			expect(until([&] { return !editor || !editor->isVisible(); }) && check_saved_panel(panel),
			       "Cancel changed saved controls, custom labels, sections, icons or navigation settings");
		}

		// One bad supported filter rejects the whole batch. Removing it then
		// imports all three valid filters, including the disabled original.
		OBSDataAutoRelease invalid_settings = obs_data_create();
		obs_data_set_int(invalid_settings, "mode", 33);
		list(invalid_settings, "items", {"Studio overlay note"});
		OBSSourceAutoRelease invalid = obs_source_create("stagekit_visibility_link", "Invalid import fixture",
								 invalid_settings, nullptr);
		obs_source_filter_add(scene, invalid);
		const auto before_modes = resolved(legacy), before_link = resolved(link),
			   before_disabled = resolved(disabled);
		expect(click(panel, "viewsManageControllers"), "could not open management for bulk import");
		expect(until([&] { return scenes->findData(uuid(scene)) >= 0; }), "import scene was not listed");
		scenes->setCurrentIndex(scenes->findData(uuid(scene)));
		expect(until([&] { return controllers->findData(QString::fromStdString(id)) >= 0; }),
		       "import controller was not listed");
		controllers->setCurrentIndex(controllers->findData(QString::fromStdString(id)));
		expect(click(panel, "viewsImportLegacy") && until([&] { return dialog("viewsImportDialog"); }),
		       "existing scene controls import did not open");
		QPointer<QDialog> importing = dialog("viewsImportDialog");
		if (importing) {
			auto *ownership = importing->findChild<QCheckBox *>("viewsImportActivate");
			auto *result = importing->findChild<QLabel *>("viewsImportError");
			auto group_count = [&] {
				OBSDataAutoRelease state = get(id);
				OBSDataAutoRelease definition = obs_data_get_obj(state, "config");
				OBSDataArrayAutoRelease list = obs_data_get_array(definition, "groups");
				return obs_data_array_count(list);
			};
			expect(ownership && ownership->isChecked() && click(importing, "viewsImportAll") && result &&
				       !result->text().isEmpty() && group_count() == 1 && !imported_group(id, legacy) &&
				       !imported_group(id, link) && resolved(legacy) == before_modes &&
				       resolved(link) == before_link && resolved(disabled) == before_disabled &&
				       obs_source_enabled(legacy) && obs_source_enabled(link) &&
				       !obs_source_enabled(disabled),
			       "invalid Import all batch changed settings, ownership or part of the controller");
			obs_source_filter_remove(scene, invalid);
			const size_t filter_count = obs_source_filter_count(scene);
			expect(click(importing, "viewsImportAll") && until([&] { return group_count() == 4; }) &&
				       result &&
				       result->text() == "Imported 3 controls. Skipped 0 already imported controls.",
			       "Import all did not import all supported controls with clear counts");
			for (const auto &[source, saved] :
			     std::vector<std::pair<obs_source_t *, QJsonObject>>{{legacy, before_modes},
										 {link, before_link},
										 {disabled, before_disabled}}) {
				OBSDataAutoRelease group = imported_group(id, source);
				OBSDataAutoRelease origin = obs_data_get_obj(group, "origin");
				OBSDataAutoRelease original_settings = obs_data_get_obj(origin, "settings");
				expect(group && json(original_settings) == saved &&
					       obs_data_get_bool(origin, "activate") &&
					       obs_data_get_bool(origin, "enabled") == (source != disabled.Get()) &&
					       obs_data_get_bool(group, "enabled") == (source != disabled.Get()) &&
					       obs_source_enabled(source) == (source != disabled.Get()),
				       "bulk import lost complete original settings or enabled state");
			}
			expect(obs_source_filter_count(scene) == filter_count,
			       "bulk import added hidden filters or removed originals");
			OBSDataAutoRelease mode_group = imported_group(id, legacy);
			OBSDataAutoRelease link_group = imported_group(id, link);
			const std::string imported_mode_id = obs_data_get_string(mode_group, "id");
			const std::string imported_link_id = obs_data_get_string(link_group, "id");
			expect(click(importing, "viewsImportAll") && group_count() == 4 && result &&
				       result->text() == "Imported 0 controls. Skipped 3 already imported controls.",
			       "repeated Import all duplicated controls or did not report skipped originals");
			OBSDataAutoRelease repeated_mode = imported_group(id, legacy);
			OBSDataAutoRelease repeated_link = imported_group(id, link);
			expect(imported_mode_id == obs_data_get_string(repeated_mode, "id") &&
				       imported_link_id == obs_data_get_string(repeated_link, "id"),
			       "repeated Import all changed imported identities");
			importing->reject();
			until([&] { return !importing || !importing->isVisible(); });
			expect(legacy_press(studio, legacy, "front") && until([&] {
				       return current(id, imported_mode_id.c_str()) == "front" &&
					      obs_sceneitem_visible(legacy_main) &&
					      !obs_sceneitem_visible(legacy_detail);
			       }),
			       "existing Scene Mode key did not remain an alias after Import all");
			expect(click(panel, "viewsBackToControls") && until([&] {
				       return button_id(panel, mode) && button_id(panel, mode)->isEnabled() &&
					      button_id(panel, linked) && button_id(panel, linked)->isEnabled();
			       }) && press_control(panel, mode) &&
				       until([&] {
					       return current(id, imported_mode_id.c_str()) == "detail" &&
						      obs_sceneitem_visible(legacy_detail) &&
						      !obs_sceneitem_visible(legacy_main);
				       }) &&
				       press_control(panel, linked) && until([&] {
					       return obs_sceneitem_visible(nested) &&
						      !obs_sceneitem_visible(wide_camera);
				       }),
			       "pinned legacy controls stopped working after controller ownership transferred");
		}

		check_other_scene_scope(panel, id, scene, issues);
		OBSSourceAutoRelease before_browsing = obs_frontend_get_current_scene();
		// Restore the previous management selection before saving for its own
		// restart fixture; operator controls remain the default page.
		expect(click(panel, "viewsManageControllers"), "could not restore controller management selection");
		scenes->setCurrentIndex(scenes->findData(old_scene));
		expect(until([&] { return controllers->findData(old_controller) >= 0; }),
		       "previous controller was not available after bulk import");
		controllers->setCurrentIndex(controllers->findData(old_controller));
		expect(until([&] { return scope->findData(old_scope) >= 0; }),
		       "previous controller scope was not restored");
		scope->setCurrentIndex(scope->findData(old_scope));
		expect(click(panel, "viewsBackToControls") && check_saved_panel(panel),
		       "custom labels, sections or saved order changed while importing controls");
		expect(scene_is(before_browsing), "management scene, controller or group browsing changed Program");
		obs_frontend_save();

		const QString capture = qEnvironmentVariable("STAGEKIT_VIEWS_CAPTURE");
		if (!capture.isEmpty()) {
			const QDir directory = QFileInfo(capture).isDir() ? QDir(capture)
									  : QFileInfo(capture).absoluteDir();
			QDockWidget *dock = nullptr;
			for (auto *parent = panel->parentWidget(); !dock && parent; parent = parent->parentWidget())
				dock = qobject_cast<QDockWidget *>(parent);
			if (dock)
				dock->resize(520, 600);
			control_list(panel)->expandAll();
			control_list(panel)->verticalScrollBar()->setValue(0);
			expect(until([&] {
				       return compact_rows(panel) &&
					      control_list(panel)->verticalScrollBar()->maximum() == 0;
			       }),
			       "the compact list did not fit the selected controls at the guide capture size");
			close_panel_settings(panel);
			expect(panel->grab().save(directory.filePath("stagekit-controls-list.png")),
			       "could not capture the real compact control list");
			expect(select_layout(panel, "buttons") && slider_key(size, Qt::Key_Home) && size->value() == 24,
			       "could not show compact icon buttons for the guide capture");
			check_geometry(panel, issues,
				       "a control button was clipped by its section after resizing the dock");
			if (auto *scroll = panel->findChild<QScrollArea *>("viewsPanelScroll")) {
				scroll->verticalScrollBar()->setValue(0);
				expect(until([&] { return scroll->verticalScrollBar()->maximum() == 0; }),
				       "compact icon buttons did not fit all selected controls in the guide capture");
			}
			QCoreApplication::processEvents();
			close_panel_settings(panel);
			expect(panel->grab().save(directory.filePath("stagekit-controls.png")),
			       "could not capture the real icon button panel");
			if (auto *only = panel->findChild<QCheckBox *>("viewsPanelImageOnly")) {
				only->setChecked(true);
				for (int i = 0; i < 10; ++i)
					expect(slider_key(size, Qt::Key_Right),
					       "could not enlarge image-only tiles for the guide capture");
				expect(size->value() == 64, "the image-only guide capture was not at 64 pixels");
				QCoreApplication::processEvents();
				close_panel_settings(panel);
				expect(panel->grab().save(directory.filePath("stagekit-controls-image-only.png")),
				       "could not capture the real image-only panel");
				open_panel_settings(panel);
				expect(panel->grab().save(directory.filePath("stagekit-controls-settings.png")),
				       "could not capture the real panel settings");
				only->setChecked(false);
				expect(slider_key(size, Qt::Key_Home),
				       "could not restore the button size after the image-only capture");
			}
			expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
			       "could not open the panel editor for capture");
			editor = dialog("viewsPanelEditor");
			if (editor) {
				auto *search = editor->findChild<QLineEdit *>("viewsPanelSearch");
				if (search)
					search->setText("Studio");
				select_pins(editor->findChild<QListWidget *>("viewsPanelPins"), {1});
				QCoreApplication::processEvents();
				expect(editor->grab().save(directory.filePath("stagekit-controls-editor.png")),
				       "could not capture the real panel editor");
				expect(click(editor, "viewsPanelIconLibrary") &&
					       until([&] { return dialog("viewsPanelIconLibraryDialog"); }),
				       "could not open the integrated icon library for capture");
				QPointer<QDialog> library = dialog("viewsPanelIconLibraryDialog");
				if (library) {
					auto *palette = library->findChild<QComboBox *>("viewsPanelIconLibraryPalette");
					expect(select_combo_option(palette, "ocean"),
					       "could not show a colour palette for the real icon library capture");
					QCoreApplication::processEvents();
					expect(library->grab().save(
						       directory.filePath("stagekit-controls-icon-library.png")),
					       "could not capture the real integrated icon library");
					library->reject();
					until([&] { return !library || !library->isVisible(); });
				}
				editor->reject();
				until([&] { return !editor || !editor->isVisible(); });
			}
			for (int i = 0; i < 10; ++i)
				expect(slider_key(size, Qt::Key_Right),
				       "could not restore the saved size after capture");
			expect(size->value() == 64, "the capture changed the saved intermediate button size");
			expect(select_layout(panel, "list"), "could not restore List after the guide captures");
		}
		// Persist nondefault values so the collection and restart assertions
		// cannot pass merely by falling back to the defaults.
		expect(select_layout(panel, "list"), "could not show the list before saving a collapsed section");
		auto *heading = section_heading(panel, "Visibility");
		if (heading) {
			control_list(panel)->expandItem(heading);
			control_list(panel)->collapseItem(heading);
		}
		expect(heading && !heading->isExpanded(), "the native list section did not collapse");
		expect(select_layout(panel, "buttons") && set_show_scene(panel, false) &&
			       check_saved_panel(panel, "buttons", false, true),
		       "could not persist nondefault presentation and navigation settings for restart");
		obs_frontend_save();
	}
	check_collection_isolation(panel, issues);
	// Persist the nondefault image-only presentation for the real OBS
	// restart probe. The ordinary labeled captures above retain their look.
	auto *image_only = panel->findChild<QCheckBox *>("viewsPanelImageOnly");
	expect(select_layout(panel, "buttons") && image_only,
	       "the image-only setting was not available after switching collections");
	if (image_only)
		image_only->setChecked(true);
	expect(check_saved_panel(panel, "buttons", false, true, true),
	       "could not preserve the image-only setting for the restart test");
	obs_frontend_save();
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[StageKit controls test] PASS: compact list and icon buttons, native mode/view navigation, direct scene selection, Studio Preview, section presets and reusable custom sections, source-based visibility names, grouped bulk editing, custom and built-in icon library images, palettes and size, native visibility, rename/missing identity, bulk import and scene collection isolation");
}

} // namespace control_panel_test
