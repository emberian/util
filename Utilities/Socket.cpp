#include "Socket.h"
#include "Misc.h"
#include <utility>
#include <stdexcept>

#ifdef WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define FD_SETSIZE 1024
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <Windows.h>

	static bool winsockInitialized = false;
#elif defined POSIX
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <netinet/in.h>
	#include <unistd.h>
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <stdio.h>
	#include <string.h>
	#include <endian.h>
#endif

using namespace Utilities::Net;
using namespace std;

bool Socket::prepareRawSocket(string address, string port, bool willListenOn, void** addressInfo) {
	addrinfo serverHints;
	addrinfo* serverAddrInfo;

#ifdef WINDOWS
	SOCKET rawSocket;

	if (!winsockInitialized) {
		WSADATA startupData;
		WSAStartup(514, &startupData);
		winsockInitialized = true;
	}
#elif defined POSIX
	int rawSocket;
#endif

	memset(&serverHints, 0, sizeof(addrinfo));

	switch (this->family) {
		case Socket::Families::IPV4: serverHints.ai_family = AF_INET; break;
		case Socket::Families::IPV6: serverHints.ai_family = AF_INET6; break;
		#ifdef WINDOWS
		case Socket::Families::IPAny: serverHints.ai_family = AF_INET6; break;
		#elif defined POSIX
		case Socket::Families::IPAny: serverHints.ai_family = AF_UNSPEC; break;
		#endif
		default: return false;
	}

	switch (this->type) {
		case Socket::Types::TCP: serverHints.ai_socktype = SOCK_STREAM; break;
		default: return false;
	}

	if (willListenOn)
		serverHints.ai_flags = AI_PASSIVE;
		
	if (getaddrinfo(address != "" ? address.c_str() : nullptr, port.c_str(), &serverHints, &serverAddrInfo) != 0) {
		return false;
	}

	if (serverAddrInfo == nullptr) {
		return false;
	}
	
	rawSocket = socket(serverAddrInfo->ai_family, serverAddrInfo->ai_socktype, serverAddrInfo->ai_protocol);
#ifdef WINDOWS
	if (rawSocket == INVALID_SOCKET) {
		closesocket(rawSocket); // It might be invalid, who cares?
		freeaddrinfo(serverAddrInfo);
		return false;
	}
	else {
		int opt = 0;
		setsockopt(rawSocket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&opt), sizeof(opt));
	}
#elif defined POSIX
	if (rawSocket == -1) {
		::close(rawSocket);
		freeaddrinfo(serverAddrInfo);
		return false;
	}
#endif

	this->rawSocket = rawSocket;
	*(addrinfo**)addressInfo = serverAddrInfo;

	return true;
}

Socket::Socket(Families family, Types type) {
	this->family = family;
	this->type = type;
	this->connected = false;
	this->lastError = 0;
	memset(this->remoteEndpointAddress, 0, ADDRESS_LENGTH);
	#ifdef WINDOWS
		this->rawSocket = INVALID_SOCKET;
	#elif defined POSIX
		this->rawSocket = -1;
	#endif
}

Socket::Socket(Families family, Types type, string address, string port) {
	this->family = family;
	this->type = type;
	this->connected = true;
	this->lastError = 0;
	memset(this->remoteEndpointAddress, 0, ADDRESS_LENGTH);

	addrinfo* serverAddrInfo;
	
	if (!this->prepareRawSocket(address, port, false, (void**)&serverAddrInfo))
		throw runtime_error("Can't create raw socket.");
	
	if (::connect(this->rawSocket, serverAddrInfo->ai_addr, (int)serverAddrInfo->ai_addrlen) != 0) {
		#ifdef WINDOWS
			closesocket(this->rawSocket);
		#elif defined POSIX
			::close(this->rawSocket);
		#endif

		freeaddrinfo(serverAddrInfo);
		
		throw runtime_error("Couldn't connect to server.");
	}
	
	freeaddrinfo(serverAddrInfo);
}

Socket::Socket(Families family, Types type, string port) {
	this->family = family;
	this->type = type;
	this->connected = true;
	this->lastError = 0;
	memset(this->remoteEndpointAddress, 0, ADDRESS_LENGTH);

	addrinfo* serverAddrInfo;
	
	if (!this->prepareRawSocket("", port, true, (void**)&serverAddrInfo))
		throw runtime_error("Can't create raw socket.");

	if (::bind(this->rawSocket, serverAddrInfo->ai_addr, (int)serverAddrInfo->ai_addrlen) != 0)
		goto error;

	if (::listen(this->rawSocket, SOMAXCONN) != 0)
		goto error;
	
	freeaddrinfo(serverAddrInfo);

	return;

error:
#ifdef WINDOWS
	closesocket(this->rawSocket);
#elif defined POSIX
	::close(this->rawSocket);
#endif
	freeaddrinfo(serverAddrInfo);
	
	throw runtime_error("Couldn't listen on port.");
}

Socket::Socket(Socket&& other) {
	*this = std::move(other);
}

Socket& Socket::operator=(Socket&& other) {
	if (this == &other)
		return *this;
		
	this->family = other.family;
	this->type = other.type;
	this->rawSocket = other.rawSocket;
	this->connected = other.connected;
	this->lastError = other.lastError;

	memcpy(this->remoteEndpointAddress, other.remoteEndpointAddress, ADDRESS_LENGTH);
	memset(other.remoteEndpointAddress, 0, ADDRESS_LENGTH);
	other.connected = false;
	#ifdef WINDOWS
		other.rawSocket = INVALID_SOCKET;
	#elif defined POSIX
		other.rawSocket = -1;
	#endif

	return *this;
}

Socket::~Socket() {
	this->close();
}

void Socket::close() {
	if (!this->connected)
		return;

	this->connected = false;
	#ifdef WINDOWS
		shutdown(static_cast<SOCKET>(this->rawSocket), SD_BOTH);
		closesocket(static_cast<SOCKET>(this->rawSocket));
		this->rawSocket = INVALID_SOCKET;
	#elif defined POSIX
		shutdown(this->rawSocket, SHUT_RDWR);
		::close(this->rawSocket);
		this->rawSocket = -1;
	#endif
}

Socket Socket::accept() {
	Socket socket(this->family, this->type);
	sockaddr_storage remoteAddress;
	sockaddr_in* ipv4Address;
	sockaddr_in6* ipv6Address;
	int addressLength = sizeof(remoteAddress);

#ifdef WINDOWS
	SOCKET rawSocket;
	
	rawSocket = ::accept((SOCKET)this->rawSocket, (sockaddr*)&remoteAddress, &addressLength);
	if (rawSocket == INVALID_SOCKET) {
		return socket;
	}

#elif defined POSIX
	int rawSocket;

	rawSocket = ::accept(this->rawSocket, (sockaddr*)&remoteAddress, (socklen_t*)&addressLength);
	if (rawSocket == -1) {
		return socket;
	}

#endif
	
	socket.rawSocket = rawSocket;
	socket.connected = true;

	memset(socket.remoteEndpointAddress, 0, sizeof(socket.remoteEndpointAddress));

	if (remoteAddress.ss_family == AF_INET) {
		ipv4Address = reinterpret_cast<sockaddr_in*>(&remoteAddress);
		memset(socket.remoteEndpointAddress, 0, 10); //to copy the ipv4 address in ipv6 mapped format
		memset(socket.remoteEndpointAddress + 10, 1, 2);
		#ifdef WINDOWS
				memcpy(socket.remoteEndpointAddress + 12, (uint8*)&ipv4Address->sin_addr.S_un.S_addr, 4);
		#elif defined POSIX
				memcpy(socket.remoteEndpointAddress + 12, (uint8*)&ipv4Address->sin_addr.s_addr, 4);
		#endif
	}
	else if (remoteAddress.ss_family == AF_INET6) {
		ipv6Address = reinterpret_cast<sockaddr_in6*>(&remoteAddress);
		#ifdef WINDOWS
				memcpy(socket.remoteEndpointAddress, ipv6Address->sin6_addr.u.Byte, sizeof(ipv6Address->sin6_addr.u.Byte));
		#elif defined POSIX
				memcpy(socket.remoteEndpointAddress, ipv6Address->sin6_addr.s6_addr, sizeof(ipv6Address->sin6_addr.s6_addr));
		#endif
	}

	return socket;
}

uint64 Socket::read(uint8* buffer, uint64 bufferSize) {
	int32 received;

	#ifdef WINDOWS
		received = recv(static_cast<SOCKET>(this->rawSocket), reinterpret_cast<int8*>(buffer), static_cast<int32>(bufferSize), 0);
	#elif defined POSIX
		received = recv(this->rawSocket, reinterpret_cast<int8*>(buffer), static_cast<int32>(bufferSize), 0);
	#endif

	if (received <= 0)
		return 0;

	return (uint32)received;
}

uint64 Socket::write(const uint8* toWrite, uint64 writeAmount) {
	int32 result;

	#ifdef WINDOWS
		result = send(static_cast<SOCKET>(this->rawSocket), reinterpret_cast<const int8*>(toWrite), static_cast<int32>(writeAmount), 0);
	#elif defined POSIX
		result = send(this->rawSocket, reinterpret_cast<const int8*>(toWrite), static_cast<int32>(writeAmount), 0);
	#endif

	return (uint32)result;
}

uint64 Socket::ensureWrite(const uint8* toWrite, uint64 writeAmount, uint8 maxAttempts) {
	uint64 sentSoFar;
	int32 result;
	uint8 tries;

	sentSoFar = 0;
	tries = 0;

	while (true) {
		#ifdef WINDOWS
			result = send(static_cast<SOCKET>(this->rawSocket), reinterpret_cast<const int8*>(toWrite + sentSoFar), static_cast<int32>(writeAmount - sentSoFar), 0);
			if (result != SOCKET_ERROR)
				sentSoFar += result;
		#elif defined POSIX
			result = send(this->rawSocket, reinterpret_cast<const int8*>(toWrite + sentSoFar), static_cast<int32>(writeAmount - sentSoFar), 0);
			if (result != -1)
				sentSoFar += result;
		#endif

		tries++;

		if (sentSoFar == writeAmount || (maxAttempts && tries == maxAttempts))
			break;

		this_thread::sleep_for(chrono::milliseconds(tries * 50));
	}

	return sentSoFar;
}

const uint8* Socket::getRemoteAddress() const {
	return this->remoteEndpointAddress;
}

bool Socket::isConnected() const {
	return this->connected;
}



SocketAsyncWorker::SocketAsyncWorker(ReadCallback callback) {
	#ifdef POSIX
		this->maxFD = 0;
	#endif
	this->running = true;
	this->callback = callback;
}

SocketAsyncWorker::~SocketAsyncWorker() {
	this->shutdown();
}

void SocketAsyncWorker::start() {
	this->worker = thread(&SocketAsyncWorker::run, this);
}

void SocketAsyncWorker::shutdown() {
	if (!this->running)
		return;

	this->running = false;
	this->worker.join();
}

void SocketAsyncWorker::run() {
	fd_set readSet;
	int readySockets;
	map<const Socket*, void*>::iterator listPosition;
	struct timeval selectTimeout;
	uint64 i;

	selectTimeout.tv_usec = 250;
	selectTimeout.tv_sec = 0;
	listPosition = this->list.begin();
		
	while (this->running) {
		FD_ZERO(&readSet);

		this_thread::sleep_for(chrono::milliseconds(25));

		this->listLock.lock();
		
		if (listPosition == this->list.end())
			listPosition = this->list.begin();

		for (i = 0; i < FD_SETSIZE && listPosition != this->list.end(); i++, listPosition++) {
			if (listPosition->second != nullptr) {
				#ifdef WINDOWS
					FD_SET(static_cast<SOCKET>(listPosition->first->rawSocket), &readSet);
				#elif defined POSIX
					FD_SET(listPosition->first->rawSocket, &readSet);
				#endif
			}
		}
		
		if (i != 0) {
			#ifdef WINDOWS
				readySockets = select(0, &readSet, nullptr, nullptr, &selectTimeout);
			#elif defined POSIX
				readySockets = select(this->maxFD + 1, &readSet, nullptr, nullptr, &selectTimeout);
			#endif
		
			if (readySockets > 0) {
				for (pair<const Socket*, void*> i : this->list) {
					const Socket* socket = i.first;
					void* state = i.second;

					if (this->callback && state && FD_ISSET(socket->rawSocket, &readSet)) {
						this->callback(*socket, state);
				
						if (--readySockets == 0)
							break;

					}
				}
			}
		}

		this->listLock.unlock();
	}

	return;
}

void SocketAsyncWorker::registerSocket(const Socket& socket, void* state) {
	this->listLock.lock();
	#ifdef POSIX
		this->maxFD = max(socket.rawSocket, this->maxFD);
	#endif
	this->list[&socket] = state;
	this->listLock.unlock();
}
	
void SocketAsyncWorker::unregisterSocket(const Socket& socket) {
	this->listLock.lock();
	#ifdef POSIX
		for (pair<const Socket*, void*> i : this->list)
			this->maxFD = max(i.first->rawSocket, this->maxFD);
	#endif
	this->list.erase(&socket);
	this->listLock.unlock();
}



int16 Utilities::Net::hostToNetworkInt16(int16 value) {
	return htons(value);
}

int32 Utilities::Net::hostToNetworkInt32(int32 value) {
	return htonl(value);
}

int64 Utilities::Net::hostToNetworkInt64(int64 value) {
	#ifdef WINDOWS
		return htonll(value);
	#elif defined POSIX
		return htobe64(value); // XXX: glibc only
	#endif
}

int16 Utilities::Net::networkToHostInt16(int16 value) {
	return ntohs(value);
}

int32 Utilities::Net::networkToHostInt32(int32 value) {
	return ntohl(value);
}

int64 Utilities::Net::networkToHostInt64(int64 value) {
	#ifdef WINDOWS
		return ntohll(value);
	#elif defined POSIX
		return be64toh(value); // XXX: glibc only
	#endif
}