#include <QDateTime>
#include <QTimeZone>
#include <maskpilot/infrastructure/adapters.h>

namespace maskpilot {
namespace {
class QueueCivilTime final : public IQueueCivilTime {
  public:
    std::int64_t utcNow() const override { return QDateTime::currentSecsSinceEpoch(); }
    std::optional<CivilMinute> at(const std::string& zone, std::int64_t utc) const override {
        const auto id = QByteArray::fromStdString(zone);
        if (!QTimeZone::isTimeZoneIdAvailable(id))
            return {};
        const QTimeZone timeZone(id);
        if (!timeZone.isValid())
            return {};
        const auto local = QDateTime::fromSecsSinceEpoch(utc, QTimeZone::utc()).toTimeZone(timeZone);
        if (!local.isValid())
            return {};
        return CivilMinute{local.date().dayOfWeek() - 1, local.time().hour() * 60 + local.time().minute()};
    }
};
} // namespace
std::shared_ptr<IQueueCivilTime> makeQueueCivilTime() { return std::make_shared<QueueCivilTime>(); }
} // namespace maskpilot
