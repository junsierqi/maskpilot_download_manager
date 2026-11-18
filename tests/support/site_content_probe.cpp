#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <fstream>
#include <iostream>
#include <maskpilot/infrastructure/adapters.h>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc != 1)
            throw std::invalid_argument("Fixture accepts one bounded JSON request on stdin.");
        std::string request;
        char buffer[4096];
        while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount()) {
            request.append(buffer, static_cast<std::size_t>(std::cin.gcount()));
            if (request.size() > 16 * 1024 * 1024)
                throw std::invalid_argument("Fixture request exceeds limit.");
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(request), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            throw std::invalid_argument("Invalid fixture request.");
        const auto object = doc.object();
        const auto body = QByteArray::fromBase64(object.value("bodyBase64").toString().toLatin1());
        maskpilot::SiteContentInput input{object.value("url").toString().toStdString(),
                                          object.value("contentType").toString().toStdString(),
                                          body.toStdString()};
        const auto mappings = object.value("mappings").toObject();
        const auto result = maskpilot::makeSiteContentProcessor()->process(
            input, {}, [&](const maskpilot::SiteContentReference& ref) -> std::optional<std::string> {
                const auto value = mappings.value(QString::fromStdString(ref.url));
                return value.isString() ? std::optional(value.toString().toStdString()) : std::nullopt;
            });
        QJsonArray references;
        for (const auto& ref : result.references)
            references.append(
                QJsonObject{{"url", QString::fromStdString(ref.url)}, {"kind", static_cast<int>(ref.kind)}});
        const auto output = QJsonDocument(QJsonObject{{"body", QString::fromStdString(result.body)},
                                                      {"references", references}})
                                .toJson(QJsonDocument::Compact);
        std::cout.write(output.constData(), output.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
