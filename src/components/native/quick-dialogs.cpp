#include "quick-dialogs.hpp"
#include "component.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace stagekit::native::quick {
QString mode_label(const QString &mode)
{
	if (mode == "Manual")
		return text("Quick.Manual");
	if (mode == "Dynamic")
		return text("Quick.Dynamic");
	return text("Quick.SourceSearch");
}

void dock_options(QWidget *parent, const Options &initial, bool creating, std::function<void(Options)> apply)
{
	auto *dialog = new QDialog(parent);
	dialog->setObjectName("nativeQuickOptions");
	dialog->setWindowTitle(text(creating ? "Quick.NewDock" : "Quick.DockOptions"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto *layout = new QFormLayout(dialog);
	auto *title = new QLineEdit(initial.title, dialog);
	title->setObjectName("nativeQuickTitle");
	layout->addRow(text("Native.DockName"), title);
	auto *mode = new QComboBox(dialog);
	mode->setObjectName("nativeQuickType");
	for (const char *option : {"Manual", "Dynamic", "Source Search"})
		mode->addItem(mode_label(option), option);
	mode->setCurrentIndex(mode->findData(initial.mode));
	mode->setEnabled(creating);
	layout->addRow(text("Quick.DockType"), mode);
	auto check = [&](const char *name, const char *label, bool value) {
		auto *box = new QCheckBox(text(label), dialog);
		box->setObjectName(name);
		box->setChecked(value);
		layout->addRow(box);
		return box;
	};
	auto *props = check("quickShowProperties", "Quick.ShowProperties", initial.properties);
	auto *filters = check("quickShowFilters", "Quick.ShowFilters", initial.filters);
	auto *parents = check("quickShowParents", "Quick.ShowParents", initial.parents);
	auto *clickable = check("quickClickable", "Quick.Clickable", initial.clickable);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
	auto *ok = buttons->button(QDialogButtonBox::Ok);
	ok->setEnabled(!title->text().trimmed().isEmpty());
	QObject::connect(title, &QLineEdit::textChanged, dialog,
			 [ok](const QString &name) { ok->setEnabled(!name.trimmed().isEmpty()); });
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(dialog, &QDialog::accepted, dialog, [=] {
		apply({title->text().trimmed(), mode->currentData().toString(), props->isChecked(),
		       filters->isChecked(), parents->isChecked(), clickable->isChecked()});
	});
	layout->addRow(buttons);
	dialog->resize(360, dialog->sizeHint().height());
	dialog->open();
	title->setFocus();
}

void pick_sources(QWidget *parent, const QList<Source> &pinned, std::function<void(QList<Source>)> add)
{
	auto *dialog = new QDialog(parent);
	dialog->setObjectName("nativeQuickPicker");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(text("Quick.AddSources"));
	auto *layout = new QVBoxLayout(dialog);
	auto *search = new QLineEdit(dialog);
	search->setPlaceholderText(text("Quick.Search"));
	search->setClearButtonEnabled(true);
	layout->addWidget(search);
	auto *columns = new QHBoxLayout;
	layout->addLayout(columns);
	auto pane = [&](const char *label, const char *name) {
		auto *column = new QVBoxLayout;
		columns->addLayout(column, 1);
		column->addWidget(new QLabel(text(label), dialog));
		auto *list = new QListWidget(dialog);
		list->setObjectName(name);
		list->setSelectionMode(QAbstractItemView::ExtendedSelection);
		column->addWidget(list);
		return list;
	};
	auto *available = pane("Quick.Available", "nativeQuickAvailable");
	auto *moves = new QVBoxLayout;
	moves->addStretch();
	auto move_button = [&](const char *label, Qt::ArrowType arrow) {
		auto *button = new QToolButton(dialog);
		button->setArrowType(arrow);
		button->setAccessibleName(text(label));
		button->setToolTip(text(label));
		moves->addWidget(button);
		return button;
	};
	auto *include = move_button("Quick.Include", Qt::RightArrow);
	include->setObjectName("nativeQuickInclude");
	auto *exclude = move_button("Quick.Exclude", Qt::LeftArrow);
	moves->addStretch();
	columns->addLayout(moves);
	auto *chosen = pane("Quick.ToAdd", "nativeQuickChosen");
	QSet<QString> existing;
	for (const auto &pin : pinned)
		existing.insert(pin.uuid);
	for (const auto &source : available_sources()) {
		if (existing.contains(source.uuid))
			continue;
		auto *item = new QListWidgetItem(source_icon(resolve(source)), source.name, available);
		item->setData(Qt::UserRole, source.uuid);
	}
	auto filter = [available, search] {
		for (int i = 0; i < available->count(); ++i)
			available->item(i)->setHidden(
				!available->item(i)->text().contains(search->text(), Qt::CaseInsensitive));
	};
	auto transfer = [filter](QListWidget *from, QListWidget *to) {
		auto selected = from->selectedItems();
		std::sort(selected.begin(), selected.end(),
			  [from](auto *a, auto *b) { return from->row(a) < from->row(b); });
		for (auto *item : selected)
			if (!item->isHidden()) {
				auto *moved = from->takeItem(from->row(item));
				to->addItem(moved);
				moved->setSelected(false);
			}
		filter();
	};
	available->setSortingEnabled(true);
	QObject::connect(search, &QLineEdit::textChanged, dialog, filter);
	QObject::connect(include, &QToolButton::clicked, dialog, [=] { transfer(available, chosen); });
	QObject::connect(exclude, &QToolButton::clicked, dialog, [=] { transfer(chosen, available); });
	QObject::connect(available, &QListWidget::itemDoubleClicked, dialog, [=] { transfer(available, chosen); });
	QObject::connect(chosen, &QListWidget::itemDoubleClicked, dialog, [=] { transfer(chosen, available); });
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
	auto *commit = buttons->addButton(text("Quick.AddSources"), QDialogButtonBox::AcceptRole);
	commit->setObjectName("nativeQuickAddSources");
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(dialog, &QDialog::accepted, dialog, [=] {
		QList<Source> selection;
		for (int i = 0; i < chosen->count(); ++i) {
			auto *item = chosen->item(i);
			selection.append({item->text(), item->data(Qt::UserRole).toString()});
		}
		add(selection);
	});
	layout->addWidget(buttons);
	dialog->resize(570, 370);
	dialog->open();
	search->setFocus();
}
} // namespace stagekit::native::quick
