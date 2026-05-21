#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

#if defined(_WIN32) || defined(_WIN64)
typedef uintptr_t SOCKET;
#else
typedef int SOCKET;
#endif

class IApp;

class NetworkManager
{
private:
	std::string serverIP;
	int serverPort;

	// Client components
	std::thread clientThread;
	std::atomic<bool> clientRunning;
	SOCKET activeClientSocket = (SOCKET)-1;
	std::mutex clientSocketMutex;

	// Server components
	std::thread serverThread;
	std::atomic<bool> serverRunning;
	SOCKET listenSocket = (SOCKET)-1;       // Track listener socket to unblock accept() instantly
	std::mutex serverSocketMutex;           // Protects listenSocket access across threads
	int listenPort = 22345;

	// Configuration for position sending
	double positions_delay_ms = -130.0;
	double positions_framerate = 60.0;

	IApp* appInterface;

public:
	NetworkManager(const std::string& serverIP, int serverPort, IApp* appInterface);
	~NetworkManager();

	void Start();
	void Stop();

private:
	//Client methods
	void RunClient();
	void HandlePositionSend(SOCKET socket);

	//Server methods
	void RunServer();
	void HandleIncomingConnection(SOCKET clientSocket);
};