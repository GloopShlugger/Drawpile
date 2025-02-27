// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SESSIONS_INTERFACE_H
#define SESSIONS_INTERFACE_H

#include <QJsonObject>
#include <QString>

class QJsonArray;
class QJsonObject;

namespace protocol {
class ProtocolVersion;
}

namespace server {

class Client;
class Session;

/**
 * Interface for a class that can accept client logins
 */
class Sessions {
public:
	struct JoinResult {
		QString id;
		QJsonObject description;
		enum {
			InviteOk,
			InviteNotFound,
			InviteLimitReached
		} invite = InviteNotFound;

		void setInvite(
			Session *session, Client *client, const QString &inviteSecret);
	};

	virtual ~Sessions();

	/**
	 * Get a list of session descriptions
	 */
	virtual QJsonArray sessionDescriptions() const = 0;

	/**
	 * Get a session with the given ID or alias
	 *
	 * @param id session ID or alias
	 * @param load if true, a session is loaded from template if it's not yet
	 * live
	 * @return session or nullptr if session was not active or couldn't be
	 * loaded
	 */
	virtual Session *getSessionById(const QString &id, bool loadTemplate) = 0;

	virtual JoinResult checkSessionJoin(
		Client *client, const QString &idOrAlias,
		const QString &inviteSecret) = 0;

	/**
	 * Create a new session
	 *
	 * In case of error, one of the following error codes may be returned:
	 *
	 *  - idInuse     - a session with this ID or alias already exists
	 *  - badProtocol - this protocol version is not supported by this server
	 *  - closed      - this server is full or not accepting new sessions
	 *
	 * @param id session unique ID
	 * @param alias session alias (may be empty)
	 * @param protocolVersion session protocol version
	 * @param founder name of the user who created the session
	 *
	 * @return session, error string pair: if session is null, error string
	 * contains the error code
	 */
	virtual std::tuple<Session *, QString> createSession(
		const QString &id, const QString &alias,
		const protocol::ProtocolVersion &protocolVersion,
		const QString &founder) = 0;
};

}

#endif
