// SPDX-License-Identifier: GPL-3.0-or-later
#include "thinsrv/dblog.h"
#include <QMetaEnum>
#include <QSqlError>
#include <QSqlQuery>

namespace server {

DbLog::DbLog(const QSqlDatabase &db)
	: m_db(db)
{
}

bool DbLog::initDb()
{
	QSqlQuery q(m_db);
	return q.exec(
		QStringLiteral("CREATE TABLE IF NOT EXISTS serverlog ("
					   "timestamp, level, topic, user, session, message)"));
}

QList<Log> DbLog::getLogEntries(
	const QString &session, const QDateTime &after, Log::Level atleast,
	bool omitSensitive, bool omitKicksAndBans, int offset, int limit) const
{
	QString sql = QStringLiteral(
		"SELECT timestamp, session, user, level, topic, message FROM "
		"serverlog WHERE 1 = 1");
	QVariantList params;
	if(!session.isEmpty()) {
		sql += QStringLiteral(" AND session = ?");
		params << session;
	}
	if(after.isValid()) {
		sql += QStringLiteral(" AND timestamp >= ?");
		params << after.addMSecs(1000).toString(Qt::ISODate);
	}

	if(atleast < Log::Level::Debug) {
		sql += QStringLiteral(" AND level <= ?");
		params << int(atleast);
	}

	if(omitSensitive) {
		sql += QStringLiteral(" AND topic <> 'ClientInfo'");
	}

	if(omitKicksAndBans) {
		sql += QStringLiteral(" AND topic NOT IN ('Kick', 'Ban', 'Unban')");
	}

	sql += QStringLiteral(" ORDER BY timestamp DESC, rowid DESC");

	if(limit > 0) {
		sql += QStringLiteral(" LIMIT ?");
		params << limit;
	}
	if(offset > 0) {
		sql += QStringLiteral(" OFFSET ?");
		params << offset;
	}

	QSqlQuery q(m_db);
	q.prepare(sql);
	for(int i = 0, count = params.size(); i < count; ++i) {
		q.bindValue(i, params[i]);
	}

	if(!q.exec()) {
		qDebug("exec: %s", qUtf8Printable(q.executedQuery()));
		qWarning(
			"Database log query error: %s",
			qUtf8Printable(q.lastError().text()));
	}

	QList<Log> results;
	while(q.next()) {
		results << Log(
			q.value(0).toDateTime(), q.value(1).toString(),
			q.value(2).toString(), Log::Level(q.value(3).toInt()),
			Log::Topic(QMetaEnum::fromType<Log::Topic>().keyToValue(
				q.value(4).toString().toLocal8Bit().constData())),
			q.value(5).toString());
	}
	return results;
}

void DbLog::storeMessage(const Log &entry)
{
	QSqlQuery q(m_db);
	q.prepare(
		QStringLiteral("INSERT INTO serverlog (timestamp, level, topic, user, "
					   "session, message) VALUES (?, ?, ?, ?, ?, ?)"));
	q.bindValue(0, entry.timestamp().toString(Qt::ISODate));
	q.bindValue(1, int(entry.level()));
	q.bindValue(
		2, QMetaEnum::fromType<Log::Topic>().valueToKey(int(entry.topic())));
	q.bindValue(3, entry.user());
	q.bindValue(4, entry.session());
	q.bindValue(5, entry.message());
	q.exec();
}

int DbLog::purgeLogs(int olderThanDays)
{
	if(olderThanDays <= 0) {
		return 0;
	}

	QSqlQuery q(m_db);
	q.prepare(QStringLiteral(
		"DELETE FROM serverlog WHERE timestamp < DATE('now', ?)"));
	q.bindValue(0, QStringLiteral("-%1 days").arg(olderThanDays));
	if(!q.exec()) {
		qWarning(
			"Couldn't purge log entries: %s",
			qUtf8Printable(q.lastError().databaseText()));
	}
	return q.numRowsAffected();
}

}
