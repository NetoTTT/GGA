/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef QGBA_LAN_CONTROL_SERVER
#define QGBA_LAN_CONTROL_SERVER

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QJsonObject;

namespace QGBA {

class Window;

/* Loopback-only JSON-lines control surface for scripted netplay tests.
 * Enabled with GGA_CTL_PORT=<port>; the actual bound port is written to
 * <appdata>/ctl-<pid>.port so a client can discover it. One command per
 * line: {"id":1,"cmd":"lan.host","params":{"port":27183}} */
class LanControlServer : public QObject {
Q_OBJECT

public:
	static LanControlServer* startFromEnvironment(Window* window);
	quint16 port() const { return m_port; }

private:
	LanControlServer(Window* window, QObject* parent = nullptr);
	bool listen(quint16 preferred);
	QString portFilePath() const;
	QString logFilePath() const;
	void writePortFile() const;
	void write(QTcpSocket* socket, int id, bool ok, const QJsonObject& payload);
	QJsonObject dispatch(const QString& cmd, const QJsonObject& params);
	QJsonObject status() const;

	Window* m_window;
	QTcpServer* m_server;
	QHash<QTcpSocket*, QByteArray> m_pending;
	quint16 m_port = 0;
};

}
#endif
