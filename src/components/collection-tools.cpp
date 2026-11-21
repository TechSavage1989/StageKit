#include "collection-tools.hpp"
#include "collection-convert.hpp"

#include <obs-module.h>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace stagekit {
namespace {
using Json = nlohmann::json;
QString tr(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

struct Catalog {
	std::filesystem::path resources;
	Json definitions;
	Catalog()
	{
		resources = std::filesystem::u8path(obs_get_module_data_path(obs_current_module()));
		const auto filename = (resources / "ports.json").u8string();
		QFile file(QString::fromUtf8(reinterpret_cast<const char *>(filename.c_str())));
		if (!file.open(QIODevice::ReadOnly))
			throw std::runtime_error("ports.json");
		const QByteArray bytes = file.readAll();
		definitions = Json::parse(bytes.begin(), bytes.end());
	}
};

void convert_copy(QWidget *parent, const Catalog &catalog)
{
	const QString input = QFileDialog::getOpenFileName(parent, tr("Ports.ChooseCollection"), {}, "OBS (*.json)");
	if (input.isEmpty())
		return;
	const QFileInfo info(input);
	const QString output = QFileDialog::getSaveFileName(parent, tr("Ports.SaveCopy"),
		info.dir().filePath(info.completeBaseName() + "-StageKit.json"), "OBS (*.json)");
	if (output.isEmpty())
		return;
	if (QFile::exists(output)) {
		QMessageBox::information(parent, tr("Ports.Manager"), tr("Ports.NewFile"));
		return;
	}
	try {
		QFile file(input);
		if (!file.open(QIODevice::ReadOnly))
			throw std::runtime_error(file.errorString().toStdString());
		const auto bytes = file.readAll();
		auto collection = Json::parse(bytes.begin(), bytes.end());
		const auto report = convert_collection(collection, catalog.definitions, catalog.resources);
		collection["name"] = QFileInfo(output).completeBaseName().toUtf8().toStdString();
		const std::string result = collection.dump(2) + "\n";
		QTemporaryFile tmp(QFileInfo(output).dir().filePath(".stagekit-copy-XXXXXX"));
		if (!tmp.open() || tmp.write(result.data(), (qint64)result.size()) != (qint64)result.size() || !tmp.flush())
			throw std::runtime_error(tmp.errorString().toStdString());
		tmp.close();
		// QFile refuses an existing destination, including a file created after the chooser closed.
		if (!QFile::rename(tmp.fileName(), output))
			throw std::runtime_error(tr("Ports.NewFile").toStdString());
		QString summary = tr("Ports.Converted").arg((qulonglong)report.converted).arg((qulonglong)report.relocated);
		if (!report.review.empty()) {
			summary += "\n\n" + tr("Ports.Review");
			for (const auto &[id, count] : report.review)
				summary += "\n" + QString::fromStdString(id) + " (" + QString::number((qulonglong)count) + ")";
		}
		QMessageBox::information(parent, tr("Ports.Manager"), summary + "\n\n" + tr("Ports.Import"));
	} catch (const std::exception &e) {
		QMessageBox::warning(parent, tr("Ports.Manager"), tr("Ports.CopyError") + "\n" + QString::fromUtf8(e.what()));
	}
}
} // namespace

void collection_tools_menu(QMenu *menu)
{
	QObject::connect(menu->addAction(tr("Ports.Manager")), &QAction::triggered, menu, [menu] {
		try {
			Catalog catalog;
			QDialog dialog(menu);
			dialog.setWindowTitle(tr("Ports.Manager"));
			auto *layout = new QVBoxLayout(&dialog);
			auto *search = new QLineEdit(&dialog);
			search->setPlaceholderText(tr("Ports.Find"));
			layout->addWidget(search);
			auto *table = new QTableWidget(0, 2, &dialog);
			table->setHorizontalHeaderLabels({tr("Ports.Original"), tr("Ports.StageKitControl")});
			table->setEditTriggers(QAbstractItemView::NoEditTriggers);
			table->verticalHeader()->hide();
			table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
			table->horizontalHeader()->setStretchLastSection(true);
			layout->addWidget(table);
			std::set<std::pair<QString, QString>> rows;
			for (const auto &[port, spec] : catalog.definitions.items()) {
				if (spec.contains("types"))
					for (const auto &type : spec.at("types")) {
						if (type.size() >= 4)
							rows.emplace(QString::fromStdString(type.at(3).get<std::string>()),
								     tr(type.at(2).get<std::string>().c_str()));
					}
				if (spec.contains("tool") && spec.contains("originalTool"))
					rows.emplace(QString::fromStdString(spec.at("originalTool").get<std::string>()),
						     tr(spec.at("tool").get<std::string>().c_str()));
				if (spec.contains("controls"))
					for (const auto &item : spec.at("controls"))
						rows.emplace(QString::fromStdString(item.at(0).get<std::string>()), tr(item.at(1).get<std::string>().c_str()));
			}
			for (const auto &[old, next] : rows) {
				const int row = table->rowCount();
				table->insertRow(row);
				table->setItem(row, 0, new QTableWidgetItem(old));
				table->setItem(row, 1, new QTableWidgetItem(next));
			}
			QObject::connect(search, &QLineEdit::textChanged, table, [table](const QString &s) {
				for (int r = 0; r < table->rowCount(); ++r)
					table->setRowHidden(r, !table->item(r, 0)->text().contains(s, Qt::CaseInsensitive) &&
						!table->item(r, 1)->text().contains(s, Qt::CaseInsensitive));
			});
			auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
			auto *convert = buttons->addButton(tr("Ports.ConvertCopy"), QDialogButtonBox::ActionRole);
			QObject::connect(convert, &QPushButton::clicked, &dialog, [&] { convert_copy(&dialog, catalog); });
			QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
			layout->addWidget(buttons);
			dialog.resize(760, 530);
			dialog.exec();
		} catch (const std::exception &e) {
			QMessageBox::warning(menu, tr("Ports.Manager"), QString::fromUtf8(e.what()));
		}
	});
}
} // namespace stagekit
