// SPDX-License-Identifier: GPL-3.0-or-later
#include "thinsrv/multiserver.h"
#include "cmake-config/config.h"
#include "libserver/jsonapi.h"
#include "libserver/serverconfig.h"
#include "libserver/serverlog.h"
#include "libserver/session.h"
#include "libserver/sessionserver.h"
#include "libserver/thinserverclient.h"
#include "libshared/net/servercmd.h"
#include "libshared/util/qtcompat.h"
#include "libshared/util/whatismyip.h"
#include "thinsrv/database.h"
#include "thinsrv/extbans.h"
#include "thinsrv/initsys.h"
#include "thinsrv/templatefiles.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#ifdef HAVE_WEBSOCKETS
#	include <QWebSocket>
#	include <QWebSocketServer>
#endif

namespace server {

MultiServer::MultiServer(ServerConfig *config, QObject *parent)
	: QObject(parent),
	m_config(config),
	m_tcpServer(nullptr),
#ifdef HAVE_WEBSOCKETS
	m_webSocketServer(nullptr),
#endif
	m_extBans(new ExtBans(config, this)),
	m_state(STOPPED),
	m_autoStop(false),
	m_port(0)
{
	m_sessions = new SessionServer(config, this);
	m_started = QDateTime::currentDateTimeUtc();

	Database *db = qobject_cast<Database*>(m_config);
	if(db) {
		db->loadExternalIpBans(m_extBans);
	}

	connect(this, &MultiServer::serverStarted, m_extBans, &ExtBans::start);
	connect(this, &MultiServer::serverStopped, m_extBans, &ExtBans::stop);

	connect(m_sessions, &SessionServer::sessionCreated, this, &MultiServer::assignRecording);
	connect(m_sessions, &SessionServer::sessionEnded, this, &MultiServer::tryAutoStop);
	connect(m_sessions, &SessionServer::userCountChanged, [this](int users) {
		printStatusUpdate();
		emit userCountChanged(users);
		// The server will be fully stopped after all users have disconnected
		if(users == 0) {
			if(m_state == STOPPING)
				stop();
			else
				tryAutoStop();
		}
	});
}

/**
 * @brief Automatically stop server when last session is closed
 *
 * This is used in socket activation mode. The server will be restarted
 * by the system init daemon when needed again.
 * @param autostop
 */
void MultiServer::setAutoStop(bool autostop)
{
	m_autoStop = autostop;
}

void MultiServer::setRecordingPath(const QString &path)
{
	m_recordingPath = path;
}

void MultiServer::setSessionDirectory(const QDir &path)
{
	m_sessions->setSessionDir(path);
}

void MultiServer::setTemplateDirectory(const QDir &dir)
{
	const TemplateLoader *old = m_sessions->templateLoader();
	TemplateFiles *loader = new TemplateFiles(dir, m_sessions);
	m_sessions->setTemplateLoader(loader);
	delete old;
}

bool MultiServer::createServer()
{
	return createServer(false);
}

bool MultiServer::createServer(bool enableWebSockets)
{
	if(!m_sslCertFile.isEmpty() && !m_sslKeyFile.isEmpty()) {
		SslServer *server =
			new SslServer(m_sslCertFile, m_sslKeyFile, m_sslKeyAlgorithm, this);
		if(!server->isValidCert()) {
			emit serverStartError("Couldn't load TLS certificate");
			return false;
		}
		m_tcpServer = server;

	} else {
		m_tcpServer = new QTcpServer(this);
	}

	connect(m_tcpServer, &QTcpServer::newConnection, this, &MultiServer::newTcpClient);

	if (enableWebSockets) {
#ifdef HAVE_WEBSOCKETS
		// TODO: Allow running a TLS-secured WebSocket server. Currently, this
		// instead requires a reverse proxy server like nginx or something. The
		// user almost certainly has one of those anyway though, so it's fine.
		m_webSocketServer = new QWebSocketServer(
			QStringLiteral("drawpile-srv_%1").arg(cmake_config::version()),
			QWebSocketServer::NonSecureMode, this);
		connect(
			m_webSocketServer, &QWebSocketServer::newConnection, this,
			&MultiServer::newWebSocketClient);
#else
		qWarning("WebSocket server requested, but support not compiled in");
#endif
	}

	return true;
}

bool MultiServer::abortStart()
{
		delete m_tcpServer;
		m_tcpServer = nullptr;
#ifdef HAVE_WEBSOCKETS
		delete m_webSocketServer;
		m_webSocketServer = nullptr;
#endif
		m_state = STOPPED;
		return false;
}

void MultiServer::updateInternalConfig()
{
	InternalConfig icfg = m_config->internalConfig();
	icfg.realPort = m_port;
#ifdef HAVE_WEBSOCKETS
	icfg.webSocket = m_webSocketServer != nullptr;
#endif
	m_config->setInternalConfig(icfg);
}

/**
 * @brief Start listening on the specified address.
 * @param port the port to listen on
 * @param address listening address
 * @return true on success
 */
bool MultiServer::start(
	quint16 tcpPort, const QHostAddress &tcpAddress, quint16 webSocketPort,
	const QHostAddress &webSocketAddress)
{
	Q_ASSERT(m_state == STOPPED);
	m_state = RUNNING;
	if(!createServer(webSocketPort != 0)) {
		return abortStart();
	}

	if(!m_tcpServer->listen(tcpAddress, tcpPort)) {
		emit serverStartError(m_tcpServer->errorString());
		m_sessions->config()->logger()->logMessage(Log().about(Log::Level::Error, Log::Topic::Status).message(m_tcpServer->errorString()));
		return abortStart();
	}

#ifdef HAVE_WEBSOCKETS
	if(m_webSocketServer && !m_webSocketServer->listen(webSocketAddress, webSocketPort)) {
		emit serverStartError(m_webSocketServer->errorString());
		m_sessions->config()->logger()->logMessage(Log().about(Log::Level::Error, Log::Topic::Status)
			.message(m_webSocketServer->errorString()));
		return abortStart();
	}
#else
	Q_UNUSED(webSocketAddress);
#endif

	m_port = m_tcpServer->serverPort();
	updateInternalConfig();

	emit serverStarted();
	m_sessions->config()->logger()->logMessage(
		Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.message(QString("Started listening for TCP connections on port %1 "
							 "at address %2")
						 .arg(tcpPort)
						 .arg(tcpAddress.toString())));
#ifdef HAVE_WEBSOCKETS
	if(m_webSocketServer) {
		m_sessions->config()->logger()->logMessage(
			Log()
				.about(Log::Level::Info, Log::Topic::Status)
				.message(QString("Started listening for WebSocket connections "
								 "on port %1 at address %2")
							 .arg(webSocketPort)
							 .arg(webSocketAddress.toString())));
	}
#endif
	return true;
}

/**
 * @brief Start listening on the given file descriptor
 * @param fd
 * @return true on success
 */
bool MultiServer::startFd(
	int tcpFd, int webSocketFd, const QStringList &ignoredOptions)
{
	Q_ASSERT(m_state == STOPPED);
	m_state = RUNNING;
	if(!createServer(webSocketFd > 0))
		return false;

	for(const QString &ignoredOption : ignoredOptions) {
		m_sessions->config()->logger()->logMessage(
			Log()
				.about(Log::Level::Warn, Log::Topic::Status)
				.message(QStringLiteral("Command-line argument %1 ignored "
										"because sockets are passed via %2")
							 .arg(ignoredOption, initsys::name())));
	}

	if(!m_tcpServer->setSocketDescriptor(tcpFd)) {
		m_sessions->config()->logger()->logMessage(
			Log()
				.about(Log::Level::Error, Log::Topic::Status)
				.message("Couldn't set TCP server socket descriptor!"));
		return abortStart();
	}

	int wsPort = -1;
#ifdef HAVE_WEBSOCKETS
	// Qt 5.12 deprecated setSocketDescriptor in favor of setNativeDescriptior,
	// but then Qt 6 flipped those deprecations the other way round.
#	if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0) &&                            \
		QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	auto setWebSocketDescriptor = &QWebSocketServer::setNativeDescriptor;
#	else
	auto setWebSocketDescriptor = &QWebSocketServer::setSocketDescriptor;
#	endif
	if(m_webSocketServer) {
	   if(!(m_webSocketServer->*setWebSocketDescriptor)(webSocketFd)) {
			m_sessions->config()->logger()->logMessage(
				Log()
					.about(Log::Level::Error, Log::Topic::Status)
					.message(
						"Couldn't set WebSocket server socket descriptor!"));
			return abortStart();
		}
		wsPort = m_webSocketServer->serverPort();
	}
#endif

	m_port = m_tcpServer->serverPort();
	updateInternalConfig();

	emit serverStarted();
	m_sessions->config()->logger()->logMessage(
		Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.message(
				wsPort == -1
					? QStringLiteral("Started listening on passed TCP socket "
									 "on port %1, WebSocket not passed")
						  .arg(m_port)
					: QStringLiteral("Started listening on passed TCP socket "
									 "on port %1, WebSocket port %2")
						  .arg(m_port)
						  .arg(wsPort)));

	return true;
}

/**
 * @brief Assign a recording file name to a new session
 *
 * The name is generated by replacing placeholders in the file name pattern.
 * If a file with the same name exists, a number is inserted just before the suffix.
 *
 * If the file name pattern points to a directory, the default pattern "%d %t session %i.dprec"
 * will be used.
 *
 * The following placeholders are supported:
 *
 *  ~/ - user's home directory (at the start of the pattern)
 *  %d - the current date (YYYY-MM-DD)
 *  %h - the current time (HH.MM.SS)
 *  %i - session ID
 *  %a - session alias (or ID if not assigned)
 *
 * @param session
 */
void MultiServer::assignRecording(Session *session)
{
	QString filename = m_recordingPath;

	if(filename.isEmpty())
		return;

	// Expand home directory
	if(filename.startsWith("~/")) {
		filename = QString(qgetenv("HOME")) + filename.mid(1);
	}

	// Use default file pattern if target is a directory
	QFileInfo fi(filename);
	if(fi.isDir()) {
		filename = QFileInfo(QDir(filename), "%d %t session %i.dprec").absoluteFilePath();
	}

	// Expand placeholders
	QDateTime now = QDateTime::currentDateTime();
	filename.replace("%d", now.toString("yyyy-MM-dd"));
	filename.replace("%t", now.toString("HH.mm.ss"));
	filename.replace("%i", session->id());
	filename.replace("%a", session->aliasOrId());

	fi = QFileInfo(filename);

	if(!fi.absoluteDir().mkpath(".")) {
		qWarning("Recording directory \"%s\" does not exist and cannot be created!", qPrintable(fi.absolutePath()));
	} else {
		session->setRecordingFile(fi.absoluteFilePath());
	}
}

/**
 * @brief Accept or reject new client connection
 */
void MultiServer::newTcpClient()
{
	QTcpSocket *tcpSocket = m_tcpServer->nextPendingConnection();
	m_sessions->config()->logger()->logMessage(
		Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.user(0, tcpSocket->peerAddress(), QString())
			.message(QStringLiteral("New TCP client connected")));
	newClient(new ThinServerClient(tcpSocket, m_sessions->config()->logger()));
}

#ifdef HAVE_WEBSOCKETS
void MultiServer::newWebSocketClient()
{
	QWebSocket *webSocket = m_webSocketServer->nextPendingConnection();

	QHostAddress ip;
	QString ipSource;
	QHostAddress peerAddress = webSocket->peerAddress();
	if(webSocket->request().hasRawHeader("X-Real-IP") &&
	   ip.setAddress(
		   QString::fromUtf8(webSocket->request().rawHeader("X-Real-IP")))) {
		ipSource = QStringLiteral("X-Real-IP header");
	} else {
		ip = peerAddress;
		ipSource = QStringLiteral("WebSocket peer address");
	}

	m_sessions->config()->logger()->logMessage(
		Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.user(0, ip, QString())
			.message(QStringLiteral(
						 "New WebSocket client connected from %1 (IP from %2)")
						 .arg(peerAddress.toString(), ipSource)));
	newClient(
		new ThinServerClient(webSocket, ip, m_sessions->config()->logger()));
}
#endif

void MultiServer::newClient(ThinServerClient *client)
{
	client->applyBan(m_config->isAddressBanned(client->peerAddress()));
	m_sessions->addClient(client);
	printStatusUpdate();
}

void MultiServer::printStatusUpdate()
{
	initsys::notifyStatus(QString("%1 users and %2 sessions")
		.arg(m_sessions->totalUsers())
		.arg(m_sessions->sessionCount())
	);
}

/**
 * @brief Stop the server if vacant (and autostop is enabled)
 */
void MultiServer::tryAutoStop()
{
	if(m_state == RUNNING && m_autoStop && m_sessions->sessionCount() == 0 && m_sessions->totalUsers() == 0) {
		m_sessions->config()->logger()->logMessage(Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.message("Autostopping due to lack of sessions."));
		stop();
	}
}

/**
 * Disconnect all clients and stop listening.
 */
void MultiServer::stop() {
	if(m_state == RUNNING) {
		m_sessions->config()->logger()->logMessage(Log()
			.about(Log::Level::Info, Log::Topic::Status)
			.message(QString("Stopping server and kicking out %1 users...")
					 .arg(m_sessions->totalUsers())
			));

		m_state = STOPPING;
		m_tcpServer->close();
#ifdef HAVE_WEBSOCKETS
		if(m_webSocketServer) {
			m_webSocketServer->close();
		}
#endif
		m_port = 0;

		m_sessions->stopAll();
	}

	if(m_state == STOPPING) {
		if(m_sessions->totalUsers() == 0) {
			m_state = STOPPED;
			delete m_tcpServer;
			m_tcpServer = nullptr;
#ifdef HAVE_WEBSOCKETS
			delete m_webSocketServer;
			m_webSocketServer = nullptr;
#endif
			m_sessions->config()->logger()->logMessage(Log()
				.about(Log::Level::Info, Log::Topic::Status)
				.message("Server stopped."));
			emit serverStopped();
		}
	}
}

JsonApiResult MultiServer::callJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	QString head;
	QStringList tail;
	std::tie(head, tail) = popApiPath(path);

	if(head == "server")
		return serverJsonApi(method, tail, request);
	else if(head == "status")
		return statusJsonApi(method, tail, request);
	else if(head == "sessions")
		return m_sessions->callSessionJsonApi(method, tail, request);
	else if(head == "users")
		return m_sessions->callUserJsonApi(method, tail, request);
	else if(head == "banlist")
		return banlistJsonApi(method, tail, request);
	else if(head == "systembans")
		return systembansJsonApi(method, tail, request);
	else if(head == "userbans")
		return userbansJsonApi(method, tail, request);
	else if(head == "listserverwhitelist")
		return listserverWhitelistJsonApi(method, tail, request);
	else if(head == "accounts")
		return accountsJsonApi(method, tail, request);
	else if(head == "log")
		return logJsonApi(method, tail, request);
	else if(head == "extbans")
		return extbansJsonApi(method, tail, request);

	return JsonApiNotFound();
}

void MultiServer::callJsonApiAsync(const QString &requestId, JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	JsonApiResult result = callJsonApi(method, path, request);
	emit jsonApiResult(requestId, result);
}

/**
 * @brief Serverwide settings
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::serverJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method != JsonApiMethod::Get && method != JsonApiMethod::Update)
		return JsonApiBadMethod();

	const ConfigKey settings[] = {
		config::ClientTimeout,
		config::SessionSizeLimit,
		config::AutoresetThreshold,
		config::SessionCountLimit,
		config::EnablePersistence,
		config::ArchiveMode,
		config::IdleTimeLimit,
		config::ServerTitle,
		config::WelcomeMessage,
		config::PrivateUserList,
		config::AllowGuestHosts,
		config::AllowGuests,
#ifdef HAVE_LIBSODIUM
		config::UseExtAuth,
		config::ExtAuthKey,
		config::ExtAuthGroup,
		config::ExtAuthFallback,
		config::ExtAuthMod,
		config::ExtAuthHost,
		config::ExtAuthAvatars,
		config::ExtAuthBanExempt,
		config::ExtAuthGhosts,
		config::ExtAuthPersist,
#endif
		config::LogPurgeDays,
		config::AllowCustomAvatars,
		config::AbuseReport,
		config::ReportToken,
		config::ForceNsfm,
		config::ExtBansUrl,
		config::ExtBansCheckInterval,
		config::AllowIdleOverride,
		config::LoginInfoUrl,
		config::EnableGhosts,
		config::RuleText,
		config::MinimumProtocolVersion,
		config::MandatoryLookup,
#ifdef HAVE_WEBSOCKETS
		config::AllowGuestWeb,
		config::AllowGuestWebSession,
#	ifdef HAVE_LIBSODIUM
		config::ExtAuthWeb,
		config::ExtAuthWebSession,
#	endif
		config::PasswordDependentWebSession,
#endif
		config::SessionUserLimit,
		config::EmptySessionLingerTime,
	};
	const int settingCount = sizeof(settings) / sizeof(settings[0]);

	if(method==JsonApiMethod::Update) {
		for(int i=0;i<settingCount;++i) {
			if(request.contains(settings[i].name)) {
				m_config->setConfigString(settings[i], request[settings[i].name].toVariant().toString());
			}
		}
	}

	QJsonObject result;
	for(int i=0;i<settingCount;++i) {
		result[settings[i].name] = QJsonValue::fromVariant(m_config->getConfigVariant(settings[i]));
	}

	// Hide values for disabled features
	if(!m_config->internalConfig().reportUrl.isValid())
		result.remove(config::AbuseReport.name);

	if(!m_config->internalConfig().extAuthUrl.isValid())
		result.remove(config::UseExtAuth.name);

#ifdef HAVE_WEBSOCKETS
	if(!m_config->internalConfig().webSocket) {
		result.remove(config::AllowGuestWeb.name);
		result.remove(config::ExtAuthWeb.name);
		result.remove(config::AllowGuestWebSession.name);
		result.remove(config::ExtAuthWebSession.name);
		result.remove(config::PasswordDependentWebSession.name);
	}
#endif

	return JsonApiResult { JsonApiResult::Ok, QJsonDocument(result) };
}

/**
 * @brief Read only view of server status
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::statusJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	Q_UNUSED(request);

	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method != JsonApiMethod::Get)
		return JsonApiBadMethod();

	QJsonObject result;
	result["started"] = m_started.toString("yyyy-MM-dd HH:mm:ss");
	result["sessions"] = m_sessions->sessionCount();
	result["maxSessions"] = m_config->getConfigInt(config::SessionCountLimit);
	result["users"] = m_sessions->totalUsers();
	QString localhost = m_config->internalConfig().localHostname;
	if(localhost.isEmpty())
		localhost = WhatIsMyIp::guessLocalAddress();
	result["ext_host"] = localhost;
	result["ext_port"] = m_config->internalConfig().getAnnouncePort();

	return JsonApiResult { JsonApiResult::Ok, QJsonDocument(result) };
}

/**
 * @brief View and modify the serverwide banlist
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::banlistJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	// Database is needed to manipulate the banlist
	Database *db = qobject_cast<Database*>(m_config);
	if(!db)
		return JsonApiNotFound();

	if(path.size()==1) {
		if(method != JsonApiMethod::Delete)
			return JsonApiBadMethod();
		if(db->deleteIpBan(path.at(0).toInt())) {
			QJsonObject body;
			body["status"] = "ok";
			body["deleted"] = path.at(0).toInt();
			return JsonApiResult {JsonApiResult::Ok, QJsonDocument(body)};
		} else
			return JsonApiNotFound();
	}

	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method == JsonApiMethod::Get) {
		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(db->getIpBanlist()) };

	} else if(method == JsonApiMethod::Create) {
		QHostAddress ip { request["ip"].toString() };
		if(ip.isNull())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Valid IP address required");
		int subnet = request["subnet"].toInt();
		QDateTime expiration = ServerConfig::parseDateTime(request["expires"].toString());
		if(expiration.isNull())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Valid expiration time required");
		QString comment = request["comment"].toString();

		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(db->addIpBan(ip, subnet, expiration, comment)) };

	} else
		return JsonApiBadMethod();
}

JsonApiResult MultiServer::systembansJsonApi(
	JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	Database *db = qobject_cast<Database *>(m_config);
	if(!db) {
		return JsonApiNotFound();
	}

	if(path.size() == 1) {
		if(method != JsonApiMethod::Delete) {
			return JsonApiBadMethod();
		} else if(db->deleteSystemBan(path.at(0).toInt())) {
			QJsonObject body;
			body["status"] = "ok";
			body["deleted"] = path.at(0).toInt();
			return JsonApiResult{JsonApiResult::Ok, QJsonDocument(body)};
		} else {
			return JsonApiNotFound();
		}
	}

	if(!path.isEmpty()) {
		return JsonApiNotFound();
	}

	if(method == JsonApiMethod::Get) {
		return JsonApiResult{
			JsonApiResult::Ok, QJsonDocument(db->getSystemBanlist())};

	} else if(method == JsonApiMethod::Create) {
		qDebug() << request;
		QString sid = request["sid"].toString();
		if(sid.isEmpty()) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "SID required");
		}

		QDateTime expiration =
			ServerConfig::parseDateTime(request["expires"].toString());
		if(expiration.isNull()) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "Valid expiration time required");
		}

		BanReaction reaction =
			ServerConfig::parseReaction(request["reaction"].toString());
		if(reaction == BanReaction::Unknown ||
		   reaction == BanReaction::NotBanned) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "Invalid reaction");
		}

		QString comment = request["comment"].toString();
		QString reason = request["reason"].toString();

		QJsonObject result =
			db->addSystemBan(sid, expiration, reaction, reason, comment);
		if(result.isEmpty()) {
			return JsonApiErrorResult(
				JsonApiResult::InternalError, "Database error");
		} else {
			return {JsonApiResult::Ok, QJsonDocument(result)};
		}
	} else {
		return JsonApiBadMethod();
	}
}

JsonApiResult MultiServer::userbansJsonApi(
	JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	Database *db = qobject_cast<Database *>(m_config);
	if(!db) {
		return JsonApiNotFound();
	}

	if(path.size() == 1) {
		if(method != JsonApiMethod::Delete) {
			return JsonApiBadMethod();
		} else if(db->deleteUserBan(path.at(0).toInt())) {
			QJsonObject body;
			body["status"] = "ok";
			body["deleted"] = path.at(0).toInt();
			return JsonApiResult{JsonApiResult::Ok, QJsonDocument(body)};
		} else {
			return JsonApiNotFound();
		}
	}

	if(!path.isEmpty()) {
		return JsonApiNotFound();
	}

	if(method == JsonApiMethod::Get) {
		return JsonApiResult{
			JsonApiResult::Ok, QJsonDocument(db->getUserBanlist())};

	} else if(method == JsonApiMethod::Create) {
		QJsonValue rawUserId = request["userId"];
		long long userId = rawUserId.isDouble() ? rawUserId.toDouble() : 0;
		if(userId <= 0) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "Valid user ID required");
		}

		QDateTime expiration =
			ServerConfig::parseDateTime(request["expires"].toString());
		if(expiration.isNull()) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "Valid expiration time required");
		}

		BanReaction reaction =
			ServerConfig::parseReaction(request["reaction"].toString());
		if(reaction == BanReaction::Unknown ||
		   reaction == BanReaction::NotBanned) {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, "Invalid reaction");
		}

		QString comment = request["comment"].toString();
		QString reason = request["reason"].toString();

		QJsonObject result =
			db->addUserBan(userId, expiration, reaction, reason, comment);
		if(result.isEmpty()) {
			return JsonApiErrorResult(
				JsonApiResult::InternalError, "Database error");
		} else {
			return {JsonApiResult::Ok, QJsonDocument(result)};
		}
	} else {
		return JsonApiBadMethod();
	}
}

/**
 * @brief View and modify the list server URL whitelist
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::listserverWhitelistJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	// Database is needed to manipulate the whitelist
	Database *db = qobject_cast<Database*>(m_config);
	if(!db)
		return JsonApiNotFound();

	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method == JsonApiMethod::Update) {
		QStringList whitelist;
		for(const auto &v : request["whitelist"].toArray()) {
			const QString str = v.toString();
			if(str.isEmpty())
				continue;

			const QRegularExpression re(str);
			if(!re.isValid())
				return JsonApiErrorResult(JsonApiResult::BadRequest, str + ": " + re.errorString());
			whitelist << str;
		}
		if(!request["enabled"].isUndefined())
			db->setConfigBool(config::AnnounceWhiteList, request["enabled"].toBool());
		if(!request["whitelist"].isUndefined())
			db->updateListServerWhitelist(whitelist);
	}

	const QJsonObject o {
		{"enabled", db->getConfigBool(config::AnnounceWhiteList)},
		{"whitelist", QJsonArray::fromStringList(db->listServerWhitelist())}
	};

	return JsonApiResult { JsonApiResult::Ok, QJsonDocument(o) };
}

/**
 * @brief View and modify registered user accounts
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::accountsJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	// Database is needed to manipulate account list
	Database *db = qobject_cast<Database*>(m_config);
	if(!db)
		return JsonApiNotFound();

	if(path.size()==1) {
		QString head = path.at(0);

		if (method == JsonApiMethod::Create) {
			if(head == "auth" && request.contains("username") && request.contains("password")) {
				RegisteredUser user = db->getUserAccount(request["username"].toString(), request["password"].toString());
				return JsonApiResult{
					JsonApiResult::Ok,
					QJsonDocument(QJsonObject{{"status", user.status}})};
			}

			return JsonApiNotFound();

		} else if(method == JsonApiMethod::Update) {
			QJsonObject o = db->updateAccount(head.toInt(), request);
			if(o.isEmpty())
				return JsonApiNotFound();
			return JsonApiResult {JsonApiResult::Ok, QJsonDocument(o)};

		} else if(method == JsonApiMethod::Delete) {
			if(db->deleteAccount(head.toInt())) {
				QJsonObject body;
				body["status"] = "ok";
				body["deleted"] = head.toInt();
				return JsonApiResult {JsonApiResult::Ok, QJsonDocument(body)};
			} else {
				return JsonApiNotFound();
			}
		} else {
			return JsonApiBadMethod();
		}
	}

	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method == JsonApiMethod::Get) {
		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(db->getAccountList()) };

	} else if(method == JsonApiMethod::Create) {
		QString username = request["username"].toString();
		QString password = request["password"].toString();
		bool locked = request["locked"].toBool();
		QString flags = request["flags"].toString();
		if(username.isEmpty())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Username required");
		if(password.isEmpty())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Password required");

		QJsonObject o = db->addAccount(username, password, locked, flags.split(','));
		if(o.isEmpty())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Error");
		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(o) };

	} else
		return JsonApiBadMethod();
}

JsonApiResult MultiServer::logJsonApi(
	JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	if(!path.isEmpty()) {
		return JsonApiNotFound();
	}

	if(method != JsonApiMethod::Get) {
		return JsonApiBadMethod();
	}

	ServerLogQuery q = m_config->logger()->query();
	q.page(parseRequestInt(request, QStringLiteral("page"), 0, 0), 100);

	if(request.contains(QStringLiteral("session"))) {
		q.session(request.value(QStringLiteral("session")).toString());
	}

	if(request.contains(QStringLiteral("user"))) {
		q.user(request.value(QStringLiteral("user")).toString());
	}

	if(request.contains(QStringLiteral("contains"))) {
		q.messageContains(request.value(QStringLiteral("contains")).toString());
	}

	if(request.contains(QStringLiteral("after"))) {
		QDateTime after = QDateTime::fromString(
			request.value(QStringLiteral("after")).toString(), Qt::ISODate);
		if(after.isValid()) {
			q.after(after);
		} else {
			return JsonApiErrorResult(
				JsonApiResult::BadRequest, QStringLiteral("Invalid timestamp"));
		}
	}

	QJsonArray out;
	for(const Log &log : q.omitSensitive(false).get()) {
		out.append(log.toJson());
	}

	return JsonApiResult{JsonApiResult::Ok, QJsonDocument(out)};
}

JsonApiResult MultiServer::extbansJsonApi(
	JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	Q_UNUSED(request);
	int pathLength = path.length();
	if(pathLength == 0) {
		if(method == JsonApiMethod::Get || method == JsonApiMethod::Delete) {
			if (method == JsonApiMethod::Delete) {
				m_config->setConfigString(config::ExtBansCacheUrl, QString());
				m_config->setConfigString(config::ExtBansCacheKey, QString());
				m_config->setExternalBans({});
			}
			QJsonObject out = {
				{QStringLiteral("config"),
				 QJsonObject{
					 {config::ExtBansUrl.name,
					  m_config->getConfigString(config::ExtBansUrl)},
					 {config::ExtBansCheckInterval.name,
					  m_config->getConfigTime(config::ExtBansCheckInterval)},
					 {config::ExtBansCacheUrl.name,
					  m_config->getConfigString(config::ExtBansCacheUrl)},
					 {config::ExtBansCacheKey.name,
					  m_config->getConfigString(config::ExtBansCacheKey)},
				 }},
				{QStringLiteral("bans"), m_config->getExternalBans()},
				{QStringLiteral("status"), m_extBans->status()},
			};
			return JsonApiResult{JsonApiResult::Ok, QJsonDocument(out)};
		} else {
			return JsonApiBadMethod();
		}

	} else if(pathLength == 1) {
		if(path[0] == QStringLiteral("refresh")) {
			if(method == JsonApiMethod::Create) {
				JsonApiResult::Status status;
				QString msg;
				switch(m_extBans->refreshNow()) {
				case ExtBans::RefreshResult::Ok:
					status = JsonApiResult::Ok;
					msg = QStringLiteral("refresh triggered");
					break;
				case ExtBans::RefreshResult::AlreadyInProgress:
					status = JsonApiResult::BadRequest;
					msg = QStringLiteral("refresh already in progress");
					break;
				case ExtBans::RefreshResult::NotActive:
					status = JsonApiResult::BadRequest;
					msg = QStringLiteral("external bans not active");
					break;
				default:
					status = JsonApiResult::InternalError;
					msg = QStringLiteral("internal error");
					break;
				}
				return JsonApiResult{
					status,
					QJsonDocument(QJsonObject{{QStringLiteral("msg"), msg}})};
			} else {
				return JsonApiBadMethod();
			}
		}

		bool ok;
		int id = path[0].toInt(&ok);
		if(ok) {
			if(method == JsonApiMethod::Update) {
				const QString enabledKey = QStringLiteral("enabled");
				if(request.contains(enabledKey)) {
					bool enabled = request[enabledKey].toBool();
					if(m_config->setExternalBanEnabled(id, enabled)) {
						return JsonApiResult{JsonApiResult::Ok, {}};
					} else {
						return JsonApiErrorResult(
							JsonApiResult::NotFound,
							QStringLiteral(
								"External ipban with id '%1' not found")
								.arg(id));
					}
				} else {
					return JsonApiErrorResult(
						JsonApiResult::BadRequest,
						QStringLiteral("Missing 'enabled' in request"));
				}
			} else {
				return JsonApiBadMethod();
			}
		}

		return JsonApiNotFound();

	} else {
		return JsonApiNotFound();
	}
}

}
