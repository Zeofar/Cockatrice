/***************************************************************************
 *   Copyright (C) 2008 by Max-Wilhelm Bruker   *
 *   brukie@laptop   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include <QtSql>
#include <QSettings>
#include <QDebug>
#include <iostream>
#include "servatrice.h"
#include "server_room.h"
#include "serversocketinterface.h"
#include "serversocketthread.h"
#include "isl_interface.h"
#include "server_logger.h"
#include "main.h"
#include "passwordhasher.h"
#include "decklist.h"
#include "pb/game_replay.pb.h"
#include "pb/event_replay_added.pb.h"
#include "pb/event_server_message.pb.h"
#include "pb/event_server_shutdown.pb.h"
#include "pb/event_connection_closed.pb.h"

void Servatrice_GameServer::incomingConnection(int socketDescriptor)
{
	if (threaded) {
		ServerSocketThread *sst = new ServerSocketThread(socketDescriptor, server, this);
		sst->start();
	} else {
		QTcpSocket *socket = new QTcpSocket;
		socket->setSocketDescriptor(socketDescriptor);
		ServerSocketInterface *ssi = new ServerSocketInterface(server, socket);
		logger->logMessage(QString("incoming connection: %1").arg(socket->peerAddress().toString()), ssi);
	}
}

void Servatrice_IslServer::incomingConnection(int socketDescriptor)
{
	QThread *thread = new QThread;
	connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
	
	IslInterface *interface = new IslInterface(socketDescriptor, cert, privateKey, server);
	interface->moveToThread(thread);
	connect(interface, SIGNAL(destroyed()), thread, SLOT(quit()));
	
	thread->start();
	QMetaObject::invokeMethod(interface, "initServer", Qt::QueuedConnection);
}

Servatrice::Servatrice(QSettings *_settings, QObject *parent)
	: Server(parent), dbMutex(QMutex::Recursive), settings(_settings), uptime(0), shutdownTimer(0)
{
	serverName = settings->value("server/name").toString();
	serverId = settings->value("server/id", 0).toInt();
	
	const QString authenticationMethodStr = settings->value("authentication/method").toString();
	if (authenticationMethodStr == "sql")
		authenticationMethod = AuthenticationSql;
	else
		authenticationMethod = AuthenticationNone;
	
	QString dbTypeStr = settings->value("database/type").toString();
	if (dbTypeStr == "mysql")
		databaseType = DatabaseMySql;
	else
		databaseType = DatabaseNone;
	dbPrefix = settings->value("database/prefix").toString();
	if (databaseType != DatabaseNone)
		openDatabase();
	
	updateServerList();
	clearSessionTables();
	
	const QString roomMethod = settings->value("rooms/method").toString();
	if (roomMethod == "sql") {
		QSqlQuery query;
		query.prepare("select id, name, descr, auto_join, join_message from " + dbPrefix + "_rooms order by id asc");
		execSqlQuery(query);
		while (query.next()) {
			QSqlQuery query2;
			query2.prepare("select name from " + dbPrefix + "_rooms_gametypes where id_room = :id_room");
			query2.bindValue(":id_room", query.value(0).toInt());
			execSqlQuery(query2);
			QStringList gameTypes;
			while (query2.next())
				gameTypes.append(query2.value(0).toString());
			
			addRoom(new Server_Room(query.value(0).toInt(),
			                        query.value(1).toString(),
			                        query.value(2).toString(),
			                        query.value(3).toInt(),
			                        query.value(4).toString(),
			                        gameTypes,
			                        this
			));
		}
	} else {
		int size = settings->beginReadArray("rooms/roomlist");
		for (int i = 0; i < size; ++i) {
		  	settings->setArrayIndex(i);
			
			QStringList gameTypes;
			int size2 = settings->beginReadArray("game_types");
				for (int j = 0; j < size2; ++j) {
				settings->setArrayIndex(j);
				gameTypes.append(settings->value("name").toString());
			}
			settings->endArray();
				
			Server_Room *newRoom = new Server_Room(
				i,
				settings->value("name").toString(),
				settings->value("description").toString(),
				settings->value("autojoin").toBool(),
				settings->value("joinmessage").toString(),
				gameTypes,
				this
			);
			addRoom(newRoom);
		}
		settings->endArray();
	}
	
	updateLoginMessage();
	
	maxGameInactivityTime = settings->value("game/max_game_inactivity_time").toInt();
	maxPlayerInactivityTime = settings->value("game/max_player_inactivity_time").toInt();
	
	maxUsersPerAddress = settings->value("security/max_users_per_address").toInt();
	messageCountingInterval = settings->value("security/message_counting_interval").toInt();
	maxMessageCountPerInterval = settings->value("security/max_message_count_per_interval").toInt();
	maxMessageSizePerInterval = settings->value("security/max_message_size_per_interval").toInt();
	maxGamesPerUser = settings->value("security/max_games_per_user").toInt();

	try { if (settings->value("servernetwork/active", 0).toInt()) {
		qDebug() << "Connecting to ISL network.";
		const QString certFileName = settings->value("servernetwork/ssl_cert").toString();
		const QString keyFileName = settings->value("servernetwork/ssl_key").toString();
		qDebug() << "Loading certificate...";
		QFile certFile(certFileName);
		if (!certFile.open(QIODevice::ReadOnly))
			throw QString("Error opening certificate file: %1").arg(certFileName);
		QSslCertificate cert(&certFile);
		if (!cert.isValid())
			throw(QString("Invalid certificate."));
		qDebug() << "Loading private key...";
		QFile keyFile(keyFileName);
		if (!keyFile.open(QIODevice::ReadOnly))
			throw QString("Error opening private key file: %1").arg(keyFileName);
		QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
		if (key.isNull())
			throw QString("Invalid private key.");
		
		QMutableListIterator<ServerProperties> serverIterator(serverList);
		while (serverIterator.hasNext()) {
			const ServerProperties &prop = serverIterator.next();
			if (prop.cert == cert) {
				serverIterator.remove();
				continue;
			}
			
			QThread *thread = new QThread;
			connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
			
			IslInterface *interface = new IslInterface(prop.id, prop.hostname, prop.address.toString(), prop.controlPort, prop.cert, cert, key, this);
			interface->moveToThread(thread);
			connect(interface, SIGNAL(destroyed()), thread, SLOT(quit()));
			
			thread->start();
			QMetaObject::invokeMethod(interface, "initClient", Qt::BlockingQueuedConnection);
		}
			
		const int networkPort = settings->value("servernetwork/port", 14747).toInt();
		qDebug() << "Starting ISL server on port" << networkPort;
		
		islServer = new Servatrice_IslServer(this, cert, key, this);
		if (islServer->listen(QHostAddress::Any, networkPort))
			qDebug() << "ISL server listening.";
		else
			throw QString("islServer->listen(): Error.");
		
	} } catch (QString error) {
		qDebug() << "ERROR --" << error;
	}
	
	pingClock = new QTimer(this);
	connect(pingClock, SIGNAL(timeout()), this, SIGNAL(pingClockTimeout()));
	pingClock->start(1000);
	
	int statusUpdateTime = settings->value("server/statusupdate").toInt();
	statusUpdateClock = new QTimer(this);
	connect(statusUpdateClock, SIGNAL(timeout()), this, SLOT(statusUpdate()));
	if (statusUpdateTime != 0) {
		qDebug() << "Starting status update clock, interval " << statusUpdateTime << " ms";
		statusUpdateClock->start(statusUpdateTime);
	}
	
	threaded = settings->value("server/threaded", false).toInt();
	gameServer = new Servatrice_GameServer(this, threaded, this);
	const int gamePort = settings->value("server/port", 4747).toInt();
	qDebug() << "Starting server on port" << gamePort;
	if (gameServer->listen(QHostAddress::Any, gamePort))
		qDebug() << "Server listening.";
	else
		qDebug() << "gameServer->listen(): Error.";
	
}

Servatrice::~Servatrice()
{
	prepareDestroy();
	QSqlDatabase::database().close();
}

bool Servatrice::openDatabase()
{
	if (!QSqlDatabase::connectionNames().isEmpty())
		QSqlDatabase::removeDatabase(QSqlDatabase::database().connectionNames().at(0));
	
	settings->beginGroup("database");
	QSqlDatabase sqldb = QSqlDatabase::addDatabase("QMYSQL");
	sqldb.setHostName(settings->value("hostname").toString());
	sqldb.setDatabaseName(settings->value("database").toString());
	sqldb.setUserName(settings->value("user").toString());
	sqldb.setPassword(settings->value("password").toString());
	settings->endGroup();
	
	std::cerr << "Opening database...";
	if (!sqldb.open()) {
		std::cerr << "error" << std::endl;
		return false;
	}
	std::cerr << "OK" << std::endl;
	
	return true;
}

bool Servatrice::checkSql()
{
	if (databaseType == DatabaseNone)
		return false;
	
	QMutexLocker locker(&dbMutex);
	if (!QSqlDatabase::database().exec("select 1").isActive())
		return openDatabase();
	return true;
}

bool Servatrice::execSqlQuery(QSqlQuery &query)
{
	if (query.exec())
		return true;
	qCritical() << "Database error:" << query.lastError().text();
	return false;
}

void Servatrice::updateServerList()
{
	qDebug() << "Updating server list...";
	
	serverListMutex.lock();
	serverList.clear();
	
	QSqlQuery query;
	query.prepare("select id, ssl_cert, hostname, address, game_port, control_port from " + dbPrefix + "_servers order by id asc");
	execSqlQuery(query);
	while (query.next()) {
		ServerProperties prop(query.value(0).toInt(), QSslCertificate(query.value(1).toString().toAscii()), query.value(2).toString(), QHostAddress(query.value(3).toString()), query.value(4).toInt(), query.value(5).toInt());
		serverList.append(prop);
		qDebug() << QString("#%1 CERT=%2 NAME=%3 IP=%4:%5 CPORT=%6").arg(prop.id).arg(QString(prop.cert.digest().toHex())).arg(prop.hostname).arg(prop.address.toString()).arg(prop.gamePort).arg(prop.controlPort);
	}
	
	serverListMutex.unlock();
}

QList<ServerProperties> Servatrice::getServerList() const
{
	serverListMutex.lock();
	QList<ServerProperties> result = serverList;
	serverListMutex.unlock();
	
	return result;
}

AuthenticationResult Servatrice::checkUserPassword(Server_ProtocolHandler *handler, const QString &user, const QString &password, QString &reasonStr)
{
	QMutexLocker locker(&dbMutex);
	const QString method = settings->value("authentication/method").toString();
	switch (authenticationMethod) {
	case AuthenticationNone: return UnknownUser;
	case AuthenticationSql: {
		if (!checkSql())
			return UnknownUser;
		
		QSqlQuery ipBanQuery;
		ipBanQuery.prepare("select time_to_sec(timediff(now(), date_add(b.time_from, interval b.minutes minute))) < 0, b.minutes <=> 0, b.visible_reason from " + dbPrefix + "_bans b where b.time_from = (select max(c.time_from) from " + dbPrefix + "_bans c where c.ip_address = :address) and b.ip_address = :address2");
		ipBanQuery.bindValue(":address", static_cast<ServerSocketInterface *>(handler)->getPeerAddress().toString());
		ipBanQuery.bindValue(":address2", static_cast<ServerSocketInterface *>(handler)->getPeerAddress().toString());
		if (!execSqlQuery(ipBanQuery)) {
			qDebug("Login denied: SQL error");
			return NotLoggedIn;
		}
		
		if (ipBanQuery.next())
			if (ipBanQuery.value(0).toInt() || ipBanQuery.value(1).toInt()) {
				reasonStr = ipBanQuery.value(2).toString();
				qDebug("Login denied: banned by address");
				return UserIsBanned;
			}
		
		QSqlQuery nameBanQuery;
		nameBanQuery.prepare("select time_to_sec(timediff(now(), date_add(b.time_from, interval b.minutes minute))) < 0, b.minutes <=> 0, b.visible_reason from " + dbPrefix + "_bans b where b.time_from = (select max(c.time_from) from " + dbPrefix + "_bans c where c.user_name = :name2) and b.user_name = :name1");
		nameBanQuery.bindValue(":name1", user);
		nameBanQuery.bindValue(":name2", user);
		if (!execSqlQuery(nameBanQuery)) {
			qDebug("Login denied: SQL error");
			return NotLoggedIn;
		}
		
		if (nameBanQuery.next())
			if (nameBanQuery.value(0).toInt() || nameBanQuery.value(1).toInt()) {
				reasonStr = nameBanQuery.value(2).toString();
				qDebug("Login denied: banned by name");
				return UserIsBanned;
			}
		
		QSqlQuery passwordQuery;
		passwordQuery.prepare("select password_sha512 from " + dbPrefix + "_users where name = :name and active = 1");
		passwordQuery.bindValue(":name", user);
		if (!execSqlQuery(passwordQuery)) {
			qDebug("Login denied: SQL error");
			return NotLoggedIn;
		}
		
		if (passwordQuery.next()) {
			const QString correctPassword = passwordQuery.value(0).toString();
			if (correctPassword == PasswordHasher::computeHash(password, correctPassword.left(16))) {
				qDebug("Login accepted: password right");
				return PasswordRight;
			} else {
				qDebug("Login denied: password wrong");
				return NotLoggedIn;
			}
		} else {
			qDebug("Login accepted: unknown user");
			return UnknownUser;
		}
	}
	}
	return UnknownUser;
}

bool Servatrice::userExists(const QString &user)
{
	if (authenticationMethod == AuthenticationSql) {
		QMutexLocker locker(&dbMutex);
		checkSql();
	
		QSqlQuery query;
		query.prepare("select 1 from " + dbPrefix + "_users where name = :name and active = 1");
		query.bindValue(":name", user);
		if (!execSqlQuery(query))
			return false;
		return query.next();
	}
	return false;
}

int Servatrice::getUserIdInDB(const QString &name)
{
	if (authenticationMethod == AuthenticationSql) {
		QMutexLocker locker(&dbMutex);
		QSqlQuery query;
		query.prepare("select id from " + dbPrefix + "_users where name = :name and active = 1");
		query.bindValue(":name", name);
		if (!execSqlQuery(query))
			return -1;
		if (!query.next())
			return -1;
		return query.value(0).toInt();
	}
	return -1;
}

bool Servatrice::isInBuddyList(const QString &whoseList, const QString &who)
{
	if (authenticationMethod == AuthenticationNone)
		return false;
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return false;
	
	int id1 = getUserIdInDB(whoseList);
	int id2 = getUserIdInDB(who);
	
	QSqlQuery query;
	query.prepare("select 1 from " + dbPrefix + "_buddylist where id_user1 = :id_user1 and id_user2 = :id_user2");
	query.bindValue(":id_user1", id1);
	query.bindValue(":id_user2", id2);
	if (!execSqlQuery(query))
		return false;
	return query.next();
}

bool Servatrice::isInIgnoreList(const QString &whoseList, const QString &who)
{
	if (authenticationMethod == AuthenticationNone)
		return false;
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return false;
	
	int id1 = getUserIdInDB(whoseList);
	int id2 = getUserIdInDB(who);
	
	QSqlQuery query;
	query.prepare("select 1 from " + dbPrefix + "_ignorelist where id_user1 = :id_user1 and id_user2 = :id_user2");
	query.bindValue(":id_user1", id1);
	query.bindValue(":id_user2", id2);
	if (!execSqlQuery(query))
		return false;
	return query.next();
}

ServerInfo_User Servatrice::evalUserQueryResult(const QSqlQuery &query, bool complete, bool withId)
{
	ServerInfo_User result;
	
	if (withId)
		result.set_id(query.value(0).toInt());
	result.set_name(query.value(1).toString().toStdString());
	
	const QString country = query.value(5).toString();
	if (!country.isEmpty())
		result.set_country(country.toStdString());
	
	if (complete) {
		const QByteArray avatarBmp = query.value(6).toByteArray();
		if (avatarBmp.size())
			result.set_avatar_bmp(avatarBmp.data(), avatarBmp.size());
	}
	
	const QString genderStr = query.value(4).toString();
	if (genderStr == "m")
		result.set_gender(ServerInfo_User::Male);
	else if (genderStr == "f")
		result.set_gender(ServerInfo_User::Female);
	
	const int is_admin = query.value(2).toInt();
	int userLevel = ServerInfo_User::IsUser | ServerInfo_User::IsRegistered;
	if (is_admin == 1)
		userLevel |= ServerInfo_User::IsAdmin | ServerInfo_User::IsModerator;
	else if (is_admin == 2)
		userLevel |= ServerInfo_User::IsModerator;
	result.set_user_level(userLevel);
	
	const QString realName = query.value(3).toString();
	if (!realName.isEmpty())
		result.set_real_name(realName.toStdString());
	
	return result;
}

ServerInfo_User Servatrice::getUserData(const QString &name, bool withId)
{
	ServerInfo_User result;
	result.set_name(name.toStdString());
	result.set_user_level(ServerInfo_User::IsUser);
	
	if (authenticationMethod == AuthenticationSql) {
		QMutexLocker locker(&dbMutex);
		if (!checkSql())
			return result;
		
		QSqlQuery query;
		query.prepare("select id, name, admin, realname, gender, country, avatar_bmp from " + dbPrefix + "_users where name = :name and active = 1");
		query.bindValue(":name", name);
		if (!execSqlQuery(query))
			return result;
		
		if (query.next())
			return evalUserQueryResult(query, true, withId);
		else
			return result;
	} else
		return result;
}

int Servatrice::getUsersWithAddress(const QHostAddress &address) const
{
	int result = 0;
	QReadLocker locker(&clientsLock);
	for (int i = 0; i < clients.size(); ++i)
		if (static_cast<ServerSocketInterface *>(clients[i])->getPeerAddress() == address)
			++result;
	return result;
}

QList<ServerSocketInterface *> Servatrice::getUsersWithAddressAsList(const QHostAddress &address) const
{
	QList<ServerSocketInterface *> result;
	QReadLocker locker(&clientsLock);
	for (int i = 0; i < clients.size(); ++i)
		if (static_cast<ServerSocketInterface *>(clients[i])->getPeerAddress() == address)
			result.append(static_cast<ServerSocketInterface *>(clients[i]));
	return result;
}

void Servatrice::clearSessionTables()
{
	qDebug() << "Clearing previous sessions...";
	
	lockSessionTables();
	QSqlQuery query;
	query.prepare("update " + dbPrefix + "_sessions set end_time=now() where end_time is null and id_server = :id_server");
	query.bindValue(":id_server", serverId);
	query.exec();
	unlockSessionTables();
}

void Servatrice::lockSessionTables()
{
	QSqlQuery("lock tables " + dbPrefix + "_sessions write, " + dbPrefix + "_users read").exec();
}

void Servatrice::unlockSessionTables()
{
	QSqlQuery("unlock tables").exec();
}

bool Servatrice::userSessionExists(const QString &userName)
{
	// Call only after lockSessionTables().
	
	QSqlQuery query;
	query.prepare("select 1 from " + dbPrefix + "_sessions where user_name = :user_name and end_time is null");
	query.bindValue(":user_name", userName);
	query.exec();
	return query.next();
}

qint64 Servatrice::startSession(const QString &userName, const QString &address)
{
	if (authenticationMethod == AuthenticationNone)
		return -1;
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return -1;
	
	QSqlQuery query;
	query.prepare("insert into " + dbPrefix + "_sessions (user_name, id_server, ip_address, start_time) values(:user_name, :id_server, :ip_address, NOW())");
	query.bindValue(":user_name", userName);
	query.bindValue(":id_server", serverId);
	query.bindValue(":ip_address", address);
	if (execSqlQuery(query))
		return query.lastInsertId().toInt();
	return -1;
}

void Servatrice::endSession(qint64 sessionId)
{
	if (authenticationMethod == AuthenticationNone)
		return;
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return;
	
	QSqlQuery query;
	query.exec("lock tables " + dbPrefix + "_sessions write");
	query.prepare("update " + dbPrefix + "_sessions set end_time=NOW() where id = :id_session");
	query.bindValue(":id_session", sessionId);
	execSqlQuery(query);
	query.exec("unlock tables");
}

QMap<QString, ServerInfo_User> Servatrice::getBuddyList(const QString &name)
{
	QMap<QString, ServerInfo_User> result;
	
	if (authenticationMethod == AuthenticationSql) {
		QMutexLocker locker(&dbMutex);
		checkSql();

		QSqlQuery query;
		query.prepare("select a.id, a.name, a.admin, a.realname, a.gender, a.country from " + dbPrefix + "_users a left join " + dbPrefix + "_buddylist b on a.id = b.id_user2 left join " + dbPrefix + "_users c on b.id_user1 = c.id where c.name = :name");
		query.bindValue(":name", name);
		if (!execSqlQuery(query))
			return result;
		
		while (query.next()) {
			const ServerInfo_User &temp = evalUserQueryResult(query, false);
			result.insert(QString::fromStdString(temp.name()), temp);
		}
	}
	return result;
}

QMap<QString, ServerInfo_User> Servatrice::getIgnoreList(const QString &name)
{
	QMap<QString, ServerInfo_User> result;
	
	if (authenticationMethod == AuthenticationSql) {
		QMutexLocker locker(&dbMutex);
		checkSql();

		QSqlQuery query;
		query.prepare("select a.id, a.name, a.admin, a.realname, a.gender, a.country from " + dbPrefix + "_users a left join " + dbPrefix + "_ignorelist b on a.id = b.id_user2 left join " + dbPrefix + "_users c on b.id_user1 = c.id where c.name = :name");
		query.bindValue(":name", name);
		if (!execSqlQuery(query))
			return result;
		
		while (query.next()) {
			ServerInfo_User temp = evalUserQueryResult(query, false);
			result.insert(QString::fromStdString(temp.name()), temp);
		}
	}
	return result;
}

void Servatrice::updateLoginMessage()
{
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return;
	
	QSqlQuery query;
	query.prepare("select message from " + dbPrefix + "_servermessages where id_server = :id_server order by timest desc limit 1");
	query.bindValue(":id_server", serverId);
	if (execSqlQuery(query))
		if (query.next()) {
			loginMessage = query.value(0).toString();
			
			Event_ServerMessage event;
			event.set_message(loginMessage.toStdString());
			SessionEvent *se = Server_ProtocolHandler::prepareSessionEvent(event);
			QMapIterator<QString, Server_ProtocolHandler *> usersIterator(users);
			while (usersIterator.hasNext())
				usersIterator.next().value()->sendProtocolItem(*se);
			delete se;
		}
}

void Servatrice::statusUpdate()
{
	const int uc = getUsersCount(); // for correct mutex locking order
	const int gc = getGamesCount();
	
	uptime += statusUpdateClock->interval() / 1000;
	
	txBytesMutex.lock();
	quint64 tx = txBytes;
	txBytes = 0;
	txBytesMutex.unlock();
	rxBytesMutex.lock();
	quint64 rx = rxBytes;
	rxBytes = 0;
	rxBytesMutex.unlock();
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return;
	
	QSqlQuery query;
	query.prepare("insert into " + dbPrefix + "_uptime (id_server, timest, uptime, users_count, games_count, tx_bytes, rx_bytes) values(:id, NOW(), :uptime, :users_count, :games_count, :tx, :rx)");
	query.bindValue(":id", serverId);
	query.bindValue(":uptime", uptime);
	query.bindValue(":users_count", uc);
	query.bindValue(":games_count", gc);
	query.bindValue(":tx", tx);
	query.bindValue(":rx", rx);
	execSqlQuery(query);
}

int Servatrice::getNextGameId()
{
	if (databaseType == DatabaseNone)
		return Server::getNextGameId();
	
	checkSql();
	
	QSqlQuery query;
	query.prepare("insert into " + dbPrefix + "_games (time_started) values (now())");
	execSqlQuery(query);
	
	return query.lastInsertId().toInt();
}

int Servatrice::getNextReplayId()
{
	if (databaseType == DatabaseNone)
		return Server::getNextGameId();
	
	checkSql();
	
	QSqlQuery query;
	query.prepare("insert into " + dbPrefix + "_replays () values ()");
	execSqlQuery(query);
	
	return query.lastInsertId().toInt();
}

void Servatrice::storeGameInformation(int secondsElapsed, const QSet<QString> &allPlayersEver, const QSet<QString> &allSpectatorsEver, const QList<GameReplay *> &replayList)
{
	const ServerInfo_Game &gameInfo = replayList.first()->game_info();
	
	Server_Room *room = rooms.value(gameInfo.room_id());
	
	Event_ReplayAdded replayEvent;
	ServerInfo_ReplayMatch *replayMatchInfo = replayEvent.mutable_match_info();
	replayMatchInfo->set_game_id(gameInfo.game_id());
	replayMatchInfo->set_room_name(room->getName().toStdString());
	replayMatchInfo->set_time_started(QDateTime::currentDateTime().addSecs(-secondsElapsed).toTime_t());
	replayMatchInfo->set_length(secondsElapsed);
	replayMatchInfo->set_game_name(gameInfo.description());
	
	const QStringList &allGameTypes = room->getGameTypes();
	QStringList gameTypes;
	for (int i = gameInfo.game_types_size() - 1; i >= 0; --i)
		gameTypes.append(allGameTypes[gameInfo.game_types(i)]);
	
	QVariantList gameIds1, playerNames, gameIds2, userIds, replayNames;
	QSetIterator<QString> playerIterator(allPlayersEver);
	while (playerIterator.hasNext()) {
		gameIds1.append(gameInfo.game_id());
		const QString &playerName = playerIterator.next();
		playerNames.append(playerName);
		replayMatchInfo->add_player_names(playerName.toStdString());
	}
	QSet<QString> allUsersInGame = allPlayersEver + allSpectatorsEver;
	QSetIterator<QString> allUsersIterator(allUsersInGame);
	while (allUsersIterator.hasNext()) {
		int id = getUserIdInDB(allUsersIterator.next());
		if (id == -1)
			continue;
		gameIds2.append(gameInfo.game_id());
		userIds.append(id);
		replayNames.append(QString::fromStdString(gameInfo.description()));
	}
	
	QVariantList replayIds, replayGameIds, replayDurations, replayBlobs;
	for (int i = 0; i < replayList.size(); ++i) {
		QByteArray blob;
		const unsigned int size = replayList[i]->ByteSize();
		blob.resize(size);
		replayList[i]->SerializeToArray(blob.data(), size);
		
		replayIds.append(QVariant((qulonglong) replayList[i]->replay_id()));
		replayGameIds.append(gameInfo.game_id());
		replayDurations.append(replayList[i]->duration_seconds());
		replayBlobs.append(blob);
		
		ServerInfo_Replay *replayInfo = replayMatchInfo->add_replay_list();
		replayInfo->set_replay_id(replayList[i]->replay_id());
		replayInfo->set_replay_name(gameInfo.description());
		replayInfo->set_duration(replayList[i]->duration_seconds());
	}
	
	SessionEvent *sessionEvent = Server_ProtocolHandler::prepareSessionEvent(replayEvent);
	allUsersIterator.toFront();
	clientsLock.lockForRead();
	while (allUsersIterator.hasNext()) {
		const QString userName = allUsersIterator.next();
		Server_AbstractUserInterface *userHandler = users.value(userName);
		if (!userHandler)
			userHandler = externalUsers.value(userName);
		if (userHandler)
			userHandler->sendProtocolItem(*sessionEvent);
	}
	clientsLock.unlock();
	delete sessionEvent;
	
	QMutexLocker locker(&dbMutex);
	if (!checkSql())
		return;
	
	QSqlQuery query1;
	query1.prepare("update " + dbPrefix + "_games set room_name=:room_name, descr=:descr, creator_name=:creator_name, password=:password, game_types=:game_types, player_count=:player_count, time_finished=now() where id=:id_game");
	query1.bindValue(":room_name", room->getName());
	query1.bindValue(":id_game", gameInfo.game_id());
	query1.bindValue(":descr", QString::fromStdString(gameInfo.description()));
	query1.bindValue(":creator_name", QString::fromStdString(gameInfo.creator_info().name()));
	query1.bindValue(":password", gameInfo.with_password() ? 1 : 0);
	query1.bindValue(":game_types", gameTypes.isEmpty() ? QString("") : gameTypes.join(", "));
	query1.bindValue(":player_count", gameInfo.max_players());
	if (!execSqlQuery(query1))
		return;
	
	QSqlQuery query2;
	query2.prepare("insert into " + dbPrefix + "_games_players (id_game, player_name) values (:id_game, :player_name)");
	query2.bindValue(":id_game", gameIds1);
	query2.bindValue(":player_name", playerNames);
	query2.execBatch();
	
	QSqlQuery replayQuery1;
	replayQuery1.prepare("update " + dbPrefix + "_replays set id_game=:id_game, duration=:duration, replay=:replay where id=:id_replay");
	replayQuery1.bindValue(":id_replay", replayIds);
	replayQuery1.bindValue(":id_game", replayGameIds);
	replayQuery1.bindValue(":duration", replayDurations);
	replayQuery1.bindValue(":replay", replayBlobs);
	replayQuery1.execBatch();
	
	QSqlQuery query3;
	query3.prepare("insert into " + dbPrefix + "_replays_access (id_game, id_player, replay_name) values (:id_game, :id_player, :replay_name)");
	query3.bindValue(":id_game", gameIds2);
	query3.bindValue(":id_player", userIds);
	query3.bindValue(":replay_name", replayNames);
	query3.execBatch();
}

DeckList *Servatrice::getDeckFromDatabase(int deckId, const QString &userName)
{
	checkSql();
	
	QMutexLocker locker(&dbMutex);
	QSqlQuery query;
	
	query.prepare("select content from " + dbPrefix + "_decklist_files where id = :id and user = :user");
	query.bindValue(":id", deckId);
	query.bindValue(":user", userName);
	execSqlQuery(query);
	if (!query.next())
		throw Response::RespNameNotFound;
	
	QXmlStreamReader deckReader(query.value(0).toString());
	DeckList *deck = new DeckList;
	deck->loadFromXml(&deckReader);
	
	return deck;
}

void Servatrice::scheduleShutdown(const QString &reason, int minutes)
{
	shutdownReason = reason;
	shutdownMinutes = minutes + 1;
	if (minutes > 0) {
		shutdownTimer = new QTimer;
		connect(shutdownTimer, SIGNAL(timeout()), this, SLOT(shutdownTimeout()));
		shutdownTimer->start(60000);
	}
	shutdownTimeout();
}

void Servatrice::incTxBytes(quint64 num)
{
	txBytesMutex.lock();
	txBytes += num;
	txBytesMutex.unlock();
}

void Servatrice::incRxBytes(quint64 num)
{
	rxBytesMutex.lock();
	rxBytes += num;
	rxBytesMutex.unlock();
}

void Servatrice::shutdownTimeout()
{
	--shutdownMinutes;
	
	SessionEvent *se;
	if (shutdownMinutes) {
		Event_ServerShutdown event;
		event.set_reason(shutdownReason.toStdString());
		event.set_minutes(shutdownMinutes);
		se = Server_ProtocolHandler::prepareSessionEvent(event);
	} else {
		Event_ConnectionClosed event;
		event.set_reason(Event_ConnectionClosed::SERVER_SHUTDOWN);
		se = Server_ProtocolHandler::prepareSessionEvent(event);
	}
	
	clientsLock.lockForRead();
	for (int i = 0; i < clients.size(); ++i)
		clients[i]->sendProtocolItem(*se);
	clientsLock.unlock();
	delete se;
	
	if (!shutdownMinutes)
		deleteLater();
}

bool Servatrice::islConnectionExists(int serverId) const
{
	// Only call with islLock locked at least for reading
	
	return islInterfaces.contains(serverId);
}

void Servatrice::addIslInterface(int serverId, IslInterface *interface)
{
	// Only call with islLock locked for writing
	
	islInterfaces.insert(serverId, interface);
	connect(interface, SIGNAL(externalUserJoined(ServerInfo_User)), this, SLOT(externalUserJoined(ServerInfo_User)));
	connect(interface, SIGNAL(externalUserLeft(QString)), this, SLOT(externalUserLeft(QString)));
	connect(interface, SIGNAL(externalRoomUserJoined(int, ServerInfo_User)), this, SLOT(externalRoomUserJoined(int, ServerInfo_User)));
	connect(interface, SIGNAL(externalRoomUserLeft(int, QString)), this, SLOT(externalRoomUserLeft(int, QString)));
	connect(interface, SIGNAL(externalRoomSay(int, QString, QString)), this, SLOT(externalRoomSay(int, QString, QString)));
	connect(interface, SIGNAL(externalRoomGameListChanged(int, ServerInfo_Game)), this, SLOT(externalRoomGameListChanged(int, ServerInfo_Game)));
	connect(interface, SIGNAL(joinGameCommandReceived(Command_JoinGame, int, int, int, qint64)), this, SLOT(externalJoinGameCommandReceived(Command_JoinGame, int, int, int, qint64)));
	connect(interface, SIGNAL(gameCommandContainerReceived(CommandContainer, int, int, qint64)), this, SLOT(externalGameCommandContainerReceived(CommandContainer, int, int, qint64)));
	connect(interface, SIGNAL(responseReceived(Response, qint64)), this, SLOT(externalResponseReceived(Response, qint64)));
	connect(interface, SIGNAL(gameEventContainerReceived(GameEventContainer, qint64)), this, SLOT(externalGameEventContainerReceived(GameEventContainer, qint64)));
}

void Servatrice::removeIslInterface(int serverId)
{
	// Only call with islLock locked for writing
	
	// XXX we probably need to delete everything that belonged to it...
	islInterfaces.remove(serverId);
}

void Servatrice::doSendIslMessage(const IslMessage &msg, int serverId)
{
	QReadLocker locker(&islLock);
	
	if (serverId == -1) {
		QMapIterator<int, IslInterface *> islIterator(islInterfaces);
		while (islIterator.hasNext())
			islIterator.next().value()->transmitMessage(msg);
	} else {
		IslInterface *interface = islInterfaces.value(serverId);
		if (interface)
			interface->transmitMessage(msg);
	}
}
