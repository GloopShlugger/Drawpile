// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LIBSERVER_SESSION_HISTORY_H
#define LIBSERVER_SESSION_HISTORY_H
#include "libserver/idqueue.h"
#include "libserver/sessionban.h"
#include "libshared/net/message.h"
#include "libshared/util/passwordhash.h"
#include <QDateTime>
#include <QObject>
#include <tuple>

namespace protocol {
class ProtocolVersion;
}

namespace server {

class Client;

/**
 * @brief Abstract base class for session history implementations
 *
 * Both the session content as well as the metadata that can persist between
 * server restarts is stored here.
 */
class SessionHistory : public QObject {
	Q_OBJECT
public:
	enum Flag {
		Persistent = 0x01,
		PreserveChat = 0x02,
		Nsfm = 0x04,
		Deputies = 0x08,
		AuthOnly = 0x10,
		IdleOverride = 0x20,
		AllowWeb = 0x40,
	};
	Q_DECLARE_FLAGS(Flags, Flag)

	SessionHistory(const QString &id, QObject *parent);

	//! Get the unique ID of the session
	QString id() const { return m_id; }

	/**
	 * @brief Get the alias for the session ID (if set)
	 *
	 * This should not change during the lifetime of the session.
	 */
	virtual QString idAlias() const = 0;

	//! Get the name of the user who started the session
	virtual QString founderName() const = 0;
	virtual void setFounderName(const QString &founder) = 0;

	//! Get the full protocol version of the session
	virtual protocol::ProtocolVersion protocolVersion() const = 0;

	/**
	 * @brief Get the session's password hash
	 *
	 * An empty QByteArray is returned if no password is set
	 */
	virtual QByteArray passwordHash() const = 0;
	bool checkPassword(const QString &password)
	{
		return passwordhash::check(password, passwordHash());
	}

	//! Set (or clear) this session's password
	virtual void setPasswordHash(const QByteArray &passwordHash) = 0;
	void setPassword(const QString &password)
	{
		setPasswordHash(passwordhash::hash(password));
	}

	//! Get the operator password hash
	virtual QByteArray opwordHash() const = 0;

	//! Set (or clear) the operator password
	virtual void setOpwordHash(const QByteArray &opword) = 0;
	void setOpword(const QString &opword)
	{
		setOpwordHash(passwordhash::hash(opword));
	}

	//! Get the starting timestamp
	QDateTime startTime() const { return m_startTime; }

	//! Get the maximum number of users allowed
	virtual int maxUsers() const = 0;

	//! Set the maximum number of users allowed
	virtual void setMaxUsers(int count) = 0;

	//! Get the title of the session
	virtual QString title() const = 0;

	//! Set the title of the session
	virtual void setTitle(const QString &title) = 0;

	//! Get the persistent session flags
	virtual Flags flags() const = 0;
	bool hasFlag(Flag flag) const { return flags().testFlag(flag); }

	//! Set the persistent session flags
	virtual void setFlags(Flags f) = 0;
	void setFlag(Flag flag, bool on = true)
	{
		Flags f = flags();
		bool isSet = f.testFlag(flag);
		if((on && !isSet) || (!on && isSet)) {
			f.setFlag(flag, on);
			setFlags(f);
		}
	}

	//! Remember a user who joined
	virtual void joinUser(uint8_t id, const QString &name);

	//! Set the history size threshold for requesting autoreset
	virtual void setAutoResetThreshold(uint limit) = 0;

	//! Get the history autoreset request threshold
	virtual uint autoResetThreshold() const = 0;

	//! Get the final autoreset threshold that includes the reset image base
	//! size
	uint effectiveAutoResetThreshold() const;

	//! Get the reset image base size
	uint autoResetThresholdBase() const { return m_autoResetBaseSize; }

	virtual int nextCatchupKey() = 0;

	/**
	 * @brief Add a new message to the history
	 *
	 * The signal newMessagesAvailable() will be emitted.
	 *
	 * @return false if there was no space for this message
	 */
	bool addMessage(const net::Message &msg);

	/**
	 * @brief Add new important message, using emergency space if needed
	 *
	 * This is like addMessage, but will use up to an extra MB of space to
	 * store important messages like joins, leaves and session owners.
	 *
	 * @return false if the emergency space ran out
	 */
	bool addEmergencyMessage(const net::Message &msg);

	/**
	 * @brief Reset the session history
	 *
	 * Resetting replaces the session history and size with the new
	 * set, but index number continue as they were.
	 *
	 * The signal newMessagesAvailable() will be emitted.
	 *
	 * @return false if new history is larger than the size limit
	 */
	bool reset(const net::MessageList &newHistory);

	/**
	 * @brief Get a batch of messages
	 *
	 * This returns a {batch, lastIndex} tuple.
	 * The batch contains zero or more messages immediately following
	 * the given index.
	 *
	 * The second element of the tuple is the index of the last message
	 * in the batch, or lastIndex() if there were no more available messages
	 */
	virtual std::tuple<net::MessageList, int> getBatch(int after) const = 0;

	/**
	 * @brief Mark messages before the given index as unneeded (for now)
	 *
	 * This is used to inform caching history storage backends which messages
	 * have been sent to all connected users and can thus be freed.
	 */
	virtual void cleanupBatches(int before) = 0;

	/**
	 * @brief End this session and delete any associated files (if any)
	 */
	virtual void terminate() = 0;

	/**
	 * @brief Set the hard size limit for the history.
	 *
	 * The size limit is checked when new messages are added to the session.
	 *
	 * See also the autoreset threshold.
	 *
	 * @param limit maximum size in bytes or 0 for no limit
	 */
	void setSizeLimit(uint limit) { m_sizeLimit = limit; }

	/**
	 * @brief Get the session size limit
	 *
	 * If zero, the size is not limited.
	 */
	uint sizeLimit() const { return m_sizeLimit; }

	/**
	 * @brief Get the size of the history in bytes
	 *
	 * Note: this is the serialized size, not the size of the in-memory
	 * representation.
	 */
	uint sizeInBytes() const { return m_sizeInBytes; }

	bool hasRegularSpaceFor(uint bytes) const { return hasSpaceFor(bytes, 0); }

	bool hasEmergencySpaceFor(uint bytes) const
	{
		return hasSpaceFor(bytes, 1024u * 1024u); // 1MiB of emergency space.
	}

	/**
	 * @brief Get the index number of the first message in history
	 *
	 * The index numbers grow monotonically during the session and are
	 * not reset even when the history itself is reset.
	 */
	int firstIndex() const { return m_firstIndex; }

	/**
	 * @brief Get the index number of the last message in history
	 */
	int lastIndex() const { return m_lastIndex; }

	/**
	 * @brief Get the list of in-session IP bans
	 */
	const SessionBanList &banlist() const { return m_banlist; }

	/**
	 * @brief Add a new banlist entry
	 */
	bool addBan(
		const QString &username, const QHostAddress &ip,
		const QString &extAuthId, const QString &sid, const QString &bannedBy,
		const Client *client = nullptr);

	bool importBans(
		const QJsonObject &data, int &outTotal, int &outImported,
		const Client *client = nullptr);

	/**
	 * @brief removeBan Remove a banlist entry
	 */
	QString removeBan(int id);

	/**
	 * @brief Public listing added
	 */
	virtual void addAnnouncement(const QString &url) = 0;

	/**
	 * @brief Public listing removed
	 */
	virtual void removeAnnouncement(const QString &url) = 0;

	/**
	 * @brief Get all announcements that haven't been removed
	 *
	 * This is used at the beginning when a session has been loaded from storage
	 */
	virtual QStringList announcements() const = 0;

	/**
	 * @brief Get the ID queue for this session history
	 *
	 * The ID queue is used to intelligently assign user IDs
	 * so that user rejoining a session get the same ID if still available
	 * and IDs are reused as little as possible.
	 */
	IdQueue &idQueue() { return m_idqueue; }

	/**
	 * @brief Set an authenticated user's operator status
	 *
	 * This is used to remember an authenticated user's status so it
	 * can be automatically restored when they log in again.
	 */
	virtual void setAuthenticatedOperator(const QString &authId, bool op);

	/**
	 * @brief Set an authenticated user's trust status
	 *
	 * This is used to remember an authenticated user's status so it
	 * can be automatically restored when they log in again.
	 */
	virtual void setAuthenticatedTrust(const QString &authId, bool trusted);

	virtual void
	setAuthenticatedUsername(const QString &authId, const QString &username);

	/**
	 * @brief Is the given name on the list of operators
	 */
	bool isOperator(const QString &authId) const
	{
		return m_authOps.contains(authId);
	}

	/**
	 * @brief Is the given name on the list of trusted users
	 */
	bool isTrusted(const QString &authId)
	{
		return m_authTrusted.contains(authId);
	}

	const QString *authenticatedUsernameFor(const QString &authId);

	/**
	 * @brief Are there any names on the list of authenticated operators?
	 */
	bool isAuthenticatedOperators() const { return !m_authOps.isEmpty(); }
	const QSet<QString> &authenticatedOperators() const { return m_authOps; }
	const QSet<QString> &authenticatedTrusted() const { return m_authTrusted; }

	const QHash<QString, QString> &authenticatedUsernames() const
	{
		return m_authUsernames;
	}

signals:
	/**
	 * @brief This signal is emited when new messages are added to the history
	 *
	 * Clients whose upload queues are empty should get the new message batch
	 * and start sending.
	 */
	void newMessagesAvailable();

protected:
	static constexpr int MIN_CATCHUP_KEY = 1;
	static constexpr int MAX_CATCHUP_KEY = 999999999;
	static constexpr int INITIAL_CATCHUP_KEY = 1000000;

	virtual void historyAdd(const net::Message &msg) = 0;
	virtual void historyReset(const net::MessageList &newHistory) = 0;
	virtual void historyAddBan(
		int id, const QString &username, const QHostAddress &ip,
		const QString &extAuthId, const QString &sid,
		const QString &bannedBy) = 0;
	virtual void historyRemoveBan(int id) = 0;
	void historyLoaded(uint size, int messageCount);

	int incrementNextCatchupKey(int &nextCatchupKey);

	SessionBanList m_banlist;

private:
	bool hasSpaceFor(uint bytes, uint extra) const;
	void addMessageInternal(const net::Message &msg, uint bytes);

	QString m_id;
	IdQueue m_idqueue;
	QDateTime m_startTime;

	uint m_sizeInBytes;
	uint m_sizeLimit;
	uint m_autoResetBaseSize;
	int m_firstIndex;
	int m_lastIndex;

	QSet<QString> m_authOps;
	QSet<QString> m_authTrusted;
	QHash<QString, QString> m_authUsernames;
};

// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=69210
namespace diagnostic_marker_private {
class [[maybe_unused]] AbstractSessionHistoryMarker : SessionHistory {
	inline void joinUser(uint8_t, const QString &) override {}
};
}

Q_DECLARE_OPERATORS_FOR_FLAGS(SessionHistory::Flags)

}

#endif
