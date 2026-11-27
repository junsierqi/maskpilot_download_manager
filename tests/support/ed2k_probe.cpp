#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <maskpilot/application/download_service.h>
#include <maskpilot/infrastructure/adapters.h>
#include <maskpilot/infrastructure/ed2k_engine.h>
#include <thread>

namespace {
std::filesystem::path path(const QString& text) {
    const auto bytes = text.toUtf8();
    return std::filesystem::path(std::u8string(bytes.constData(), bytes.constData() + bytes.size()));
}
class Budget final : public maskpilot::INativePayloadBudget {
    std::uint64_t limit_;

  public:
    std::uint64_t used = 0;
    bool stopped = false;
    explicit Budget(std::int64_t limit)
        : limit_(limit < 0 ? std::numeric_limits<std::uint64_t>::max() : limit) {}
    std::size_t accept(std::size_t bytes) noexcept override {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, limit_ - used));
        used += count;
        stopped = count != bytes || used == limit_;
        return count;
    }
    void finish() noexcept override {}
    bool exhausted() const noexcept override { return stopped; }
    bool failed() const noexcept override { return false; }
};
maskpilot::Ed2kEndpoint endpoint(const QString& text) {
    const auto fields = text.split(':');
    if (fields.size() < 2 || fields.size() > 4)
        throw std::invalid_argument("Invalid fixture endpoint.");
    bool ok = false;
    const auto port = fields[1].toUInt(&ok);
    if (!ok || !port || port > 65535)
        throw std::invalid_argument("Invalid fixture port.");
    maskpilot::Ed2kEndpoint value{fields[0].toStdString(), static_cast<std::uint16_t>(port)};
    if (fields.size() >= 3) {
        const auto flags = fields[2].toUInt(&ok);
        if (!ok || flags > 255)
            throw std::invalid_argument("Invalid fixture flags.");
        value.cryptOptions = static_cast<std::uint8_t>(flags);
    }
    if (fields.size() == 4)
        value.clientHash = fields[3].toStdString();
    return value;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    for (const auto* name : {"url", "destination", "work", "task-id", "source", "server", "quota", "rate",
                             "timeout-ms", "pause-at", "sha256", "proxy", "connections"})
        parser.addOption({QString::fromLatin1(name), QString::fromLatin1(name), "value"});
    parser.addOption({"service", "Exercise the unified service with parallel tasks."});
    parser.addOption({"reject-scan", "Reject the completion scan."});
    parser.process(application);
    try {
        maskpilot::Task task;
        task.id = parser.isSet("task-id") ? parser.value("task-id").toStdString() : maskpilot::newTaskId();
        task.spec.url = parser.value("url").toStdString();
        task.spec.destination = std::filesystem::absolute(path(parser.value("destination")));
        task.spec.proxy = parser.value("proxy").toStdString();
        task.spec.expectedSha256 = parser.value("sha256").toStdString();
        task.spec.connections = parser.isSet("connections") ? parser.value("connections").toInt() : 2;
        task.spec.speedLimit = parser.value("rate").toLongLong();
        const auto work = std::filesystem::absolute(path(parser.value("work")));
        maskpilot::Ed2kOptions options;
        for (const auto& value : parser.values("source"))
            options.sources.push_back(endpoint(value));
        for (const auto& value : parser.values("server"))
            options.servers.push_back(endpoint(value));
        if (parser.isSet("service")) {
            std::filesystem::create_directories(work);
            auto store = maskpilot::makeSqliteStore(work / "tasks.db");
            maskpilot::DownloadService service(
                std::move(store.tasks), maskpilot::makeUnifiedTransferFactory(), work / "parts", {},
                maskpilot::QueueDependencies{
                    std::move(store.queues), maskpilot::makeQueueCivilTime(), {}, store.traffic});
            service.configureLimits(2, 0).get();
            if (parser.isSet("quota")) {
                auto queue = service.queues().get().front();
                maskpilot::QueueConfiguration policy;
                policy.id = queue.id;
                policy.name = queue.name;
                policy.expectedRevision = queue.revision;
                policy.dailyPayloadBudget = parser.value("quota").toLongLong();
                service.configureQueue(policy).get();
            }
            task.spec.ed2k = options;
            service.add(task.spec).get();
            if (!parser.isSet("quota")) {
                task.spec.destination += ".second";
                service.add(task.spec).get();
            }
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
            bool complete = false, waiting = false;
            std::vector<maskpilot::Task> tasks;
            do {
                tasks = service.snapshot();
                complete = !tasks.empty() && std::all_of(tasks.begin(), tasks.end(), [](const auto& t) {
                    return t.state == maskpilot::TaskState::Completed;
                });
                waiting = !tasks.empty() && std::all_of(tasks.begin(), tasks.end(), [](const auto& t) {
                    return t.state == maskpilot::TaskState::PolicyWaiting;
                });
                if (complete || waiting)
                    break;
                for (const auto& t : tasks)
                    if (t.state == maskpilot::TaskState::NeedsAttention ||
                        t.state == maskpilot::TaskState::Failed)
                        throw std::runtime_error(t.error);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < deadline);
            const auto usage = service.queueTraffic(maskpilot::defaultQueueId).get();
            QJsonArray observations;
            for (const auto& observed : tasks)
                observations.append(
                    QJsonObject{{"state", QString::fromLatin1(maskpilot::stateName(observed.state))},
                                {"downloaded", static_cast<qint64>(observed.downloaded)},
                                {"total", static_cast<qint64>(observed.total)},
                                {"peers", observed.peers},
                                {"error", QString::fromStdString(observed.error)}});
            const QJsonObject result{{"tasks", observations},
                                     {"serviceError", QString::fromStdString(service.lastError())},
                                     {"complete", complete},
                                     {"waiting", waiting},
                                     {"payload", static_cast<qint64>(usage.charged)},
                                     {"count", static_cast<int>(tasks.size())}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
            return complete ? 0 : waiting ? 3 : 2;
        }
        auto transport = maskpilot::makeEd2kTransfer(options);
        maskpilot::TransferControl control;
        auto budget =
            std::make_shared<Budget>(parser.isSet("quota") ? parser.value("quota").toLongLong() : -1);
        control.payloadBudget = budget;
        control.speedLimit = task.spec.speedLimit;
        if (parser.isSet("reject-scan"))
            control.scanBeforePublish = [](const std::filesystem::path&) {
                return maskpilot::CompletionScanResult{maskpilot::TransferOutcome::Attention,
                                                       maskpilot::ScanState::Rejected,
                                                       "Fixture scan rejected."};
            };
        const auto timeout = parser.isSet("timeout-ms") ? parser.value("timeout-ms").toLongLong() : 30000;
        const auto pause = parser.isSet("pause-at") ? parser.value("pause-at").toLongLong() : -1;
        std::atomic_bool finished{false};
        std::jthread watcher([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            while (!finished && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!finished)
                control.cancel = true;
        });
        maskpilot::TransferEvent last;
        std::string lastStatus;
        std::size_t events = 0;
        try {
            transport->run(task, work, control, [&](maskpilot::TransferEvent event) {
                last = event;
                if (!event.error.empty()) {
                    if (lastStatus != event.error)
                        std::cerr << "status: " << event.error << '\n';
                    lastStatus = event.error;
                }
                ++events;
                if (pause >= 0 && event.outcome == maskpilot::TransferOutcome::Progress &&
                    event.downloaded >= pause)
                    control.pause = true;
            });
        } catch (...) {
            finished = true;
            throw;
        }
        finished = true;
        watcher.join();
        budget->finish();
        std::string cleanupError;
        if (last.outcome == maskpilot::TransferOutcome::Complete)
            try {
                transport->discard(task, work);
            } catch (const std::exception& error) {
                cleanupError = error.what();
            }
        const QJsonObject result{{"outcome", static_cast<int>(last.outcome)},
                                 {"downloaded", static_cast<qint64>(last.downloaded)},
                                 {"total", static_cast<qint64>(last.total)},
                                 {"payload", static_cast<qint64>(budget->used)},
                                 {"resumable", last.resumable},
                                 {"events", static_cast<qint64>(events)},
                                 {"error", QString::fromStdString(last.error)},
                                 {"cleanupError", QString::fromStdString(cleanupError)},
                                 {"lastStatus", QString::fromStdString(lastStatus)},
                                 {"taskId", QString::fromStdString(task.id)}};
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
        return last.outcome == maskpilot::TransferOutcome::Complete    ? 0
               : last.outcome == maskpilot::TransferOutcome::Paused    ? 3
               : last.outcome == maskpilot::TransferOutcome::Cancelled ? 4
                                                                       : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
