#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

// Test-owned TCP origin. Real curl consumes its Range responses while the Qt loop remains responsive.
class LocalHttpServer final : public QObject {
    QTcpServer server_;
    QByteArray data_;
    struct Request {
        QString path;
        bool probe;
    };
    std::vector<Request> requests_;

  public:
    explicit LocalHttpServer(QObject* parent = nullptr) : QObject(parent), data_(2 * 1024 * 1024, '\0') {
        for (qsizetype n = 0; n < data_.size(); ++n)
            data_[n] = static_cast<char>((n + (n / 4096) * 31) % 251);
        if (!server_.listen(QHostAddress::LocalHost, 0))
            throw std::runtime_error("Cannot bind an owned HTTP test origin");
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
                    if (socket->property("responding").toBool())
                        return;
                    input->append(socket->readAll());
                    if (input->size() > 64 * 1024) {
                        socket->abort();
                        return;
                    }
                    if (!input->contains("\r\n\r\n"))
                        return;
                    socket->setProperty("responding", true);
                    const auto lines = input->split('\n');
                    const auto first = lines.front().trimmed().split(' ');
                    if (first.size() != 3 || first.front() != "GET") {
                        socket->abort();
                        return;
                    }
                    qint64 begin = 0, end = data_.size() - 1;
                    bool ranged = false;
                    for (auto line : lines) {
                        line = line.trimmed();
                        if (!line.toLower().startsWith("range:"))
                            continue;
                        const auto match = QRegularExpression("^Range: bytes=(\\d+)-(\\d+)$",
                                                              QRegularExpression::CaseInsensitiveOption)
                                               .match(QString::fromLatin1(line));
                        if (!match.hasMatch()) {
                            socket->abort();
                            return;
                        }
                        begin = match.captured(1).toLongLong();
                        end = match.captured(2).toLongLong();
                        ranged = true;
                    }
                    if (begin < 0 || end < begin || end >= data_.size()) {
                        socket->abort();
                        return;
                    }
                    const bool probe = ranged && begin == 0 && end == 0;
                    requests_.push_back({QString::fromUtf8(first[1]), probe});
                    QByteArray response = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                    response += "ETag: \"queue-ui-v1\"\r\nConnection: close\r\nContent-Length: " +
                                QByteArray::number(end - begin + 1) + "\r\n";
                    if (ranged)
                        response += "Content-Range: bytes " + QByteArray::number(begin) + "-" +
                                    QByteArray::number(end) + "/" + QByteArray::number(data_.size()) + "\r\n";
                    response += "\r\n";
                    socket->write(response);
                    if (probe) {
                        socket->write(data_.left(1));
                        socket->disconnectFromHost();
                        return;
                    }
                    auto position = std::make_shared<qint64>(begin);
                    auto elapsed = std::make_shared<QElapsedTimer>();
                    elapsed->start();
                    auto* tick = new QTimer(socket);
                    tick->setTimerType(Qt::PreciseTimer);
                    tick->setInterval(5);
                    connect(tick, &QTimer::timeout, socket,
                            [this, socket, position, elapsed, begin, end, tick] {
                                if (socket->state() != QAbstractSocket::ConnectedState) {
                                    tick->stop();
                                    return;
                                }
                                // Keep the intended 1024 bytes/5ms rate when an event loop
                                // coalesces timer callbacks; bound each catch-up write.
                                const auto due = (elapsed->elapsed() / 5) * 1024 - (*position - begin);
                                const auto count = std::min<qint64>({due, 16 * 1024, end - *position + 1});
                                if (count <= 0)
                                    return;
                                socket->write(data_.constData() + *position, count);
                                *position += count;
                                if (*position > end) {
                                    tick->stop();
                                    socket->disconnectFromHost();
                                }
                            });
                    tick->start();
                });
            }
        });
    }
    QString url(const QString& path) const {
        return "http://127.0.0.1:" + QString::number(server_.serverPort()) + path;
    }
    int bodyRequests(const QString& path) const {
        return static_cast<int>(std::count_if(requests_.begin(), requests_.end(), [&](const auto& item) {
            return item.path == path && !item.probe;
        }));
    }
    int requests(const QString& path) const {
        return static_cast<int>(std::count_if(requests_.begin(), requests_.end(),
                                              [&](const auto& item) { return item.path == path; }));
    }
    const QByteArray& data() const { return data_; }
};
