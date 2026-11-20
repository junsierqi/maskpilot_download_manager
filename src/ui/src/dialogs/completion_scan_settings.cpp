#include <QSettings>
#include <QStringList>
#include <maskpilot/ui/completion_scan_settings.h>
#include <stdexcept>
namespace maskpilot::ui {
CompletionScanOptions loadCompletionScanSettings(QSettings& settings) {
    CompletionScanOptions value;
    value.enabled = settings.value("completionScan/enabled", false).toBool();
    value.program =
        std::filesystem::path(settings.value("completionScan/program").toString().toStdU16String());
    value.arguments.clear();
    for (const auto& argument :
         settings.value("completionScan/arguments", QStringList{"{file}"}).toStringList())
        value.arguments.push_back(argument.toUtf8().toStdString());
    bool valid = false;
    value.timeoutSeconds = settings.value("completionScan/timeoutSeconds", 120).toInt(&valid);
    if (!valid)
        value.timeoutSeconds = 0; // An enabled invalid policy must be repairable, never silently disabled.
    return value;
}
void saveCompletionScanSettings(QSettings& settings, const CompletionScanOptions& value) {
    if (const auto issue = validateCompletionScanOptions(value); !issue.empty())
        throw std::invalid_argument(issue);
    QStringList arguments;
    for (const auto& argument : value.arguments)
        arguments.push_back(QString::fromUtf8(argument));
    settings.setValue("completionScan/enabled", value.enabled);
    settings.setValue("completionScan/program", QString::fromStdU16String(value.program.u16string()));
    settings.setValue("completionScan/arguments", arguments);
    settings.setValue("completionScan/timeoutSeconds", value.timeoutSeconds);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("Scanner settings could not be saved. The current runtime setting is "
                                 "active; repair local settings storage before restarting.");
}
} // namespace maskpilot::ui
