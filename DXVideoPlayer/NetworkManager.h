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
	// Client components
	std::thread clientThread;
	std::atomic<bool> clientRunning;
	SOCKET activeClientSocket = (SOCKET)-1;
	std::mutex clientSocketMutex;

	// Server components
	std::thread serverThread;
	std::atomic<bool> serverRunning;
	SOCKET listenSocket = (SOCKET)-1;
	std::mutex serverSocketMutex;
	int listenPort = 22345;

	// Configuration for position sending
	double positions_delay_ms = -130.0;
	double positions_framerate = 60.0;

	IApp* appInterface;

	float last_known_position = 0.0f;
	float previousSentPosition = 0.0f;
	bool stopping_phase = false;
	bool transition_mode_active = false;
	bool sequence_triggered = false;
	float transition_start_position = 0.0f;
	float transition_target_position = 0.0f;
	double transition_duration_ms = 10000.0;
	std::chrono::steady_clock::time_point transition_start_time;

public:
	NetworkManager(IApp* appInterface);
	~NetworkManager();

	void Start();
	void Stop();
	bool IsTransitionActive() const { return transition_mode_active; }
	bool IsStoppingPhase() const { return stopping_phase; }
	float GetTransitionTargetPosition() const { return transition_target_position; }
	void SetupTransition(float targetPos, int id);

private:
	//Client methods
	void RunClient();
	void PositionSend(SOCKET socket);

	//Server methods
	void RunServer();
	void HandleIncomingConnection(SOCKET clientSocket);
};