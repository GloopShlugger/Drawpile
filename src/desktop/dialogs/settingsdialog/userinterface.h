// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DESKTOP_DIALOGS_SETTINGSDIALOG_USERINTERFACE_H
#define DESKTOP_DIALOGS_SETTINGSDIALOG_USERINTERFACE_H
#include "desktop/dialogs/settingsdialog/page.h"

class QFormLayout;
class QVBoxLayout;

namespace desktop {
namespace settings {
class Settings;
}
}

namespace dialogs {
namespace settingsdialog {

class UserInterface final : public Page {
	Q_OBJECT
public:
	UserInterface(
		desktop::settings::Settings &settings, QWidget *parent = nullptr);

protected:
	void
	setUp(desktop::settings::Settings &settings, QVBoxLayout *layout) override;

private:
	void
	initInterfaceMode(desktop::settings::Settings &settings, QFormLayout *form);

	void initKineticScrolling(
		desktop::settings::Settings &settings, QFormLayout *form);

	void
	initMiscellaneous(desktop::settings::Settings &settings, QFormLayout *form);

	void initRequiringRestart(
		desktop::settings::Settings &settings, QFormLayout *form);

	void pickColor(
		desktop::settings::Settings &settings,
		QColor (desktop::settings::Settings::*getColor)() const,
		void (desktop::settings::Settings::*setColor)(QColor),
		const QColor &defaultColor);
};

}
}

#endif
