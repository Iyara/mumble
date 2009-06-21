/* Copyright (C) 2005-2009, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "murmur_pch.h"

#include "User.h"
#include "Channel.h"
#include "ACL.h"
#include "Group.h"
#include "Message.h"
#include "ServerDB.h"
#include "Connection.h"
#include "Server.h"
#include "DBus.h"
#include "Meta.h"
#include "PacketDataStream.h"

uint qHash(const Peer &p) {
	return p.first ^ p.second;
}

LogEmitter::LogEmitter(QObject *p) : QObject(p) {
};

void LogEmitter::addLogEntry(const QString &msg) {
	emit newLogEntry(msg);
};

SslServer::SslServer(QObject *p) : QTcpServer(p) {
}

void SslServer::incomingConnection(int v) {
	QSslSocket *s = new QSslSocket(this);
	s->setSocketDescriptor(v);
	qlSockets.append(s);
}

QSslSocket *SslServer::nextPendingSSLConnection() {
	if (qlSockets.isEmpty())
		return NULL;
	return qlSockets.takeFirst();
}

ServerUser::ServerUser(Server *p, QSslSocket *socket) : Connection(p, socket), User() {
	saiUdpAddress.sin_port = 0;
	saiUdpAddress.sin_addr.s_addr = htonl(socket->peerAddress().toIPv4Address());
	saiUdpAddress.sin_family = AF_INET;

	bUdp = true;
	uiVersion = 0;
	bVerified = true;
}

Server::Server(int snum, QObject *p) : QThread(p) {
	bValid = true;
	iServerNum = snum;

	readParams();
	initialize();

	qtsServer = new SslServer(this);

	connect(qtsServer, SIGNAL(newConnection()), this, SLOT(newClient()), Qt::QueuedConnection);

	if (! qtsServer->listen(qhaBind, usPort)) {
		log(QString("Server: TCP Listen on port %1 failed").arg(usPort));
		bValid = false;
	} else {
		log(QString("Server listening on port %1").arg(usPort));
	}

	sUdpSocket = INVALID_SOCKET;

	if (bValid) {
#ifdef Q_OS_UNIX
		sUdpSocket = ::socket(PF_INET, SOCK_DGRAM, 0);
#else
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
		sUdpSocket = ::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
		DWORD dwBytesReturned = 0;
		BOOL bNewBehaviour = FALSE;
		if (WSAIoctl(sUdpSocket, SIO_UDP_CONNRESET, &bNewBehaviour, sizeof(bNewBehaviour), NULL, 0, &dwBytesReturned, NULL, NULL) == SOCKET_ERROR) {
			log(QString("Failed to set SIO_UDP_CONNRESET: %1").arg(WSAGetLastError()));
		}
#endif
		if (sUdpSocket == INVALID_SOCKET) {
			log("Failed to create UDP Socket");
			bValid = false;
		} else {
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(usPort);
			addr.sin_addr.s_addr = htonl(qhaBind.toIPv4Address());
			if (::bind(sUdpSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
				log(QString("Failed to bind UDP Socket to port %1").arg(usPort));
			} else {
#ifdef Q_OS_UNIX
				int val = 0xe0;
				if (setsockopt(sUdpSocket, IPPROTO_IP, IP_TOS, &val, sizeof(val))) {
					val = 0x80;
					if (setsockopt(sUdpSocket, IPPROTO_IP, IP_TOS, &val, sizeof(val)))
						log("Server: Failed to set TOS for UDP Socket");
				}
#endif
			}
		}
	}

	connect(this, SIGNAL(tcpTransmit(QByteArray, unsigned int)), this, SLOT(tcpTransmitData(QByteArray, unsigned int)), Qt::QueuedConnection);
	connect(this, SIGNAL(reqSync(unsigned int)), this, SLOT(doSync(unsigned int)));

	for (int i=1;i<5000;i++)
		qqIds.enqueue(i);

	qtTimeout = new QTimer(this);
	connect(qtTimeout, SIGNAL(timeout()), this, SLOT(checkTimeout()));

	getBans();
	readChannels();
	readLinks();
	initializeCert();

	if (bValid)
		initRegister();
}

void Server::startThread() {
	if (! isRunning()) {
		bRunning = true;
		start(QThread::HighestPriority);
#ifdef Q_OS_LINUX
		// QThread::HighestPriority == Same as everything else...
		int policy;
		struct sched_param param;
		if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
			if (policy == SCHED_OTHER) {
				policy = SCHED_FIFO;
				param.sched_priority = 1;
				pthread_setschedparam(pthread_self(), policy, &param);
			}
		}
#endif
	}
	if (! qtTimeout->isActive())
		qtTimeout->start(15500);
}

void Server::stopThread() {
	bRunning = false;
	if (isRunning()) {
		qrwlUsers.lockForWrite();
		terminate();
		wait();
		qrwlUsers.unlock();
	}
	qtTimeout->stop();
}

Server::~Server() {
	stopThread();
	if (sUdpSocket != INVALID_SOCKET)
#ifdef Q_OS_UNIX
		close(sUdpSocket);
#else
		closesocket(sUdpSocket);
#endif
	clearACLCache();
	log("Stopped");
}

void Server::readParams() {
	qsPassword = Meta::mp.qsPassword;
	usPort = static_cast<unsigned short>(Meta::mp.usPort + iServerNum - 1);
	iTimeout = Meta::mp.iTimeout;
	iMaxBandwidth = Meta::mp.iMaxBandwidth;
	iMaxUsers = Meta::mp.iMaxUsers;
	iDefaultChan = Meta::mp.iDefaultChan;
	qsWelcomeText = Meta::mp.qsWelcomeText;
	qhaBind = Meta::mp.qhaBind;
	qsRegName = Meta::mp.qsRegName;
	qsRegPassword = Meta::mp.qsRegPassword;
	qsRegHost = Meta::mp.qsRegHost;
	qurlRegWeb = Meta::mp.qurlRegWeb;
	qrUserName = Meta::mp.qrUserName;
	qrChannelName = Meta::mp.qrChannelName;

	QString qsHost = getConf("host", QString()).toString();
	if (! qsHost.isEmpty()) {
		if (! qhaBind.setAddress(qsHost)) {
			QHostInfo hi = QHostInfo::fromName(qsHost);
			foreach(QHostAddress qha, hi.addresses()) {
				if ((qha.protocol() == QAbstractSocket::IPv4Protocol) || (qha.protocol() == QAbstractSocket::IPv6Protocol)) {
					qhaBind = qha;
					break;
				}
			}
			if ((qhaBind == QHostAddress::AnyIPv6) || (qhaBind.isNull())) {
				log(QString("Lookup of bind hostname %1 failed").arg(qsHost));
				qhaBind = Meta::mp.qhaBind;
			}

		}
		log(QString("Binding to address %1").arg(qhaBind.toString()));
	}

	qsPassword = getConf("password", qsPassword).toString();
	usPort = static_cast<unsigned short>(getConf("port", usPort).toUInt());
	iTimeout = getConf("timeout", iTimeout).toInt();
	iMaxBandwidth = getConf("bandwidth", iMaxBandwidth).toInt();
	iMaxUsers = getConf("users", iMaxUsers).toInt();
	iDefaultChan = getConf("defaultchannel", iDefaultChan).toInt();
	qsWelcomeText = getConf("welcometext", qsWelcomeText).toString();

	qsRegName = getConf("registername", qsRegName).toString();
	qsRegPassword = getConf("registerpassword", qsRegPassword).toString();
	qsRegHost = getConf("registerhostname", qsRegHost).toString();
	qurlRegWeb = QUrl(getConf("registerurl", qurlRegWeb.toString()).toString());

	qrUserName=QRegExp(getConf("username", qrUserName.pattern()).toString());
	qrChannelName=QRegExp(getConf("channelname", qrChannelName.pattern()).toString());
}

void Server::setLiveConf(const QString &key, const QString &value) {
	QString v = value.trimmed().isEmpty() ? QString() : value;
	int i = v.toInt();
	if (key == "password")
		qsPassword = !v.isNull() ? v : Meta::mp.qsPassword;
	else if (key == "timeout")
		iTimeout = i ? i : Meta::mp.iTimeout;
	else if (key == "bandwidth")
		iMaxBandwidth = i ? i : Meta::mp.iMaxBandwidth;
	else if (key == "users")
		iMaxUsers = i ? i : Meta::mp.iMaxUsers;
	else if (key == "defaultchannel")
		iDefaultChan = i ? i : Meta::mp.iDefaultChan;
	else if (key == "welcometext")
		qsWelcomeText = !v.isNull() ? v : Meta::mp.qsWelcomeText;
	else if (key == "registername")
		qsRegName = !v.isNull() ? v : Meta::mp.qsRegName;
	else if (key == "registerpassword")
		qsRegPassword = !v.isNull() ? v : Meta::mp.qsRegPassword;
	else if (key == "registerhostname")
		qsRegHost = !v.isNull() ? v : Meta::mp.qsRegHost;
	else if (key == "registerurl")
		qurlRegWeb = !v.isNull() ? v : Meta::mp.qurlRegWeb;
	else if (key == "username")
		qrUserName=!v.isNull() ? QRegExp(v) : Meta::mp.qrUserName;
	else if (key == "channelname")
		qrChannelName=!v.isNull() ? QRegExp(v) : Meta::mp.qrChannelName;
}


BandwidthRecord::BandwidthRecord() {
	iRecNum = 0;
	iSum = 0;
	for (int i=0;i<N_BANDWIDTH_SLOTS;i++)
		a_iBW[i] = 0;
}

void BandwidthRecord::addFrame(int size) {
	iSum -= a_iBW[iRecNum];
	a_iBW[iRecNum] = static_cast<unsigned char>(size);
	iSum += a_iBW[iRecNum];

	a_qtWhen[iRecNum].restart();

	iRecNum++;
	if (iRecNum == N_BANDWIDTH_SLOTS)
		iRecNum = 0;
}

int BandwidthRecord::bytesPerSec() const {
	quint64 elapsed = a_qtWhen[iRecNum].elapsed();
	return static_cast<int>((iSum * 1000000LL) / elapsed);
}

int BandwidthRecord::onlineSeconds() const {
	return static_cast<int>(qtFirst.elapsed() / 1000000LL);
}

int BandwidthRecord::idleSeconds() const {
	return static_cast<int>(a_qtWhen[(iRecNum + N_BANDWIDTH_SLOTS - 1) % N_BANDWIDTH_SLOTS].elapsed() / 1000000LL);
}

int BandwidthRecord::bandwidth() const {
	int sincelast = static_cast<int>(a_qtWhen[iRecNum].elapsed() / 20000LL);
	int todo = N_BANDWIDTH_SLOTS - sincelast;
	if (todo < 0)
		return 0;

	int sum = 0;
	for (int i=0;i<todo;i++)
		sum += a_iBW[(iRecNum+N_BANDWIDTH_SLOTS - i) % N_BANDWIDTH_SLOTS];
	return (sum*50)/sincelast;
}

void Server::run() {
	qint32 len;
#if defined(__LP64__)
	char encbuff[512+8];
	char *encrypt = encbuff + 4;
#else
	char encrypt[512];
#endif
	char buffer[512];

	sockaddr_in from;
#ifdef Q_OS_UNIX
	socklen_t fromlen;
#else
	int fromlen;
#endif

	if (sUdpSocket == INVALID_SOCKET)
		return;

	while (bRunning) {
#ifdef Q_OS_DARWIN
		/*
		 * Pthreads on Darwin suck.  They won't allow us to shut down the
		 * server thread while we're in the recvfrom() syscall.
		 *
		 * We use this little hack to loop through our outer loop once in
		 * a while to determine if we should still be running.
		 */
		static struct pollfd fds = {
			sUdpSocket, POLLIN, 0
		};
		int ret = poll(&fds, 1, 1000);

		if (ret == 0) {
			continue;
		} else if (ret == -1) {
			qCritical("poll() failed: %s", strerror(errno));
			break;
		}
#endif

		fromlen = sizeof(from);
#ifdef Q_OS_WIN
		len=::recvfrom(sUdpSocket, encrypt, 512, 0, reinterpret_cast<struct sockaddr *>(&from), &fromlen);
#else
		len=static_cast<qint32>(::recvfrom(sUdpSocket, encrypt, 512, MSG_TRUNC, reinterpret_cast<struct sockaddr *>(&from), &fromlen));
#endif
		if (len == 0) {
			break;
		} else if (len == SOCKET_ERROR) {
			break;
		} else if (len < 5) {
			// 4 bytes crypt header + type + session
			continue;
		} else if (len > 512) {
			continue;
		}

		QReadLocker rl(&qrwlUsers);

		quint64 key = (static_cast<unsigned long long>(from.sin_addr.s_addr) << 16) ^ from.sin_port;

		ServerUser *u = qhPeerUsers.value(key);
		if (u) {
			if (! checkDecrypt(u, encrypt, buffer, len)) {
				continue;
			}
		} else {
			// Unknown peer
			foreach(ServerUser *usr, qhHostUsers.value(from.sin_addr.s_addr)) {
				if (usr->csCrypt.isValid() && checkDecrypt(usr, encrypt, buffer, len)) {
					// Every time we relock, reverify users' existance.
					// The main thread might delete the user while the lock isn't held.
					unsigned int uiSession = usr->uiSession;
					rl.unlock();
					qrwlUsers.lockForWrite();
					if (qhUsers.contains(uiSession)) {
						u = usr;
						qhHostUsers[from.sin_addr.s_addr].remove(u);
						qhPeerUsers.insert(key, u);
						u->saiUdpAddress.sin_port = from.sin_port;
						qrwlUsers.unlock();
						rl.relock();
						if (! qhUsers.contains(uiSession))
							u = NULL;
					}
					break;
				}
			}
			if (! u) {
				continue;
			}
		}
		len -= 4;


		unsigned int msgType = (buffer[0] >> 5) & 0x7;

		if (msgType == MessageHandler::UDPPing) {
			QByteArray qba;
			sendMessage(u, buffer, len, qba, true);
		} else if (msgType == MessageHandler::UDPVoice) {
			u->bUdp = true;
			processMsg(u, buffer, len);
		}
	}
}

bool Server::checkDecrypt(ServerUser *u, const char *encrypt, char *plain, unsigned int len) {
	if (u->csCrypt.isValid() && u->csCrypt.decrypt(reinterpret_cast<const unsigned char *>(encrypt), reinterpret_cast<unsigned char *>(plain), len))
		return true;

	if (u->csCrypt.tLastGood.elapsed() > 5000000ULL) {
		if (u->csCrypt.tLastRequest.elapsed() > 5000000ULL) {
			u->csCrypt.tLastRequest.restart();
			emit reqSync(u->uiSession);
		}
	}
	return false;
}

void Server::sendMessage(ServerUser *u, const char *data, int len, QByteArray &cache, bool force) {
	if ((u->bUdp || force) && (u->saiUdpAddress.sin_port != 0) && u->csCrypt.isValid()) {
#if defined(__LP64__)
		STACKVAR(char, ebuffer, len+4+16);
		char *buffer = reinterpret_cast<char *>(((reinterpret_cast<quint64>(ebuffer) + 8) & ~7) + 4);
#else
		STACKVAR(char, buffer, len+4);
#endif
		u->csCrypt.encrypt(reinterpret_cast<const unsigned char *>(data), reinterpret_cast<unsigned char *>(buffer), len);
#ifdef Q_OS_WIN
		DWORD dwFlow = 0;
		if (Meta::hQoS)
			QOSAddSocketToFlow(Meta::hQoS, sUdpSocket, reinterpret_cast<struct sockaddr *>(& u->saiUdpAddress), QOSTrafficTypeVoice, QOS_NON_ADAPTIVE_FLOW, &dwFlow);
		::sendto(sUdpSocket, buffer, len+4, 0, reinterpret_cast<struct sockaddr *>(& u->saiUdpAddress), sizeof(u->saiUdpAddress));
		if (Meta::hQoS && dwFlow)
			QOSRemoveSocketFromFlow(Meta::hQoS, 0, dwFlow, 0);
#else
		::sendto(sUdpSocket, buffer, len+4, 0, reinterpret_cast<struct sockaddr *>(& u->saiUdpAddress), sizeof(u->saiUdpAddress));
#endif


	} else {
		if (cache.isEmpty())
			cache = QByteArray(data, len);
		emit tcpTransmit(cache,u->uiSession);
	}
}

#define SENDTO \
		if (! pDst->bDeaf && ! pDst->bSelfDeaf && (pDst != u)) { \
			if (poslen && pDst->ssContext == u->ssContext) \
				sendMessage(pDst, buffer, len, qba); \
			else \
				sendMessage(pDst, buffer, len - poslen, qba_npos); \
		}

void Server::processMsg(ServerUser *u, const char *data, int len) {
	if (u->sState != User::Authenticated || u->bMute || u->bSuppressed)
		return;

	User *p;
	BandwidthRecord *bw = & u->bwr;
	Channel *c = u->cChannel;
	QByteArray qba, qba_npos;
	unsigned int counter;
	char buffer[512];
	PacketDataStream pdi(data + 1, len - 1);
	PacketDataStream pds(buffer+1, 511);
	unsigned int target = data[0] & 0x1f;
	unsigned int poslen;

	// IP + UDP + Crypt + Data
	int packetsize = 20 + 8 + 4 + len;
	bw->addFrame(packetsize);

	if (bw->bytesPerSec() > iMaxBandwidth) {
		// Suppress packet.
		return;
	}

	pdi >> counter;

	// Skip QList<QByteArray>
	pdi >> counter;
	while (counter && pdi.isValid()) {
		unsigned int v;
		pdi >> v;
		pdi.skip(v);
	}

	poslen = pdi.left();

	pds << u->uiSession;
	pds.append(data + 1, len - 1);

	len = pds.size() + 1;

	if (target == 0x1f) {
		buffer[0] = static_cast<char>(target);
		sendMessage(u, buffer, len, qba);
		return;
	} else if (target == 0) {
		buffer[0] = 0;
		foreach(p, c->qlUsers) {
			ServerUser *pDst = static_cast<ServerUser *>(p);
			SENDTO;
		}

		if (! c->qhLinks.isEmpty()) {
			QSet<Channel *> chans = c->allLinks();
			chans.remove(c);

			QMutexLocker qml(&qmCache);

			foreach(Channel *l, chans) {
				if (ChanACL::hasPermission(u, l, ChanACL::Speak, acCache)) {
					foreach(p, l->qlUsers) {
						ServerUser *pDst = static_cast<ServerUser *>(p);
						SENDTO;
					}
				}
			}
		}
	} else if (u->qmTargets.contains(target)) {
		QSet<ServerUser *> channel;
		QSet<ServerUser *> direct;

		if (u->qmTargetCache.contains(target)) {
			const ServerUser::TargetCache &cache = u->qmTargetCache.value(target);
			channel = cache.first;
			direct = cache.second;
		} else {
			const WhisperTarget &wt = u->qmTargets.value(target);
			if (! wt.qlChannels.isEmpty()) {
				QMutexLocker qml(&qmCache);

				foreach(const WhisperTarget::Channel &wtc, wt.qlChannels) {
					Channel *wc = qhChannels.value(wtc.iId);
					if (wc) {
						bool link = wtc.bLinks && ! wc->qhLinks.isEmpty();
						bool dochildren = wtc.bChildren && ! wc->qlChannels.isEmpty();
						bool group = ! wtc.qsGroup.isEmpty();
						if (!link && !dochildren && ! group) {
							// Common case
							if (ChanACL::hasPermission(u, wc, ChanACL::Whisper, acCache)) {
								foreach(p, wc->qlUsers) {
									channel.insert(static_cast<ServerUser *>(p));
								}
							}
						} else {
							QSet<Channel *> channels;
							if (link)
								channels = wc->allLinks();
							else
								channels.insert(wc);
							if (dochildren)
								channels.unite(wc->allChildren());
							foreach(Channel *tc, channels) {
								if (ChanACL::hasPermission(u, tc, ChanACL::Whisper, acCache)) {
									foreach(p, tc->qlUsers) {
										if (! group || Group::isMember(tc, tc, wtc.qsGroup, p)) {
											channel.insert(static_cast<ServerUser *>(p));
										}
									}
								}
							}
						}
					}
				}
			}

			foreach(unsigned int id, wt.qlSessions) {
				ServerUser *pDst = qhUsers.value(id);
				if (pDst && ! channel.contains(pDst))
					direct.insert(pDst);
			}

			int uiSession = u->uiSession;
			qrwlUsers.unlock();
			qrwlUsers.lockForWrite();

			if (qhUsers.contains(uiSession))
				u->qmTargetCache.insert(target, ServerUser::TargetCache(channel, direct));
			qrwlUsers.unlock();
			qrwlUsers.lockForRead();
			if (! qhUsers.contains(uiSession))
				return;
		}
		if (! channel.isEmpty()) {
			buffer[0] = 1;
			foreach(ServerUser *pDst, channel) {
				SENDTO;
			}
			if (! direct.isEmpty()) {
				qba.clear();
				qba_npos.clear();
			}
		}
		if (! direct.isEmpty()) {
			buffer[0] = 2;
			foreach(ServerUser *pDst, direct) {
				SENDTO;
			}
		}
	}
}

void Server::log(ServerUser *u, const QString &str) {
	QString msg = QString("<%1:%2(%3)> %4").arg(u->uiSession).arg(u->qsName).arg(u->iId).arg(str);
	log(msg);
}

void Server::log(const QString &msg) {
	dblog(msg);
	qWarning("%d => %s", iServerNum, msg.toUtf8().constData());
}

void Server::newClient() {
	forever {
		QSslSocket *sock = qtsServer->nextPendingSSLConnection();
		if (! sock)
			return;

		QHostAddress adr = sock->peerAddress();

		if (meta->banCheck(adr)) {
			log(QString("Ignoring connection: %1 (Global ban)").arg(addressToString(sock->peerAddress(), sock->peerPort())));
			sock->disconnectFromHost();
			sock->deleteLater();
			return;
		}

		HostAddress ha(adr);

		foreach(const Ban &ban, qlBans) {
			if (ban.haAddress.match(ha, ban.iMask)) {
				log(QString("Ignoring connection: %1 (Server ban)").arg(addressToString(sock->peerAddress(), sock->peerPort())));
				sock->disconnectFromHost();
				sock->deleteLater();
				return;
			}
		}

		sock->setPrivateKey(qskKey);
		sock->setLocalCertificate(qscCert);
		sock->addCaCertificates(qlCA);

		if (qqIds.isEmpty()) {
			sock->disconnectFromHost();
			sock->deleteLater();
			return;
		}

		if (qhUsers.isEmpty())
			startThread();

		ServerUser *u = new ServerUser(this, sock);
		u->uiSession = qqIds.dequeue();
		u->haAddress = ha;

		{
			QWriteLocker wl(&qrwlUsers);
			qhUsers.insert(u->uiSession, u);
			qhHostUsers[htonl(sock->peerAddress().toIPv4Address())].insert(u);
		}

		connect(u, SIGNAL(connectionClosed(const QString &)), this, SLOT(connectionClosed(const QString &)));
		connect(u, SIGNAL(message(unsigned int, const QByteArray &)), this, SLOT(message(unsigned int, const QByteArray &)));
		connect(u, SIGNAL(handleSslErrors(const QList<QSslError> &)), this, SLOT(sslError(const QList<QSslError> &)));
		connect(u, SIGNAL(encrypted()), this, SLOT(encrypted()));

		log(u, QString("New connection: %1").arg(addressToString(sock->peerAddress(), sock->peerPort())));

		u->setToS();

		sock->startServerEncryption();
	}
}

void Server::encrypted() {
	ServerUser *uSource = qobject_cast<ServerUser *>(sender());
	int major, minor, patch;
	QString release;

	Meta::getVersion(major, minor, patch, release);

	MumbleProto::Version mpv;
	mpv.set_version((major << 16) | (minor << 8) | patch);
	mpv.set_release(u8(release));
	mpv.set_os(u8(meta->qsOS));
	mpv.set_os_version(u8(meta->qsOSVersion));
	sendMessage(uSource, mpv);

	QList<QSslCertificate> certs = uSource->peerCertificateChain();
	if (!certs.isEmpty()) {
		const QSslCertificate &cert = certs.last();
		uSource->qslEmail = cert.alternateSubjectNames().values(QSsl::EmailEntry);
		uSource->qsHash = cert.digest(QCryptographicHash::Sha1).toHex();
		if (! uSource->qslEmail.isEmpty() && uSource->bVerified) {
			log(uSource, QString("Strong certificate for %1 <%2> (signed by %3)").arg(cert.subjectInfo(QSslCertificate::CommonName)).arg(uSource->qslEmail.join(", ")).arg(certs.first().issuerInfo(QSslCertificate::CommonName)));
		}

		foreach(const Ban &ban, qlBans) {
			if (ban.qsHash == uSource->qsHash) {
				log(uSource, QString("Certificate hash is banned."));
				uSource->disconnectSocket();
			}
		}
	}
}

void Server::sslError(const QList<QSslError> &errors) {
	ServerUser *u = qobject_cast<ServerUser *>(sender());
	bool ok = true;
	foreach(QSslError e, errors) {
		switch (e.error()) {
			case QSslError::InvalidPurpose:
				// Allow email certificates.
				break;
			case QSslError::NoPeerCertificate:
			case QSslError::SelfSignedCertificate:
			case QSslError::SelfSignedCertificateInChain:
			case QSslError::UnableToGetLocalIssuerCertificate:
			case QSslError::HostNameMismatch:
				u->bVerified = false;
				break;
			default:
				log(u, QString("SSL Error: %1").arg(e.errorString()));
				ok = false;
		}
	}
	if (! u)
		return;

	if (ok)
		u->proceedAnyway();
	else
		u->disconnectSocket(true);
}

void Server::connectionClosed(const QString &reason) {
	Connection *c = qobject_cast<Connection *>(sender());
	if (! c)
		return;
	ServerUser *u = static_cast<ServerUser *>(c);

	log(u, QString("Connection closed: %1").arg(reason));

	if (u->sState == User::Authenticated) {
		MumbleProto::UserRemove mpur;
		mpur.set_session(u->uiSession);
		sendExcept(u, mpur);

		emit userDisconnected(u);
	}

	{
		QWriteLocker wl(&qrwlUsers);

		qhUsers.remove(u->uiSession);
		qhHostUsers[u->saiUdpAddress.sin_addr.s_addr].remove(u);
		quint64 key = (static_cast<unsigned long long>(u->saiUdpAddress.sin_addr.s_addr) << 16) ^ u->saiUdpAddress.sin_port;
		qhPeerUsers.remove(key);

		if (u->cChannel)
			u->cChannel->removeUser(u);
	}

	qqIds.enqueue(u->uiSession);

	if (u->sState == User::Authenticated)
		clearACLCache(u);

	u->deleteLater();

	if (qhUsers.isEmpty())
		stopThread();
}

void Server::message(unsigned int uiType, const QByteArray &qbaMsg, ServerUser *u) {
	if (u == NULL) {
		u = static_cast<ServerUser *>(sender());
	}

	if (uiType == MessageHandler::UDPTunnel) {
		int l = qbaMsg.size();
		if (l < 2)
			return;

		QReadLocker rl(&qrwlUsers);

		u->bUdp = false;

		const char *buffer = qbaMsg.constData();

		unsigned int msgType = (buffer[0] >> 5) & 0x7;

		if (msgType == MessageHandler::UDPVoice)
			processMsg(u, buffer, l);
		return;
	}

#define MUMBLE_MH_MSG(x) case MessageHandler:: x : { \
		MumbleProto:: x msg; \
		if (msg.ParseFromArray(qbaMsg.constData(), qbaMsg.size())) { \
			printf("== %s:\n", #x); \
			msg.PrintDebugString(); \
			msg.DiscardUnknownFields(); \
			msg##x(u, msg); \
		} \
		break; \
	}

	switch (uiType) {
			MUMBLE_MH_ALL
	}
}

void Server::checkTimeout() {
	QList<ServerUser *> qlClose;

	qrwlUsers.lockForRead();
	foreach(ServerUser *u, qhUsers) {
		if (u->activityTime() > (iTimeout * 1000)) {
			log(u, "Timeout");
			qlClose.append(u);
		}
	}
	qrwlUsers.unlock();
	foreach(ServerUser *u, qlClose)
		u->disconnectSocket(true);
}

void Server::tcpTransmitData(QByteArray a, unsigned int id) {
	Connection *c = qhUsers.value(id);
	if (c) {
		QByteArray qba;
		int len = a.size();

		qba.resize(len + 4);
		unsigned char *uc = reinterpret_cast<unsigned char *>(qba.data());
		uc[0] = MessageHandler::UDPTunnel;
		uc[1] = static_cast<unsigned char>((len >> 16) & 0xFF);
		uc[2] = static_cast<unsigned char>((len >> 8) & 0xFF);
		uc[3] = static_cast<unsigned char>(len & 0xFF);
		memcpy(uc + 4, a.constData(), len);

		c->sendMessage(qba);
		c->forceFlush();
	}
}

void Server::doSync(unsigned int id) {
	ServerUser *u = qhUsers.value(id);
	if (u) {
		log(u, "Requesting crypt-nonce resync");
		MumbleProto::CryptSetup mpcs;
		sendMessage(u, mpcs);
	}
}

void Server::sendProtoMessage(ServerUser *u, const ::google::protobuf::Message &msg, unsigned int msgType) {
	QByteArray cache;
	u->sendMessage(msg, msgType, cache);
}

void Server::sendProtoAll(const ::google::protobuf::Message &msg, unsigned int msgType) {
	sendProtoExcept(NULL, msg, msgType);
}

void Server::sendProtoExcept(ServerUser *u, const ::google::protobuf::Message &msg, unsigned int msgType) {
	QByteArray cache;
	foreach(ServerUser *usr, qhUsers)
		if ((usr != u) && (usr->sState == User::Authenticated))
			usr->sendMessage(msg, msgType, cache);
}

void Server::removeChannel(Channel *chan, User *src, Channel *dest) {
	Channel *c;
	User *p;

	if (dest == NULL)
		dest = chan->cParent;

	chan->unlink(NULL);

	foreach(c, chan->qlChannels) {
		removeChannel(c, src, dest);
	}

	foreach(p, chan->qlUsers) {
		chan->removeUser(p);

		MumbleProto::UserState mpus;
		mpus.set_session(p->uiSession);
		mpus.set_channel_id(dest->iId);
		sendAll(mpus);

		userEnterChannel(p, dest);
	}

	MumbleProto::ChannelRemove mpcr;
	mpcr.set_channel_id(chan->iId);
	sendAll(mpcr);

	removeChannel(chan);
	emit channelRemoved(chan);

	if (chan->cParent) {
		QWriteLocker wl(&qrwlUsers);
		chan->cParent->removeChannel(chan);
	}

	delete chan;
}

void Server::userEnterChannel(User *p, Channel *c, bool quiet) {
	clearACLCache(p);

	if (quiet && (p->cChannel == c))
		return;

	{
		QWriteLocker wl(&qrwlUsers);
		c->addUser(p);
	}

	if (quiet)
		return;

	setLastChannel(p);

	bool mayspeak = hasPermission(p, c, ChanACL::Speak);
	bool sup = p->bSuppressed;

	if (! p->bMute) {
		if (mayspeak == sup) {
			// Ok, he can speak and was suppressed, or vice versa
			p->bSuppressed = ! mayspeak;

			MumbleProto::UserState mpus;
			mpus.set_session(p->uiSession);
			mpus.set_suppressed(p->bSuppressed);
			sendAll(mpus);
		}
	}
	emit userStateChanged(p);
}

bool Server::hasPermission(User *p, Channel *c, QFlags<ChanACL::Perm> perm) {
	QMutexLocker qml(&qmCache);
	return ChanACL::hasPermission(p, c, perm, acCache);
}

void Server::clearACLCache(User *p) {
	{
		QMutexLocker qml(&qmCache);

		if (p) {
			ChanACL::ChanCache *h = acCache.take(p);
			if (h)
				delete h;
		} else {
			foreach(ChanACL::ChanCache *h, acCache)
				delete h;
			acCache.clear();
		}
	}

	{
		QWriteLocker lock(&qrwlUsers);

		foreach(ServerUser *u, qhUsers)
			u->qmTargetCache.clear();
	}
}

QString Server::addressToString(const QHostAddress &adr, unsigned short port) {
	HostAddress ha(adr);

	if ((Meta::mp.iObfuscate != 0) && adr.protocol() == QAbstractSocket::IPv4Protocol) {
		quint32 num = adr.toIPv4Address() ^ Meta::mp.iObfuscate;

		QHostAddress n(num);
		ha = HostAddress(n);
	}
	QString p = QString::number(port);
	return QString("%1:%2").arg(ha.toString(), p);
}

bool Server::validateUserName(const QString &name) {
	return (qrUserName.exactMatch(name) && (name.length() <= 512));
}

bool Server::validateChannelName(const QString &name) {
	return (qrChannelName.exactMatch(name) && (name.length() <= 512));
}
