/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "LANConnector.h"
#include "moc_LANConnector.cpp"

#include <QMessageBox>

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

	updateStatus();
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
		QMessageBox* fail = new QMessageBox(QMessageBox::Warning, tr("Couldn't Connect"),
		                                    tr("Could not join %1. Check the IP, port and ROM.").arg(addr),
		                                    QMessageBox::Ok);
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
	bool connected = m_controller && m_controller->isLANConnected();
	m_ui.host->setDisabled(connected);
	m_ui.join->setDisabled(connected);
	m_ui.hostPort->setDisabled(connected);
	m_ui.joinAddr->setDisabled(connected);
	m_ui.joinPort->setDisabled(connected);
	m_ui.disconnect->setEnabled(connected);
	if (connected) {
		m_ui.status->setText(tr("Connected (slot %1).").arg(m_controller->lanSlot()));
	} else {
		m_ui.status->setText(tr("Not connected."));
	}
}
