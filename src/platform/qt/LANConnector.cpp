/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "LANConnector.h"
#include "moc_LANConnector.cpp"

#include <QMessageBox>
#include <QAbstractSocket>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QStringList>

#include "CoreController.h"
#include "Window.h"

using namespace QGBA;

LANConnector::LANConnector(Window* window, QWidget* parent)
	: QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint)
	, m_window(window)
{
	m_ui.setupUi(this);

	connect(window, &QObject::destroyed, this, &QWidget::close);
	connect(m_ui.host, &QAbstractButton::clicked, this, &LANConnector::host);
	connect(m_ui.join, &QAbstractButton::clicked, this, &LANConnector::join);
	connect(m_ui.disconnect, &QAbstractButton::clicked, this, &LANConnector::detach);
	connect(&m_statusTimer, &QTimer::timeout, this, &LANConnector::updateStatus);
	m_statusTimer.start(1000);

	updateStatus();
}

static QString hostAddresses() {
	QStringList addresses;
	for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
		const auto flags = interface.flags();
		if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
		    || flags.testFlag(QNetworkInterface::IsLoopBack)) {
			continue;
		}
		for (const QNetworkAddressEntry& entry : interface.addressEntries()) {
			const QHostAddress& address = entry.ip();
			if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()
			    || address.isMulticast() || address.toIPv4Address() == 0) {
				continue;
			}
			addresses.append(QStringLiteral("%1: %2").arg(interface.humanReadableName(), address.toString()));
		}
	}
	if (addresses.isEmpty()) {
		return QObject::tr("No active IPv4 addresses found.");
	}
	return addresses.join(QLatin1Char('\n'));
}

static CoreController* needController(Window* window) {
	if (!window->controller()) {
		window->selectROM();
	}
	if (!window->controller() || window->controller()->platform() != mPLATFORM_GBA) {
		return nullptr;
	}
	return window->controller().get();
}

void LANConnector::host() {
	CoreController* raw = needController(m_window);
	if (!raw) {
		return;
	}
	m_controller = m_window->controller();
	CoreController::Interrupter interrupter(m_controller);
	if (!m_controller->attachLAN(true, QString(), m_ui.hostPort->value())) {
		QMessageBox* fail = new QMessageBox(QMessageBox::Warning, tr("Couldn't Host"),
		                                    tr("Could not listen on port %1.").arg(m_ui.hostPort->value()),
		                                    QMessageBox::Ok);
		fail->setAttribute(Qt::WA_DeleteOnClose);
		fail->show();
		m_controller.reset();
	}
	interrupter.resume();
	updateStatus();
}

void LANConnector::join() {
	QString addr = m_ui.joinAddr->text().trimmed();
	if (addr.isEmpty()) {
		return;
	}
	CoreController* raw = needController(m_window);
	if (!raw) {
		return;
	}
	m_controller = m_window->controller();
	CoreController::Interrupter interrupter(m_controller);
	if (!m_controller->attachLAN(false, addr, m_ui.joinPort->value())) {
		QString why;
		switch (m_controller->lanRejectReason()) {
		case 3:
			why = tr("ROM mismatch: both sides need the identical ROM dump.");
			break;
		case 1:
		case 2:
			why = tr("Version mismatch: update both sides to the same build.");
			break;
		default:
			why = tr("Could not join %1. Check the IP, port and ROM.").arg(addr);
			break;
		}
		QMessageBox* fail = new QMessageBox(QMessageBox::Warning, tr("Couldn't Connect"), why, QMessageBox::Ok);
		fail->setAttribute(Qt::WA_DeleteOnClose);
		fail->show();
		m_controller.reset();
	}
	interrupter.resume();
	updateStatus();
}

void LANConnector::detach() {
	if (m_controller) {
		m_controller->detachLAN();
		m_controller.reset();
	}
	updateStatus();
}

void LANConnector::updateStatus() {
	if (m_controller && m_controller->pollHostSession()) {
		// Session just started; status text below reflects it.
	}
	bool connected = m_controller && m_controller->isLANConnected();
	bool hosting = m_controller && m_controller->isLANHosting();
	bool session = m_controller && m_controller->isLANSessionActive();
	bool active = connected || hosting;
	m_ui.host->setDisabled(active);
	m_ui.join->setDisabled(active);
	m_ui.hostPort->setDisabled(active);
	m_ui.joinAddr->setDisabled(active);
	m_ui.joinPort->setDisabled(active);
	m_ui.disconnect->setEnabled(active);
	m_ui.clientCount->setText(tr("Remote clients: %1 / 1").arg(session && hosting ? 1 : 0));
	QString addresses = hostAddresses();
	if (m_ui.hostAddresses->toPlainText() != addresses) {
		m_ui.hostAddresses->setPlainText(addresses);
	}
	if (connected && session) {
		if (m_controller->lanSlot() == 0) {
			m_ui.status->setText(tr("Host P0 connected to client P1."));
		} else {
			m_ui.status->setText(tr("Client P1 connected to host P0."));
		}
	} else if (hosting) {
		m_ui.status->setText(tr("Hosting on port %1: playing solo until P1 joins, then the session restarts together.").arg(m_ui.hostPort->value()));
	} else if (connected) {
		m_ui.status->setText(tr("Connected; waiting for the session to start."));
	} else {
		m_ui.status->setText(tr("Not connected."));
	}
}
