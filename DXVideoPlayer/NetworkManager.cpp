#include "NetworkManager.h"
#include "IApp.h"

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cmath>
#include <algorithm>
#include "PlaybackManager.h"
#include "utils.h"
#include "Logger.h"

#pragma comment(lib, "ws2_32.lib")

NetworkManager::NetworkManager(IApp* appInterface)
    : appInterface(appInterface)
{
}

NetworkManager::~NetworkManager()
{
    Stop();
}

void NetworkManager::Start()
{
    if (!clientRunning)
    {
        clientRunning = true;
        clientThread = std::thread(&NetworkManager::RunClient, this);
        //std::cout << "NetworkManager: Client background thread started." << std::endl;
        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Start", "Client background thread started!");
    }

    if (!serverRunning)
    {
        serverRunning = true;
        serverThread = std::thread(&NetworkManager::RunServer, this);
        //std::cout << "NetworkManager: Server listener thread started on port " << listenPort << "." << std::endl;
        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Start", "Server listener thread started on port: " + std::to_string(listenPort));
    }
}

void NetworkManager::Stop()
{
    // Step 1: Signal loop conditions to drop out instantly
    clientRunning = false;
    serverRunning = false;

    // Step 2: Forcefully kick the outbound client socket out of blocking connect/send calls
    {
        std::lock_guard<std::mutex> lock(clientSocketMutex);
        if (activeClientSocket != (SOCKET)-1)
        {
            closesocket(activeClientSocket);
            activeClientSocket = (SOCKET)-1;
        }
    }

    // Step 3: Forcefully close the listening server socket.
    // This instantly breaks accept() out of its blocking state with an error.
    {
        std::lock_guard<std::mutex> lock(serverSocketMutex);
        if (listenSocket != (SOCKET)-1)
        {
            closesocket(listenSocket);
            listenSocket = (SOCKET)-1;
        }
    }

    // Step 4: Wait for threads to complete cleanly without deadlocks
    if (clientThread.joinable())
    {
        clientThread.join();
    }

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    //std::cout << "NetworkManager: All background threads stopped cleanly." << std::endl;
    Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Stop", "All background threads stopped cleanly!");
}

//##########################################################################################
//##    CLIENT IMPLEMENTATION (SEND POSITIONS)
//##########################################################################################

void NetworkManager::RunClient()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return;

    while (clientRunning)
    {
        SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == (SOCKET)-1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clientSocketMutex);
            if (!clientRunning) {
                closesocket(clientSocket);
                break;
            }
            activeClientSocket = clientSocket;
        }

		Config config = appInterface->GetConfig();
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        /*serverAddr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);*/
		serverAddr.sin_port = htons(config.target_port);
		inet_pton(AF_INET, config.target_ip.c_str(), &serverAddr.sin_addr);


        //std::cout << "[Network Client] Attempting to connect to server at " << serverIP << ":" << serverPort << "..." << std::endl;
		//std::cout << "[Network Client] Attempting to connect to server at " << config.target_ip << ":" << config.target_port << "..." << std::endl;
		Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunClient", "Attempting to connect to server at " + config.target_ip + ":" + std::to_string(config.target_port) + "...");
        if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == -1)
        {
            std::lock_guard<std::mutex> lock(clientSocketMutex);
            closesocket(clientSocket);
            activeClientSocket = (SOCKET)-1;

            for (int i = 0; i < 20 && clientRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        //std::cout << "[Network Client]: Client connected to Position Server." << std::endl;
		Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunClient", "Client connected to Position Server!");
        HandlePositionSend(clientSocket);

        {
            std::lock_guard<std::mutex> lock(clientSocketMutex);
            closesocket(clientSocket);
            activeClientSocket = (SOCKET)-1;
        }
    }

    WSACleanup();
}

void NetworkManager::HandlePositionSend(SOCKET socket)
{
    auto period_duration = std::chrono::milliseconds(appInterface->GetConfig().send_period_ms);
    char msg_buffer[64];
    float last_known_position = 0.0f;
    double scale = appInterface->GetConfig().positions_scale;
    double offset = appInterface->GetConfig().positions_offset;
    float brakeAcceleration = 200;
    auto next_frame = std::chrono::steady_clock::now() + period_duration;
    float previousSentPosition = 0.0f;
    double current_speed = 0.0f;
    positions_delay_ms = appInterface->GetConfig().positions_delay_ms;
    PlaybackManager* playbackMgr = appInterface->GetPlaybackManager();

    while (clientRunning)
    {
        auto now = std::chrono::steady_clock::now();
        auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);

        if (time_left.count() > 2)
            std::this_thread::sleep_for(time_left - std::chrono::milliseconds(2));

        while (std::chrono::steady_clock::now() < next_frame)
        {
            //Spin until it's time for the next frame
        }

        auto trigger_time = std::chrono::steady_clock::now();
        int64_t trigger_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(trigger_time.time_since_epoch()).count();

        float pos_value = 0.0f;
        double progress_pct = 0.0;

        std::vector<float> positions = appInterface->GetPositions();

        if (!positions.empty())
        {
            double last_video_time = appInterface->GetLastPTS();
            int64_t capture_time_ns = appInterface->GetBGCaptureTimeNS();
            double calc_time = 0.0;

            if (capture_time_ns > 0)
            {
                int64_t elapsed_ns = trigger_ns - capture_time_ns;
                double elapsed_sec = (double)elapsed_ns / 1000000000.0;

                if (elapsed_sec > 2)
                    elapsed_sec = 0;

                calc_time = last_video_time + elapsed_sec;
            }
            else
            {
                calc_time = last_video_time;
            }

            double delay_s = positions_delay_ms / 1000.0;
            calc_time -= delay_s;
            if (calc_time < 0.0)
                calc_time = 0.0;

            int count = (int)positions.size();
            double exact_index = calc_time * positions_framerate;
            int base_idx = (int)std::floor(exact_index);
            double frac = exact_index - base_idx;

            int idx0 = (std::min)(base_idx, count - 1);
            int idx1 = (std::min)(base_idx + 1, count - 1);

            if (idx0 < 0) idx0 = 0;
            if (idx1 < 0) idx1 = 0;

            float val0 = positions[idx0];
            float val1 = positions[idx1];

            float calculated_csv_pos = (float)(val0 * (1.0 - frac) + val1 * frac) * scale + offset;

            if (!playbackMgr->transitionMode)
            {
                pos_value = calculated_csv_pos;
                if (playbackMgr->backgroundTrack->IsActive())
                {
                    last_known_position = pos_value;
                }
            }
            else
            {
                pos_value = last_known_position;
            }

            if (count > 0)
            {
                progress_pct = exact_index / (double)count;
                if (progress_pct > 1.0)
                {
                    progress_pct = 1.0;
                }
            }
        }
        else
        {
            pos_value = last_known_position;
        }

        double dt_sec = period_duration.count() / 1000.0;
        if (dt_sec <= 0.001) dt_sec = 0.040;
        if (!playbackMgr->stopping)
            current_speed = (double)(last_known_position - previousSentPosition) / dt_sec;
        /*std::cout << "[Network Client] Calculated position: " << last_known_position << " | Speed: " << current_speed << std::endl;*/

        if (playbackMgr->transitionMode)
        {
            //PHASE 1 - BRAKE TO STOP
            if (playbackMgr->stopping)
            {
                progress_pct = 0.0;
                //Check in which direction the monitor is moving
                int movementDir = GetMovementDirection(current_speed);

                // Accelerate toward zero (brake)
                double brake_dv = brakeAcceleration * dt_sec;
                double newSpeed = current_speed;
                if (current_speed > 0) {
                    newSpeed -= brake_dv;
                    if (newSpeed < 0) newSpeed = 0;
                }
                else if (current_speed < 0) {
                    newSpeed += brake_dv;
                    if (newSpeed > 0) newSpeed = 0;
                }

                //std::cout << "[Network Client] New Speed: " << newSpeed << std::endl;
				Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandlePositionSend", "New Speed: " + std::to_string(newSpeed));

                pos_value = last_known_position + (float)(newSpeed * dt_sec);
                last_known_position = pos_value;
                current_speed = newSpeed;

                if (newSpeed == 0)
                {
					current_speed = 0.0;
					playbackMgr->stopping = false;

                    //COMPUTE DURATION
                    // 1. Record the exact position where the braking finished
					playbackMgr->transitionStartPosition = pos_value;
                    playbackMgr->transitionStartTime = std::chrono::steady_clock::now();
                    
                    // 2. Calculate the distance left to travel
                    float distanceToTravel = std::abs(playbackMgr->transitionTargetPosition - playbackMgr->transitionStartPosition);

                    // 3. Define a comfortable average speed (e.g., units per millisecond)
                    float desiredAverageSpeed = 200.0;

                    // 4. Calculate the duration dynamically (in milliseconds)
                    if (desiredAverageSpeed > 0.0f) 
                    {
                        playbackMgr->transitionDuration = (distanceToTravel / desiredAverageSpeed) * 1000.0; // convert to milliseconds
                    }
                    else 
                    {
                        playbackMgr->transitionDuration = 10000.0; // Fallback to 10 second to avoid divide-by-zero
                    }

                    //Clamp the transitionDuration so it doesn't take an eternity or happen too fast
                    if (playbackMgr->transitionDuration < 500.0) 
                        playbackMgr->transitionDuration = 500.0;   // Minimum 0.5s
                    if (playbackMgr->transitionDuration > 20000.0) 
                        playbackMgr->transitionDuration = 20000.0; // Maximum 20.0s
                }
            }
            //PHASE 2 - MOVE TO A DESIRED POSITION
            else
            {
                auto time_passed = std::chrono::duration_cast<std::chrono::milliseconds>(now - playbackMgr->transitionStartTime);
                double percentage = (double)time_passed.count() / playbackMgr->transitionDuration;

                if (percentage >= 1.0)
                {
                    percentage = 1.0;
                    playbackMgr->transitionMode = false;
                    //std::cout << "### MOVE FINISHED @ " << playbackMgr->transitionTargetPosition << std::endl;
					Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandlePositionSend", "Move finished at position: " + std::to_string(playbackMgr->transitionTargetPosition));
                }

                double smooth_perc = smoothStep(percentage);
                pos_value = (float)(playbackMgr->transitionStartPosition * (1.0 - smooth_perc) + playbackMgr->transitionTargetPosition * smooth_perc);
                progress_pct = percentage;

                static int log_cnt = 0;
                if (log_cnt++ % 12 == 0) {
					/*std::cout << "### TRANSITION: " << playbackMgr->transitionStartPosition << " -> " << playbackMgr->transitionTargetPosition
                        << " | P: " << (int)(percentage * 100) << "% | VEL/s: " << (int)current_speed << std::endl;*/
					Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandlePositionSend", "Transition: " + std::to_string(playbackMgr->transitionStartPosition) + " -> " + std::to_string(playbackMgr->transitionTargetPosition)
						+ " | P: " + std::to_string((int)(percentage * 100)) + "% | VEL/s: " + std::to_string((int)current_speed));
                }
            }
        }

        if (!playbackMgr->backgroundTrack->IsActive() && !playbackMgr->stopping)
        {
			pos_value = last_known_position;
        }

        int len = snprintf(msg_buffer, sizeof(msg_buffer), "{\"positions\":[%.4f]}", pos_value);
        previousSentPosition = last_known_position;
        last_known_position = pos_value;

        for (int i = 0; i < len; i++)
        {
            if (msg_buffer[i] == ',')
                msg_buffer[i] = '.';
        }

        if (len > 0) {
            if (send(socket, msg_buffer, len + 1, 0) == -1)
            {
                //std::cout << "[Network Client] Connection lost." << std::endl;
				Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandlePositionSend", "Connection lost while sending data to server.");
                break;
            }
        }

        next_frame += period_duration;
        if (std::chrono::steady_clock::now() > next_frame + period_duration)
        {
            next_frame = std::chrono::steady_clock::now() + period_duration;
        }
    }
}

//##########################################################################################
//##    SERVER IMPLEMENTATION (RECEIVE COMMANDS)
//##########################################################################################

void NetworkManager::RunServer()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    while (serverRunning)
    {
        SOCKET localListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (localListenSocket == (SOCKET)-1)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        char reuse = 1;
        setsockopt(localListenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(listenPort);
        serverAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(localListenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == -1)
        {
            //std::cerr << "[Network Server] Bind failed on port " << listenPort << std::endl;
			Logger::LogMessage(MESSAGE_TYPE::ERRORS, "NetworkManager", "RunServer", "Bind failed on port: " + std::to_string(listenPort));
            closesocket(localListenSocket);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (listen(localListenSocket, SOMAXCONN) == -1)
        {
            closesocket(localListenSocket);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        //std::cout << "[Network Server] Server listening on port " << listenPort << "..." << std::endl;
		Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunServer", "Server listening on port: " + std::to_string(listenPort) + "...");

        {
            std::lock_guard<std::mutex> lock(serverSocketMutex);
            if (!serverRunning) {
                closesocket(localListenSocket);
                break;
            }
            listenSocket = localListenSocket;
        }

        // Apply a socket receive timeout so accept() checks serverRunning every 500ms
        DWORD timeout = 500;
        setsockopt(localListenSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        while (serverRunning)
        {
            sockaddr_in clientAddr;
            int clientAddrLen = sizeof(clientAddr);
            SOCKET inboundClient = accept(localListenSocket, (sockaddr*)&clientAddr, &clientAddrLen);

            if (inboundClient == (SOCKET)-1)
            {
                if (!serverRunning) break;
                continue;
            }

            // Extract and cleanly format the incoming client's IP address
            char clientIPStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIPStr, INET_ADDRSTRLEN);

            // --- NEW: Print connection message immediately ---
            //std::cout << "[Network Server] [+] New client connected from: " << clientIPStr << std::endl;
			Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunServer", "New client connected from: " + std::string(clientIPStr));

            appInterface->SetClientSocket(inboundClient);

            // Spin off a separate worker thread for this client connection.
            // This prevents a single client from blocking the main server accept loop.
            std::thread connectionThread(&NetworkManager::HandleIncomingConnection, this, inboundClient);
            connectionThread.detach();
        }

        {
            std::lock_guard<std::mutex> lock(serverSocketMutex);
            closesocket(localListenSocket);
            listenSocket = (SOCKET)-1;
        }
    }

    WSACleanup();
}

void NetworkManager::HandleIncomingConnection(SOCKET clientSocket)
{
    char recvBuffer[1024];

    // Set data transfer timeout so the thread periodically checks serverRunning
    DWORD timeout = 1000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    while (serverRunning)
    {
        int bytesReceived = recv(clientSocket, recvBuffer, sizeof(recvBuffer) - 1, 0);
        if (bytesReceived > 0)
        {
            recvBuffer[bytesReceived] = '\0';
            //std::cout << "[Network Server] Received message: " << recvBuffer << std::endl;
            appInterface->HandleNetworkCommand(std::string(recvBuffer));
        }
        else if (bytesReceived == 0)
        {
            //std::cout << "[Network Server] [-] Client disconnected gracefully." << std::endl;
			Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandleIncomingConnection", "Client disconnected gracefully.");
            break;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
            {
                continue; // Timeout passed, verify loop state safely
            }
            //std::cout << "[Network Server] [-] Client connection lost abruptly." << std::endl;
			Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandleIncomingConnection", "Client connection lost abruptly. Error code: " + std::to_string(err));
            break;
        }
    }

    // Always clean up the individual client socket handle when exiting this connection's thread lifecycle
    closesocket(clientSocket);
}