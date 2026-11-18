#pragma once
#include <QString>
#include <QTimer>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>

namespace maskpilot::ui {
template <class T, class Success>
void watchFuture(std::future<T> future, QObject* owner, Success success,
                 std::function<void(QString)> failure) {
    auto value = std::make_shared<std::future<T>>(std::move(future));
    auto* timer = new QTimer(owner);
    QObject::connect(timer, &QTimer::timeout, owner,
                     [timer, value, success = std::move(success), failure = std::move(failure)]() mutable {
                         if (value->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                             return;
                         timer->stop();
                         timer->deleteLater();
                         try {
                             if constexpr (std::is_void_v<T>) {
                                 value->get();
                                 success();
                             } else {
                                 success(value->get());
                             }
                         } catch (const std::exception& error) {
                             failure(QString::fromUtf8(error.what()));
                         }
                     });
    timer->start(20);
}
} // namespace maskpilot::ui
