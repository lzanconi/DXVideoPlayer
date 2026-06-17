#include "NetworkManager.h"
#include "IApp.h"

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cmath>
#include <algorithm>
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
        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Start", "Client background thread started.");
    }

    if (!serverRunning)
    {
        serverRunning = true;
        serverThread = std::thread(&NetworkManager::RunServer, this);
        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Start", "Server listener thread started on port " + std::to_string(listenPort) + ".");
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

    Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "Stop", "All background threads stopped cleanly.");
}

/*
* targetPos -> the target playback position to transition to
* id -> the choreography ID associated with this transition, used for logging and debugging purposes    
*/
void NetworkManager::SetupTransition(float targetPos, float fadeIn, float fadeOut, int id)
{
    std::lock_guard<std::mutex> lock(clientSocketMutex);

    //Assigns the new target pos
    this->transition_target_position = targetPos;
	this->transitionFadeIn = fadeIn;
	this->transitionFadeOut = fadeOut;
    //Redirects the PositionSend pipeline away from sending live video-calculated frames, shifting it instead into Brake-Move-to transition logic loop.
    this->transition_mode_active = true;
    //Before moving to the target position via a smooth curve, the system uses this flag to actively decelerate (brake) the current velocity down to 0.0
    this->stopping_phase = true;
    //This ensures that any completion events intended to execute at the end of a transition, can fire cleanly exactly once when the destination is reached
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
        serverAddr.sin_port = htons(config.target_port);
        inet_pton(AF_INET, config.target_ip.c_str(), &serverAddr.sin_addr);

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

        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunClient", "Client connected to Position Server.");

        PositionSend(clientSocket);

        {
            std::lock_guard<std::mutex> lock(clientSocketMutex);
            closesocket(clientSocket);
            activeClientSocket = (SOCKET)-1;
        }
    }

    WSACleanup();
}

void NetworkManager::PositionSend(SOCKET socket)
{
    Config config = appInterface->GetConfig();
    auto period_duration = std::chrono::milliseconds(config.send_period_ms);

    char msg_buffer[64];

    double scale = config.positions_scale;
    double offset = config.positions_offset;
    float brakeAcceleration = static_cast<float>(config.cover_stop_acceleration);
    float desired_average_speed = static_cast<float>(config.cover_reference_speed);
    double positions_framerate = 60.0;

    auto next_frame = std::chrono::steady_clock::now() + period_duration;
    double current_speed = 0.0;

    while (clientRunning)
    {
        auto now = std::chrono::steady_clock::now();
        auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
        if (time_left.count() > 2)
        {
            std::this_thread::sleep_for(time_left - std::chrono::milliseconds(2));
        }

        while (std::chrono::steady_clock::now() < next_frame)
        {
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
                double elapsed_sec = static_cast<double>(elapsed_ns) / 1000000000.0;

                if (elapsed_sec > 2.0)
                    elapsed_sec = 0.0;

                calc_time = last_video_time + elapsed_sec;
            }
            else
            {
                calc_time = last_video_time;
            }

            double delay_s = config.positions_delay_ms / 1000.0;
            calc_time -= delay_s;

            if (calc_time < 0.0)
                calc_time = 0.0;

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

            if (!this->transition_mode_active && !appInterface->IsCoverActive())
            {
                pos_value = calculated_csv_pos;
                if (appInterface->IsBackgroundPlaying())
                {
                    previousSentPosition = last_known_position;
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
        if (this->transition_mode_active)
        {
            // PHASE 1: BRAKE TO STOP
            if (this->stopping_phase)
            {
                progress_pct = 0.0;
                double brake_dv = brakeAcceleration * dt_sec;
                double newSpeed = current_speed;

                if (current_speed > 0)
                {
                    newSpeed -= brake_dv;
                    if (newSpeed < 0)
                        newSpeed = 0;
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
                    this->stopping_phase = false;

                    transition_start_position = pos_value;
                    transition_start_time = std::chrono::steady_clock::now();
                    transition_target_position = appInterface->GetTransitionPosition();

                    float distance_to_travel = std::abs(this->transition_target_position - transition_start_position);

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

                    /*appTransitionId = appInterface->GetTransitionId();

                    if (appTransitionId >= 0)
                    {
                        char status_buf[256];
                        snprintf(status_buf, sizeof(status_buf), "{\"play_choreography\":%d,\"loop\":false,\"fade_in_seconds\":%.3f}", appTransitionId, (transition_duration_ms / 1000.0));
                        appInterface->HandleNetworkCommand(std::string(status_buf));
                    }*/
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
                    this->transition_mode_active = false;
                    Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "PositionSend", "Transition complete at position: " + std::to_string(transition_target_position));

                    int transitionId = appInterface->GetTransitionId();
                    if (transitionId >= 0)
                    {
                        char status_buf[256];
                        //snprintf(status_buf, sizeof(status_buf), "{\"play_choreography\":%d,\"loop\":false,\"fade_in_seconds\":1.0}", transitionId);
                        snprintf(status_buf, sizeof(status_buf),
                            "{\"play_choreography\":%d,\"loop\":false,\"fade_in_seconds\":%.3f,\"fade_out_seconds\":%.3f}",
                            transitionId, this->transitionFadeIn, this->transitionFadeOut);

                        appInterface->HandleNetworkCommand(std::string(status_buf));
                    }
                }

                double smooth_perc = smoothStep(percentage);
                pos_value = static_cast<float>(transition_start_position * (1.0 - smooth_perc) + transition_target_position * smooth_perc);
                progress_pct = percentage;
            }
        }

        // Direct position fallbacks for stopped background states
        if (!appInterface->IsBackgroundPlaying() && !appInterface->IsCoverActive())
        {
            pos_value = last_known_position;
        }

        // FIX 2: Only cycle historical tracking values sequentially while actively running a transition phase
        if (this->transition_mode_active || appInterface->IsCoverActive())
        {
            previousSentPosition = last_known_position;
        }
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
        if (appInterface->IsCoverActive())
        {
            if (last_known_position == appInterface->GetTransitionPosition() && !sequence_triggered)
            {
                Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "PositionSend", "Cover frame transition finished at position: " + std::to_string(last_known_position));
                sequence_triggered = true;
            }
        }

        // Dispatch raw sequence payload down the active TCP connection channel
        if (len > 0)
        {
            if (send(socket, msg_buffer, len + 1, 0) == -1)
            {
                Logger::LogMessage(MESSAGE_TYPE::ERRORS, "NetworkManager", "PositionSend", "Lost synchronization stream connection link channel.");
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
            Logger::LogMessage(MESSAGE_TYPE::ERRORS, "NetworkManager", "RunServer", "Bind failed on port " + std::to_string(listenPort) + ".");
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

        Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunServer", "Server listening on port " + std::to_string(listenPort) + ".");

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

            char clientIPStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIPStr, INET_ADDRSTRLEN);

            Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "RunServer", "New client connected from: " + std::string(clientIPStr));

            appInterface->SetClientSocket(inboundClient);

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

    DWORD timeout = 1000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    while (serverRunning)
    {
        int bytesReceived = recv(clientSocket, recvBuffer, sizeof(recvBuffer) - 1, 0);
        if (bytesReceived > 0)
        {
            recvBuffer[bytesReceived] = '\0';
            appInterface->HandleNetworkCommand(std::string(recvBuffer));
        }
        else if (bytesReceived == 0)
        {
            Logger::LogMessage(MESSAGE_TYPE::INFO, "NetworkManager", "HandleIncomingConnection", "Client disconnected gracefully.");
            break;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
            {
                continue;
            }
            Logger::LogMessage(MESSAGE_TYPE::ERRORS, "NetworkManager", "HandleIncomingConnection", "Client connection lost abruptly.");
            break;
        }
    }

    closesocket(clientSocket);
}