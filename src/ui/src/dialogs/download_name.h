#pragma once
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <maskpilot/domain/task.h>

namespace maskpilot::ui {
// A display default only. Application/domain validation remains authoritative.
inline QString defaultDownloadName(const QString& url) {
    auto name = url.startsWith("torrent:") ? QFileInfo(url.mid(8)).completeBaseName()
                                           : QFileInfo(QUrl(url).path()).fileName();
    if (isEd2kUrl(url.toStdString())) {
        try {
            name = QString::fromStdString(parseEd2kFileLink(url.toStdString()).filename);
        } catch (const std::invalid_argument&) {
            name.clear();
        }
    }
    if (url.startsWith("magnet:?"))
        name = QUrlQuery(QUrl(url)).queryItemValue("dn");
    if (isTorrentUrl(url.toStdString()) && name.isEmpty())
        name = "Torrent download";
    name.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f\x7f])"), "_");
    while (name.endsWith('.') || name.endsWith(' '))
        name.chop(1);
    if (name.isEmpty() || name == "." || name == "..")
        name = "download.bin";
    if (QRegularExpression(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$))",
                           QRegularExpression::CaseInsensitiveOption)
            .match(name)
            .hasMatch())
        name.prepend('_');
    name = name.left(180);
    if (!name.isEmpty() && name.back().isHighSurrogate())
        name.chop(1);
    return name;
}
} // namespace maskpilot::ui
