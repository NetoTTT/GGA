/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "LanControlServer.h"

#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

#include "CoreController.h"
#include "Window.h"

using namespace QGBA;

LanControlServer::LanControlServer(Window* window, QObject* parent)
	: QObject(parent)
	, m_window(window)
	, m_server(new QTcpServer(this))
{
	connect(m_server, &QTcpServer::newConnection, this, [this]() {
		while (QTcpSocket* socket = m_server->nextPendingConnection()) {
			m_pending.insert(socket, QByteArray());
			connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
				QByteArray& buffer = m_pending[socket];
				buffer += socket->readAll();
				int newline;
				while ((newline = buffer.indexOf('\n')) >= 0) {
					QByteArray line = buffer.left(newline);
					buffer.remove(0, newline + 1);
					int id = -1;
					QString cmd;
					QJsonObject params;
					QJsonParseError error{};
					QJsonDocument doc = QJsonDocument::fromJson(line, &error);
					if (error.error == QJsonParseError::NoError && doc.isObject()) {
						QJsonObject request = doc.object();
						id = request.value(QStringLiteral("id")).toInt(-1);
						cmd = request.value(QStringLiteral("cmd")).toString();
						params = request.value(QStringLiteral("params")).toObject();
					} else {
						QJsonObject payload;
						payload.insert(QStringLiteral("error"), QStringLiteral("bad json"));
						write(socket, id, false, payload);
						continue;
					}
					QJsonObject result = dispatch(cmd, params);
					bool ok = !result.contains(QStringLiteral("error"));
					write(socket, id, ok, result);
				}
			});
			connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
				m_pending.remove(socket);
				socket->deleteLater();
			});
		}
	});
}

bool LanControlServer::listen(quint16 preferred) {
	for (quint16 port = preferred; port < preferred + 32; ++port) {
		if (m_server->listen(QHostAddress::LocalHost, port)) {
			m_port = m_server->serverPort();
			return true;
		}
	}
	return false;
}

QString LanControlServer::portFilePath() const {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	     + QStringLiteral("/ctl-%1.port").arg(QCoreApplication::applicationPid());
}

QString LanControlServer::logFilePath() const {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	     + QStringLiteral("/lanplay-%1.log").arg(QCoreApplication::applicationPid());
}

void LanControlServer::writePortFile() const {
	QFile file(portFilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QByteArray::number(m_port));
		file.write("\n");
	}
}

LanControlServer* LanControlServer::startFromEnvironment(Window* window) {
	QByteArray raw = qgetenv("GGA_CTL_PORT");
	if (raw.isEmpty()) {
		return nullptr;
	}
	bool ok = false;
	uint wanted = raw.toUInt(&ok);
	if (!ok || !wanted || wanted > 0xFFFF) {
		wanted = 27800;
	}
	LanControlServer* server = new LanControlServer(window, window);
	if (!server->listen(static_cast<quint16>(wanted))) {
		fprintf(stderr, "DBG ctl: could not listen on %u..%u\n",
			static_cast<unsigned>(wanted), static_cast<unsigned>(wanted + 31));
		delete server;
		return nullptr;
	}
	server->writePortFile();
	fprintf(stderr, "DBG ctl: listening on 127.0.0.1:%u (pid %u)\n",
		static_cast<unsigned>(server->port()),
		static_cast<unsigned>(QCoreApplication::applicationPid()));
	return server;
}

void LanControlServer::write(QTcpSocket* socket, int id, bool ok, const QJsonObject& payload) {
	QJsonObject response;
	response.insert(QStringLiteral("id"), id);
	response.insert(QStringLiteral("ok"), ok);
	if (ok) {
		response.insert(QStringLiteral("result"), payload);
	} else {
		response.insert(QStringLiteral("error"), payload.value(QStringLiteral("error")).toString());
	}
	socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
}

QJsonObject LanControlServer::status() const {
	QJsonObject out;
	std::shared_ptr<CoreController> controller = m_window->controller();
	out.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
	out.insert(QStringLiteral("port"), static_cast<double>(m_port));
	out.insert(QStringLiteral("rom"), static_cast<bool>(controller));
	if (!controller) {
		return out;
	}
	out.insert(QStringLiteral("platform_gba"), controller->platform() == mPLATFORM_GBA);
	out.insert(QStringLiteral("frames"), static_cast<double>(controller->frameCounter()));
	out.insert(QStringLiteral("lan_v2_active"), controller->isLAN2Active());
	out.insert(QStringLiteral("lan_v2_hosting"), controller->isLAN2Hosting());
	out.insert(QStringLiteral("lan_v2_running"), controller->isLAN2Running());
	out.insert(QStringLiteral("lan_v2_frame"), controller->lan2Frame());
	out.insert(QStringLiteral("lan_v2_error"), controller->lanError());
	return out;
}

QJsonObject LanControlServer::dispatch(const QString& cmd, const QJsonObject& params) {
	QJsonObject out;
	std::shared_ptr<CoreController> controller = m_window->controller();

	if (cmd == QLatin1String("ping")) {
		out.insert(QStringLiteral("pong"), true);
		return out;
	}
	if (cmd == QLatin1String("lan.status") || cmd == QLatin1String("status")) {
		return status();
	}
	if (cmd == QLatin1String("emu.quit") || cmd == QLatin1String("quit")) {
		QCoreApplication::quit();
		out.insert(QStringLiteral("quitting"), true);
		return out;
	}
	if (cmd == QLatin1String("log.path")) {
		out.insert(QStringLiteral("path"), logFilePath());
		return out;
	}
	if (cmd == QLatin1String("log.tail")) {
		int lines = params.value(QStringLiteral("lines")).toInt(40);
		if (lines < 1) {
			lines = 1;
		}
		QFile file(logFilePath());
		if (!file.open(QIODevice::ReadOnly)) {
			out.insert(QStringLiteral("lines"), QJsonValue(QJsonArray()));
			return out;
		}
		QList<QByteArray> all = file.readAll().split('\n');
		if (all.size() > lines) {
			all = all.mid(all.size() - lines);
		}
		QJsonArray array;
		for (const QByteArray& line : all) {
			array.append(QString::fromUtf8(line));
		}
		out.insert(QStringLiteral("path"), logFilePath());
		out.insert(QStringLiteral("lines"), array);
		return out;
	}
	if (!controller) {
		out.insert(QStringLiteral("error"), QStringLiteral("no rom loaded"));
		return out;
	}
	if (cmd == QLatin1String("lan.host")) {
		int port = params.value(QStringLiteral("port")).toInt(27182);
		CoreController::Interrupter interrupter(controller);
		bool ok = controller->attachLAN2(true, QString(), port);
		interrupter.resume();
		if (!ok) {
			out.insert(QStringLiteral("error"), QStringLiteral("could not listen on port %1").arg(port));
			return out;
		}
		out.insert(QStringLiteral("hosting"), true);
		return out;
	}
	if (cmd == QLatin1String("lan.join")) {
		QString addr = params.value(QStringLiteral("addr")).toString();
		if (addr.isEmpty()) {
			addr = QStringLiteral("127.0.0.1");
		}
		int port = params.value(QStringLiteral("port")).toInt(27182);
		CoreController::Interrupter interrupter(controller);
		bool ok = controller->attachLAN2(false, addr, port);
		interrupter.resume();
		if (!ok) {
			QString why = controller->lanError();
			out.insert(QStringLiteral("error"),
				why.isEmpty() ? QStringLiteral("could not join %1:%2").arg(addr).arg(port) : why);
			return out;
		}
		out.insert(QStringLiteral("joined"), true);
		return out;
	}
	if (cmd == QLatin1String("lan.stop")) {
		CoreController::Interrupter interrupter(controller);
		controller->detachLAN2();
		interrupter.resume();
		out.insert(QStringLiteral("stopped"), true);
		return out;
	}
	if (cmd == QLatin1String("lan.poll")) {
		controller->pollHostSession();
		controller->pollClientAttach();
		out.insert(QStringLiteral("polled"), true);
		return out;
	}
	if (cmd == QLatin1String("state.dump")) {
		int slot = params.value(QStringLiteral("slot")).toInt(0);
		QString path = params.value(QStringLiteral("path")).toString();
		if (path.isEmpty()) {
			out.insert(QStringLiteral("error"), QStringLiteral("missing path"));
			return out;
		}
		controller->lan2DumpState(slot, path);
		out.insert(QStringLiteral("dumped"), path);
		return out;
	}

	out.insert(QStringLiteral("error"), QStringLiteral("unknown command: %1").arg(cmd));
	return out;
}
