#include "NetworkManager.h"
#include "IApp.h"

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cmath>
#include <algorithm>
#include "utils.h"

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
        std::cout << "NetworkManager: Client background thread started." << std::endl;
    }

    if (!serverRunning)
    {
        serverRunning = true;
        serverThread = std::thread(&NetworkManager::RunServer, this);
        std::cout << "NetworkManager: Server listener thread started on port " << listenPort << "." << std::endl;
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

    std::cout << "NetworkManager: All background threads stopped cleanly." << std::endl;
}

void NetworkManager::SetupTransition(float targetPos, int id)
{
    std::lock_guard<std::mutex> lock(clientSocketMutex);
    this->transition_target_position = targetPos;
    this->transition_mode_active = true;
    this->stopping_phase = true;
    this->sequence_triggered = false;
    this->transition_start_time = std::chrono::steady_clock::now();
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
        std::cout << "[Network Client] Attempting to connect to server at " << config.target_ip << ":" << config.target_port << "..." << std::endl;
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

        std::cout << "[Network Client]: Client connected to Position Server." << std::endl;
        //HandlePositionSend(clientSocket);
		PositionSend(clientSocket);

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
    /*auto period_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(1.0 / positions_framerate)
    );*/
    auto period_duration = std::chrono::milliseconds(appInterface->GetConfig().send_period_ms);
    char msg_buffer[64];
    float last_known_position = 0.0f;
    double scale = appInterface->GetConfig().positions_scale;
    double offset = appInterface->GetConfig().positions_offset;
    float brakeAcceleration = 200.0;
    auto next_frame = std::chrono::steady_clock::now() + period_duration;
    float previousSentPosition = 0.0f;
    double current_speed = 0.0f;
    positions_delay_ms = appInterface->GetConfig().positions_delay_ms;

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

        int len = snprintf(msg_buffer, sizeof(msg_buffer), "{\"positions\":[%.4f]}", calculated_csv_pos);
        for (int i = 0; i < len; i++)
        {
            if (msg_buffer[i] == ',')
                msg_buffer[i] = '.';
        }

        if (len > 0) {
            if (send(socket, msg_buffer, len + 1, 0) == -1)
            {
                std::cout << "[Network Client] Connection lost." << std::endl;
                break;
            }
        }

		appInterface->GetAppState().lastSentPosition = calculated_csv_pos;

        next_frame += period_duration;
    }
}

void NetworkManager::PositionSend(SOCKET socket)
{
    Config config = appInterface->GetConfig();
    //It computes a precise timing duration block based on the configured send period millisecond value
    auto period_duration = std::chrono::milliseconds(config.send_period_ms);

    char msg_buffer[64];

    // Local configuration mappings matching KineticVideoPlayer logic
    double scale = config.positions_scale;
    double offset = config.positions_offset;
    //Determines how fast the system decelerates on transitions;
    float brakeAcceleration = static_cast<float>(config.cover_stop_acceleration);
    //defines targeted relocation velocity
    float desired_average_speed = static_cast<float>(config.cover_reference_speed);
    double positions_framerate = 60.0;

    //Establishes a precise time for when the next network data packet should be sent.
	//It basically controls the pacing of the position transmissions
    auto next_frame = std::chrono::steady_clock::now() + period_duration;
    double current_speed = 0.0;

    while (clientRunning)
    {
        bool appIsPlaying = appInterface->IsBackgroundPlaying();
        bool appTransitionMode = appInterface->InTransitionMode();
        bool appIsStopping = appInterface->IsStoppingPhase();
        bool appIsCover = appInterface->IsCoverActive();
        float appTransitionPos = appInterface->GetTransitionPosition();
        int appTransitionId = appInterface->GetTransitionId();

        // Handle explicit external resets if supported by your application state
        // if (appInterface->ResetPositionTriggered()) { ... last_known_position = 0.0f; }

        //Checks the system clock to see how long until the scheduled send time (next_frame)...
        auto now = std::chrono::steady_clock::now();
        auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
        //...if more than 2 milliseconds remain
        if (time_left.count() > 2)
        {
            //Put the thread to sleep to avoid CPU waste
            std::this_thread::sleep_for(time_left - std::chrono::milliseconds(2));
        }

        //Handles the remaining time (under 2ms) using a precise spin-lock.
        while (std::chrono::steady_clock::now() < next_frame)
        {
        }

        //Captures a high-resolution snapshot of the exact time point when this calculations cycle began...
        auto trigger_time = std::chrono::steady_clock::now();
        //...and converts it to system nanoseconds.
        int64_t trigger_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(trigger_time.time_since_epoch()).count();

        //Initializes local variables to capture the output position value and progression percentage
        float pos_value = 0.0f;
        double progress_pct = 0.0;
        
        //Extracts the positions to move the screen
        std::vector<float> positions = appInterface->GetPositions();

        //Verifies the positions has been successfully parsed from CSV file
        if (!positions.empty())
        {
            //The moment when the video frame has been fully decoded (FFmpeg)
            double last_video_time = appInterface->GetLastPTS();
            //The moment when the video has been fully processed (e.g. copied into VideoRAM) just before being rendered (Direct3D)
            int64_t capture_time_ns = appInterface->GetBGCaptureTimeNS();
            //Position in time during playback of the video
            double calc_time = 0.0;

            //CALCULATES THE PLAYBACK TIME POINT 
            //If the video has started playing...
            if (capture_time_ns > 0)
            {
				//Computes the elapsed time since the last video frame was processed 
                int64_t elapsed_ns = trigger_ns - capture_time_ns;
				//Converts the elapsed time into seconds
                double elapsed_sec = static_cast<double>(elapsed_ns) / 1000000000.0;

                //If more than 2 seconds have passed without an update, it assumes the video has paused, stopped, or the system lagged heavily. 
                //It resets elapsed_sec to 0 to prevent the physical position from violently jumping forward into the future.
                if (elapsed_sec > 2.0)
                    elapsed_sec = 0.0;

                //Finally computes the video playback playhead position
                calc_time = last_video_time + elapsed_sec;
            }
            else
            {
                calc_time = last_video_time;
            }

            //Converts the config delay from milliseconds to seconds
            double delay_s = config.positions_delay_ms / 1000.0;
			//Rewind the video playhead position by the delay amount to compensate for the system latency between when the video frame is processed and when the position data is sent 
            calc_time -= delay_s;

            //Clamp the playhead to never go negative
            if (calc_time < 0.0) 
                calc_time = 0.0;


			//Get the total number of positions
            int count = static_cast<int>(positions.size());
            double exact_index = calc_time * positions_framerate;
            int base_idx = static_cast<int>(std::floor(exact_index));
            double frac = exact_index - base_idx;

            int idx0 = (std::min)(base_idx, count - 1);
            int idx1 = (std::min)(base_idx + 1, count - 1);

            if (idx0 < 0) idx0 = 0;
            if (idx1 < 0) idx1 = 0;

            float val0 = positions[idx0];
            float val1 = positions[idx1];

            // Linear Interpolation between subsequent position frames
            float calculated_csv_pos = static_cast<float>(val0 * (1.0 - frac) + val1 * frac);

            if (!appTransitionMode && !appIsCover)
            {
                pos_value = calculated_csv_pos;
                if (appIsPlaying)
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
                progress_pct = exact_index / static_cast<double>(count);
                if (progress_pct > 1.0) progress_pct = 1.0;
            }
        }
        else
        {
            pos_value = last_known_position;
        }

        // Calculate average linear velocity: v = dx / dt
        double dt_sec = static_cast<double>(period_duration.count()) / 1000.0;
        if (dt_sec <= 0.001) dt_sec = 0.040;
        current_speed = static_cast<double>(last_known_position - previousSentPosition) / dt_sec;

        // Ported Multi-Phase Transition Management Loop
        if (appTransitionMode)
        {
            // PHASE 1: BRAKE TO STOP
            if (appIsStopping)
            {
                progress_pct = 0.0;
                double brake_dv = brakeAcceleration * dt_sec;
                double newSpeed = current_speed;

                if (current_speed > 0)
                {
                    newSpeed -= brake_dv;
                    if (newSpeed < 0) newSpeed = 0;
                }
                else if (current_speed < 0)
                {
                    newSpeed += brake_dv;
                    if (newSpeed > 0) newSpeed = 0;
                }

                pos_value = last_known_position + static_cast<float>(newSpeed * dt_sec);

                if (newSpeed == 0.0)
                {
                    current_speed = 0.0;
                    appIsStopping = false;

                    transition_start_position = pos_value;
                    transition_start_time = std::chrono::steady_clock::now();
                    transition_target_position = appTransitionPos;

                    float distance_to_travel = std::abs(transition_target_position - transition_start_position);

                    if (desired_average_speed > 0.0f)
                    {
                        transition_duration_ms = (distance_to_travel / desired_average_speed) * 1000.0;
                    }
                    else
                    {
                        transition_duration_ms = 10000.0;
                    }

                    if (transition_duration_ms < 500.0)  transition_duration_ms = 500.0;
                    if (transition_duration_ms > 20000.0) transition_duration_ms = 20000.0;

                    // Ported status dispatch tracking back to external endpoints via strings/JSON if available
                    /*
                    if (appTransitionId >= 0) {
                        char status_buf[256];
                        snprintf(status_buf, sizeof(status_buf), "{\"play_video\":%d,\"loop\":false,\"duration\":%.3f}", appTransitionId, transition_duration_ms / 1000.0);
                        appInterface->HandleNetworkCommand(std::string(status_buf));
                    }
                    */

					appTransitionId = appInterface->GetTransitionId(); 

                    if (appTransitionId >= 0)
                    {
						char status_buf[256];
                        snprintf(status_buf, sizeof(status_buf), "{\"play_choreography\":%d,\"loop\":false,\"fade_in_seconds\":%.3f}", appTransitionId, (transition_duration_ms / 1000.0));
						appInterface->HandleNetworkCommand(std::string(status_buf));
                    }
                }
            }
            // PHASE 2: MOVE TO DESIRED TARGET VIA SMOOTHSTEP ALGORITHM
            else
            {
                auto time_passed = std::chrono::duration_cast<std::chrono::milliseconds>(now - transition_start_time);
                double percentage = static_cast<double>(time_passed.count()) / transition_duration_ms;

                if (percentage >= 1.0)
                {
                    percentage = 1.0;
                    appTransitionMode = false;
                    std::cout << "[Network Client] Move complete at position: " << transition_target_position << std::endl;
                }

                // SmoothStep generation formula helper mapping: 3t^2 - 2t^3
                /*double smooth_perc = percentage * percentage * (3.0 - 2.0 * percentage);*/
                double smooth_perc = smoothStep(percentage);
                pos_value = static_cast<float>(transition_start_position * (1.0 - smooth_perc) + transition_target_position * smooth_perc);
                progress_pct = percentage;
            }
        }

        // Direct position fallbacks for stopped background states
        if (!appIsPlaying && !appIsCover)
        {
            pos_value = last_known_position;
        }

        previousSentPosition = last_known_position;
        last_known_position = pos_value;

        // Save unscaled position state
        appInterface->GetAppState().lastSentPosition = pos_value;

        // Apply dynamic scale and offset variables before shipping data down the socket pipeline
        pos_value = pos_value * static_cast<float>(scale) + static_cast<float>(offset);
        int len = snprintf(msg_buffer, sizeof(msg_buffer), "{\"positions\":[%.4f]}", pos_value);

        // Ensure accurate localization parameters across different decimal configurations
        for (int i = 0; i < len; i++)
        {
            if (msg_buffer[i] == ',') msg_buffer[i] = '.';
        }

        // Trigger transition completed events if executing a Cover configuration loop
        if (appIsCover)
        {
            if (last_known_position == appTransitionPos && !sequence_triggered)
            {
                std::cout << "[Network Client] Cover frame transition finished! " << last_known_position << std::endl;
                // Dispatch or invoke callbacks back to main render loop thread spaces
                sequence_triggered = true;
            }
        }

        // Dispatch raw sequence payload down the active TCP connection channel
        if (len > 0)
        {
            if (send(socket, msg_buffer, len + 1, 0) == -1)
            {
                std::cout << "[Network Client] Lost synchronization stream connection link channel." << std::endl;
                break;
            }
        }

        next_frame += period_duration;
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
            std::cerr << "[Network Server] Bind failed on port " << listenPort << std::endl;
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

        std::cout << "[Network Server] Server listening on port " << listenPort << "..." << std::endl;

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
            std::cout << "[Network Server] [+] New client connected from: " << clientIPStr << std::endl;

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
            std::cout << "[Network Server] [-] Client disconnected gracefully." << std::endl;
            break;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
            {
                continue; // Timeout passed, verify loop state safely
            }
            std::cout << "[Network Server] [-] Client connection lost abruptly." << std::endl;
            break;
        }
    }

    // Always clean up the individual client socket handle when exiting this connection's thread lifecycle
    closesocket(clientSocket);
}