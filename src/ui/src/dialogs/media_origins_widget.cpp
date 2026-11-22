#include "media_origins_widget.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace maskpilot::ui {
struct MediaOriginsWidget::Impl {
    bool chinese;
    std::function<std::string()> source;
    std::vector<MediaOriginAccess> rules;
    QTableWidget* table;
    QCheckBox* consent;
    QString text(const char* en, const char* zh) const { return QString::fromUtf8(chinese ? zh : en); }
    void refresh() {
        consent->setEnabled(!rules.empty());
        table->setRowCount(static_cast<int>(rules.size()));
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto& rule = rules[static_cast<std::size_t>(row)];
            table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(rule.origin)));
            table->setItem(row, 1,
                           new QTableWidgetItem(mediaOriginRequiresLogin(rule)
                                                    ? text("Login required", "需要登录")
                                                    : text("Anonymous", "匿名访问")));
        }
    }
};
MediaOriginsWidget::MediaOriginsWidget(bool chinese, std::function<std::string()> source, QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {
    impl_->chinese = chinese;
    impl_->source = std::move(source);
    setObjectName("mediaOrigins");
    const auto t = [this](const char* en, const char* zh) { return impl_->text(en, zh); };
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(t("Additional media origins", "额外媒体来源")));
    auto* note =
        new QLabel(t("Add only the exact origins needed by this media. Each uses its own login; anonymous "
                     "entries receive no source credentials.",
                     "仅添加此媒体需要的确切来源。各来源使用独立登录材料；匿名来源不会收到网页的登录信息。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    impl_->table = new QTableWidget(0, 2);
    impl_->table->setObjectName("mediaOriginsTable");
    impl_->table->setHorizontalHeaderLabels(
        {t("Origin (scheme, host, port)", "来源（协议、主机、端口）"), t("Access", "访问方式")});
    impl_->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    impl_->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    impl_->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    impl_->table->setSelectionMode(QAbstractItemView::SingleSelection);
    impl_->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    impl_->table->verticalHeader()->hide();
    impl_->table->setMinimumHeight(110);
    impl_->table->setMaximumHeight(170);
    layout->addWidget(impl_->table);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton(t("Add origin…", "添加来源…"));
    auto* edit = new QPushButton(t("Edit…", "编辑…"));
    auto* remove = new QPushButton(t("Remove", "移除"));
    add->setObjectName("addMediaOrigin");
    edit->setObjectName("editMediaOrigin");
    remove->setObjectName("removeMediaOrigin");
    edit->setEnabled(false);
    remove->setEnabled(false);
    connect(impl_->table, &QTableWidget::itemSelectionChanged, this, [this, edit, remove] {
        const bool selected = impl_->table->currentRow() >= 0;
        edit->setEnabled(selected);
        remove->setEnabled(selected);
    });
    actions->addWidget(add);
    actions->addWidget(edit);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addLayout(actions);
    impl_->consent =
        new QCheckBox(t("I approve access to these additional origins", "我确认允许访问上述额外来源"));
    impl_->consent->setObjectName("mediaOriginsConfirmed");
    impl_->consent->setEnabled(false);
    layout->addWidget(impl_->consent);
    connect(impl_->consent, &QCheckBox::toggled, this, &MediaOriginsWidget::changed);
    const auto mutate = [this] {
        impl_->consent->setChecked(false);
        impl_->refresh();
        Q_EMIT changed();
    };
    connect(remove, &QPushButton::clicked, this, [this, mutate] {
        const auto row = impl_->table->currentRow();
        if (row >= 0) {
            impl_->rules.erase(impl_->rules.begin() + row);
            mutate();
        }
    });
    const auto openEditor = [this, t, mutate](bool creating) {
        const auto row = impl_->table->currentRow();
        if ((!creating && row < 0) || (creating && impl_->rules.size() >= 16))
            return;
        MediaOriginAccess rule = creating ? MediaOriginAccess{} : impl_->rules[static_cast<std::size_t>(row)];
        QDialog dialog(this);
        dialog.setWindowTitle(t("Media origin access", "媒体来源访问"));
        dialog.setObjectName("mediaOriginEditor");
        dialog.resize(550, 420);
        auto* form = new QFormLayout(&dialog);
        auto* origin = new QLineEdit(QString::fromStdString(rule.origin));
        origin->setObjectName("mediaOriginUrl");
        origin->setPlaceholderText("https://cdn.example.com");
        form->addRow(t("Origin", "来源"), origin);
        const auto secret = [&](const char* name, const QString& label, const std::string& value) {
            auto* field = new QLineEdit;
            field->setMaxLength(128 * 1024);
            field->setText(QString::fromUtf8(value));
            field->setObjectName(name);
            field->setEchoMode(QLineEdit::Password);
            form->addRow(label, field);
            return field;
        };
        auto* cookie = secret("mediaOriginCookie", "Cookie", rule.cookie);
        auto* authorization = secret("mediaOriginAuthorization", "Authorization", rule.authorization);
        auto* required = new QCheckBox(t("Require login when resuming", "继续任务时需要登录材料"));
        required->setObjectName("mediaOriginAuthenticationRequired");
        required->setChecked(mediaOriginRequiresLogin(rule));
        form->addRow(required);
        auto* headers = new QTableWidget(0, 3);
        headers->setObjectName("mediaOriginHeaders");
        headers->setHorizontalHeaderLabels({t("Header", "请求头"), t("Value", "值"), {}});
        headers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        headers->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        headers->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
        headers->setColumnWidth(2, 42);
        headers->verticalHeader()->hide();
        headers->verticalHeader()->setDefaultSectionSize(qMax(38, fontMetrics().height() + 18));
        headers->setSelectionBehavior(QAbstractItemView::SelectRows);
        headers->setMinimumHeight(100);
        const auto append = [headers, t](const QString& name, const QString& value) {
            const auto index = headers->rowCount();
            headers->insertRow(index);
            headers->setCellWidget(index, 0, new QLineEdit(name));
            auto* field = new QLineEdit;
            field->setMaxLength(128 * 1024);
            field->setText(value);
            field->setEchoMode(QLineEdit::Password);
            headers->setCellWidget(index, 1, field);
            auto* remove = new QPushButton(QString::fromUtf8("×"));
            remove->setAutoDefault(false);
            remove->setObjectName("removeMediaOriginHeader");
            remove->setStyleSheet("QPushButton { padding: 0; min-width: 0; }");
            remove->setToolTip(t("Remove header", "移除请求头"));
            remove->setAccessibleName(remove->toolTip());
            headers->setCellWidget(index, 2, remove);
            connect(remove, &QPushButton::clicked, headers, [headers, remove] {
                for (int row = 0; row < headers->rowCount(); ++row)
                    if (headers->cellWidget(row, 2) == remove) {
                        headers->removeRow(row);
                        break;
                    }
            });
        };
        for (const auto& [name, value] : rule.headers)
            append(QString::fromUtf8(name), QString::fromUtf8(value));
        const auto updateRequirement = [cookie, authorization, headers, required] {
            const bool material =
                !cookie->text().isEmpty() || !authorization->text().isEmpty() || headers->rowCount() > 0;
            if (material)
                required->setChecked(true);
            required->setEnabled(!material);
        };
        connect(cookie, &QLineEdit::textChanged, &dialog, updateRequirement);
        connect(authorization, &QLineEdit::textChanged, &dialog, updateRequirement);
        connect(headers->model(), &QAbstractItemModel::rowsInserted, &dialog, updateRequirement);
        connect(headers->model(), &QAbstractItemModel::rowsRemoved, &dialog, updateRequirement);
        updateRequirement();
        form->addRow(t("Custom headers", "自定义请求头"), headers);
        auto* headerActions = new QHBoxLayout;
        auto* addHeader = new QPushButton(t("Add header", "添加请求头"));
        addHeader->setObjectName("addMediaOriginHeader");
        addHeader->setAutoDefault(false);
        headerActions->addWidget(addHeader);
        headerActions->addStretch();
        form->addRow(headerActions);
        connect(addHeader, &QPushButton::clicked, &dialog, [headers, append] {
            if (headers->rowCount() < 64)
                append({}, {});
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(t("OK", "确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(t("Cancel", "取消"));
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            rule.origin = origin->text().trimmed().toStdString();
            rule.cookie = cookie->text().toStdString();
            rule.authorization = authorization->text().toStdString();
            rule.authenticationRequired = required->isChecked();
            rule.headers.clear();
            for (int index = 0; index < headers->rowCount(); ++index)
                rule.headers.emplace_back(
                    qobject_cast<QLineEdit*>(headers->cellWidget(index, 0))->text().trimmed().toStdString(),
                    qobject_cast<QLineEdit*>(headers->cellWidget(index, 1))->text().toStdString());
            DownloadSpec candidate;
            candidate.kind = TransferKind::Media;
            candidate.url = impl_->source();
            candidate.mediaOrigins = impl_->rules;
            if (creating)
                candidate.mediaOrigins.push_back(rule);
            else
                candidate.mediaOrigins[static_cast<std::size_t>(row)] = rule;
            if (const auto error = validateMediaOrigins(candidate); !error.empty()) {
                QMessageBox::warning(&dialog, dialog.windowTitle(), QString::fromStdString(error));
                return;
            }
            dialog.accept();
        });
        if (dialog.exec() == QDialog::Accepted) {
            rule.origin = mediaOrigin(rule.origin, true);
            if (creating)
                impl_->rules.push_back(std::move(rule));
            else
                impl_->rules[static_cast<std::size_t>(row)] = std::move(rule);
            mutate();
        }
    };
    connect(add, &QPushButton::clicked, this, [openEditor] { openEditor(true); });
    connect(edit, &QPushButton::clicked, this, [openEditor] { openEditor(false); });
    connect(impl_->table, &QTableWidget::cellDoubleClicked, this,
            [openEditor](int, int) { openEditor(false); });
}
MediaOriginsWidget::~MediaOriginsWidget() = default;
void MediaOriginsWidget::readFrom(const DownloadSpec& spec) {
    const QSignalBlocker blocker(this);
    impl_->rules = spec.mediaOrigins;
    impl_->consent->setChecked(spec.mediaOriginsConfirmed);
    impl_->refresh();
}
void MediaOriginsWidget::writeTo(DownloadSpec& spec) const {
    spec.mediaOrigins = impl_->rules;
    spec.mediaOriginsConfirmed = !impl_->rules.empty() && impl_->consent->isChecked();
}
void MediaOriginsWidget::invalidateAccess() {
    DownloadSpec spec;
    writeTo(spec);
    invalidateMediaOrigins(spec);
    readFrom(spec);
    Q_EMIT changed();
}
} // namespace maskpilot::ui
