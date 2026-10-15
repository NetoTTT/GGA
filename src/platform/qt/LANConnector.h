/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "ui_LANConnector.h"

#include <memory>

namespace QGBA {

class CoreController;
class Window;

class LANConnector : public QDialog {
Q_OBJECT

public:
	LANConnector(Window* window, QWidget* parent = nullptr);

public slots:
	void host();
	void join();
	void detach();

private slots:
	void updateStatus();

private:
	Ui::LANConnector m_ui;

	std::shared_ptr<CoreController> m_controller;
	Window* m_window;
};

}
