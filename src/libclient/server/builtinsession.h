// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LIBCLIENT_SERVER_BUILTINSESSION_H
#define LIBCLIENT_SERVER_BUILTINSESSION_H
#include "libclient/drawdance/aclstate.h"
#include "libserver/session.h"

namespace canvas {
class PaintEngine;
}

namespace drawdance {
class CanvasState;
}

namespace server {

class BuiltinSession final : public Session {
	Q_OBJECT
public:
	BuiltinSession(
		ServerConfig *config, sessionlisting::Announcements *announcements,
		canvas::PaintEngine *paintEngine, const QString &id,
		const QString &idAlias, const QString &founder,
		QObject *parent = nullptr);

	BuiltinSession(const BuiltinSession &) = delete;
	BuiltinSession(BuiltinSession &&) = delete;
	BuiltinSession &operator=(const BuiltinSession &) = delete;
	BuiltinSession &operator=(BuiltinSession &&) = delete;

	bool supportsAutoReset() const override;
	void readyToAutoReset(
		const AutoResetResponseParams &params, const QString &payload) override;

	void doInternalReset(const drawdance::CanvasState &canvasState);

	StreamResetStartResult
	handleStreamResetStart(int ctxId, const QString &correlator) override;

	StreamResetAbortResult handleStreamResetAbort(int ctxId) override;

	StreamResetPrepareResult
	handleStreamResetFinish(int ctxId, int expectedMessageCount) override;

protected:
	void addToHistory(const net::Message &msg) override;
	void onSessionInitialized() override;
	void onSessionReset() override;
	void onClientJoin(Client *client, bool host) override;
	void onClientDeop(Client *client) override;
	void onResetStream(Client &client, const net::Message &msg) override;
	void onStateChanged() override;

private:
	void internalReset(const drawdance::CanvasState &canvasState);

	canvas::PaintEngine *m_paintEngine;
	drawdance::AclState m_acls;
	net::MessageList m_resetImage;
	size_t m_resetImageSize = 0;
	QString m_pinnedMessage;
	int m_defaultLayer = 0;
	bool m_softResetRequested = false;
};
}

#endif
