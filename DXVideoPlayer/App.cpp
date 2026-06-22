#include "App.h"
#include "IRenderer.h"
#include "DXRenderer.h"
#include "DXShader.h"
#include "VideoSource.h"
#include "utils.h"
#include "NetworkManager.h"
#include "ContentManager.h"
#include "ConfigManager.h"  
#include <iostream>
#include <json.hpp>
#include "Sequence.h"
#include "PlaybackManager.h"
#include "Logger.h"

using json = nlohmann::json;

// Initialize the static AppState member
AppState App::state;

App::App(int width, int height)
{
	configMgr = new ConfigManager();
	configMgr->LoadConfig(".\\conf.json"); 

    contentMgr = new ContentManager(this);
	contentMgr->LoadContents();

    if (contentMgr->GetVideoContentsMap().empty())
    {
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "App", "App", "No .mp4 files found in the Videos folder");    
    }

    wndClass.lpfnWndProc = WndProc; 
    wndClass.lpszClassName = L"VP"; 
    wndClass.hInstance = GetModuleHandle(NULL);
    RegisterClass(&wndClass);
    window = CreateWindow(L"VP", L"DirectX Video Player", WS_OVERLAPPEDWINDOW, 100, 100, width, height, 0, 0, wndClass.hInstance, this);

    DXRenderer* dxRenderer = new DXRenderer();
    dxRenderer->Initialize(window);
    renderer = dxRenderer;
    state.renderer = renderer;

    videoShader = new DXShader();
    videoShader->LoadFromFile(renderer->GetDevice(), L"shaders.hlsl");

	contentMgr->LoadVideoSources(renderer->GetDevice(), renderer->GetContext());

	playbackMgr = new PlaybackManager(this, videoShader);
	playbackMgr->InitializeVideoTracks();

	contentMgr->LoadSequences("prod", playbackMgr);

	ShowWindow(window, SW_SHOW);
	ToggleFullscreen(window);

    state.networkMgr = new NetworkManager(this);
	state.networkMgr->Start();
}

App::~App()
{
    if (state.networkMgr)
    {
        state.networkMgr->Stop();
        delete state.networkMgr;
        state.networkMgr = nullptr;
    }

    if (configMgr)
    {
        delete configMgr;
		configMgr = nullptr;
    }

    if (playbackMgr)
    {
        delete playbackMgr;
		playbackMgr = nullptr;
    }

    for (auto source : state.sources)
        delete source;

    for (auto sequence : state.sequences)
		delete sequence;

    if (renderer)
        delete renderer;

    if (videoShader)
		delete videoShader;
}

/*
* MAIN LOOP 
* 
*/
void App::Run()
{
    //Continously runs until a WM_QUIT message is received (application closed or destroyed)
    while (msg.message != WM_QUIT)
    {
        //Listens for Windows message (e.g. keyboard input) and routes them to the WndProc method
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            DispatchMessage(&msg);
            continue;
        }

		playbackMgr->ProcessDeferredCommands();
        
        // =================================================================
        // PHASE 1: NON-BLOCKING DECODING AND TEXTURE UPDATE
        // In PHASE 1:
		// -Decodes the next video frame for the active tracks (background always, foreground if active)
		// -Computes the dynamic alpha values for fade-in and fade-out effects 
		// -Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
        // =================================================================

		//Retrieves the Direct3D device context from the renderer, which is required for updating video frames and copying decoded data into GPU textures.
        ID3D11DeviceContext* ctx = renderer->GetContext();

		playbackMgr->UpdateLayers(ctx);

        //Get current window dimensions
        RECT rc; 
        GetClientRect(window, &rc);
        float w = (float)(rc.right - rc.left);
        float h = (float)(rc.bottom - rc.top);

        //Prepare Direct3D rendering context
		renderer->BeginFrame();

		playbackMgr->RenderLayers(w, h);

		//Swap chains and present the rendered frame to the screen
        renderer->EndFrame();
	}
}

void App::SendTCPMessage(const std::string& message)
{
    if (clientSocket > 0)
    {
        int bytesSent = send(static_cast<SOCKET>(clientSocket), message.c_str(), static_cast<int>(message.length()), 0);
        if (bytesSent == -1)
        {
			Logger::LogMessage(MESSAGE_TYPE::ERRORS, "App", "SendTCPMessage", "Failed to send response back to client");
        }
    }
}

VideoSource* App::GetBackgroundVideo()
{
    return playbackMgr->backgroundTrack->GetSource();
}

std::vector<float> App::GetPositions()
{
	return playbackMgr->backgroundTrack->GetSource()->positions;
}

double App::GetLastPTS()
{
    return playbackMgr->backgroundTrack->GetSource()->lastPTS;
}

int64_t App::GetBGCaptureTimeNS()
{
    return playbackMgr->backgroundTrack->GetSource()->bg_capture_time_ns;
}

AppState& App::GetAppState()
{
	return state;
}

Config& App::GetConfig()
{
	return configMgr->config;  
}

PlaybackManager* App::GetPlaybackManager()
{
    return playbackMgr;
}

bool App::IsBackgroundPlaying()
{
    if (!playbackMgr || !playbackMgr->backgroundTrack) 
        return false;

    return playbackMgr->backgroundActive &&
        (playbackMgr->backgroundTrack->state == VideoTrackState::Playing ||
            playbackMgr->backgroundTrack->state == VideoTrackState::FadingIn);
}

bool App::InTransitionMode()
{
    if (!state.networkMgr) 
        return false;

    return state.networkMgr->IsTransitionActive();
}

bool App::IsStoppingPhase()
{
    if (!state.networkMgr) 
        return false;
    
    return state.networkMgr->IsStoppingPhase();
}

bool App::IsCoverActive()
{
    return playbackMgr ? playbackMgr->coverActive : false;
}

float App::GetTransitionPosition()
{
    if (!state.networkMgr) 
        return 0.0f;
    
    return state.networkMgr->GetTransitionTargetPosition();
}

int App::GetTransitionId()
{
	return state.transitionId;
}

void App::SetClientSocket(int socket)
{
	clientSocket = socket;
}

void App::HandleNetworkCommand(const std::string& jsonStr)
{
    try
    {
        //Parse the json string received from the NetworkManager into a JSON object for easy access to its properties
        auto j = json::parse(jsonStr);
        std::string filename = "";
        bool commandProcessed = false;
        std::string responseMessage = "{\"status\":\"ok\"}";

        //Prepare a DeferredCommand struct to store the parsed command information that will be safely passed to the main thread for execution
        DeferredCommand cmd;

		//STOP COMMAND:
        if (j.contains("stop"))
        {
            //Set the command type to Stop, which will be used in the main thread to determine which action to execute
            cmd.type = NetworkCommandType::Stop;

            //Safely enqueue the command into the shared command queue with proper locking to ensure thread safety
			playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"stop\"}";
        }

		//PLAY BACKGROUND COMMAND:
        if (j.contains("play_background"))
        {
            //Set the command type to PlayBackground, which will be used in the main thread to determine which action to execute
            cmd.type = NetworkCommandType::PlayBackground;
            //Extract the filename parameter from the JSON command, which specifies which video to play in the background
            cmd.filename = j["play_background"].get<std::string>();
            if (j.contains("fade_in_seconds"))
            {
                cmd.fadeInDuration = j["fade_in_seconds"].get<float>();
            }
            if (j.contains("fade_out_seconds"))
            {
                cmd.fadeOutDuration = j["fade_out_seconds"].get<float>();
            }
            if (j.contains("loop"))
            {
                cmd.looped = j["loop"].get<bool>();
            }
            //Safely enqueue the command into the shared command queue with proper locking to ensure thread safety
            playbackMgr->EnqueueNetworkCommand(cmd);
            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_background\"}";
		}

		//PLAY FOREGROUND COMMAND:
        if (j.contains("play_foreground"))
        {
            //Set the command type to PlayForeground, which will be used in the main thread to determine which action to execute
            cmd.type = NetworkCommandType::PlayForeground;
            //Extract the filename parameter from the JSON command, which specifies which video to play in the foreground
            cmd.filename = j["play_foreground"].get<std::string>();

            if (j.contains("fade_in_seconds"))
            {
                cmd.fadeInDuration = j["fade_in_seconds"].get<float>();
            }

            if (j.contains("fade_out_seconds"))
            {
                cmd.fadeOutDuration = j["fade_out_seconds"].get<float>();
            }

            if (j.contains("loop"))
            {
                cmd.looped = j["loop"].get<bool>();
            }

            //Safely enqueue the command into the shared command queue with proper locking to ensure thread safety
			playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_background\"}";
        }

		//PLAY SEQUENCE COMMAND:
        if (j.contains("play_sequence"))
        {
            cmd.type = NetworkCommandType::PlaySequence;
            cmd.filename = j["play_sequence"].get<std::string>();

			//Initially set fadeInDuration to -1 to indicate that the sequence should use the default fade-in durations specified for each item, but allow it to be overridden by an optional parameter in the command
            cmd.fadeInDuration = -1.0;

            //Safely enqueue the command into the shared command queue with proper locking to ensure thread safety
			playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_sequence\"}";
        }

		//PLAY COVER COMMAND:
        if (j.contains("play_cover"))
        {
            cmd.type = NetworkCommandType::PlayCover;
			const Config& config = GetConfig();
            //Because we use the cover file from config, we override any filename sent in the command
            //cmd.filename = j["play_cover"].get<std::string>();
			cmd.filename = GetFilenameFromPath(config.cover_filename);

            if (j.contains("fade_in_seconds")) {
                cmd.fadeInDuration = j["fade_in_seconds"].get<float>();
            }
            if (j.contains("fade_out_seconds")) {
                cmd.fadeOutDuration = j["fade_out_seconds"].get<float>();
            }
            if (j.contains("loop")) {
                cmd.looped = j["loop"].get<bool>();
            }

			playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_cover\"}";
        }

		//HIDE CODER COMMAND:
        if (j.contains("hide_cover"))
        {
            cmd.type = NetworkCommandType::HideCover;

            // Safely enqueue the command with thread protection
            playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"hide_cover\"}";
        }

		//PLAY CHOREOGRAPHY COMMAND:
        if (j.contains("play_choreography"))
        {
            std::string cmdStr = j.dump();
            cmd.type = NetworkCommandType::PlayChoreography;
			cmd.filename = j["play_choreography"].get<std::string>();

            if (j.contains("id")) {
                cmd.choreoID = j["id"].get<int>();
			}
            if (j.contains("fade_in_seconds")) {
                cmd.fadeInDuration = j["fade_in_seconds"].get<float>();
			}
            if (j.contains("fade_out_seconds")) {
                cmd.fadeOutDuration = j["fade_out_seconds"].get<float>();
            }
            if (j.contains("fg_fade_out_seconds")) {
                cmd.fgFadeOutDuration = j["fg_fade_out_seconds"].get<float>();
			}
            if (j.contains("loop")) {
                cmd.looped = j["loop"].get<bool>();
            }
            if (j.contains("force_cover_on_exit")) {
                cmd.forceCoverOnExit = j["force_cover_on_exit"].get<bool>();
			}

            playbackMgr->EnqueueNetworkCommand(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_choreography\"}";
        }

        //If the command was successfully parsed and recognized, send an acknowledgment response back to the client
        if (clientSocket > 0 && commandProcessed)
        {
            SendTCPMessage(responseMessage);
        }
    }
    catch (const std::exception& ex)
    {
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "App", "HandleNetworkCommand", "Error handling network command: " + std::string(ex.what()));
	}
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->HandleMessage(hwnd, msg, wp, lp);

    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) 
        PostQuitMessage(0);

    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    
    if (msg == WM_KEYDOWN && wp == VK_SPACE)
    {
		playbackMgr->PlayTrackOnLayer("Cover_Toyota.mp4", playbackMgr->foregroundTrack, playbackMgr->foregroundActive, LayerType::Foreground);
		return 0;
    }

    if (msg == WM_KEYDOWN && wp == 'F')
        ToggleFullscreen(hwnd);

    if (msg == WM_KEYDOWN && wp == 'T')
        playbackMgr->ForceStopForegroundLayers(1.0f);

    if (msg == WM_SIZE && renderer->GetSwapChain()) 
        renderer->Resize(0, 0);

    return DefWindowProc(hwnd, msg, wp, lp);
}

void App::ToggleFullscreen(HWND hwnd)
{
    isFullscreen = !isFullscreen;
    if (isFullscreen) {
        // Save windowed placement and style, then go borderless fullscreen
        GetWindowPlacement(hwnd, &windowPlacement);
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        while (ShowCursor(FALSE) >= 0);

    }
    else {
        // Restore windowed style and placement
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &windowPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        while (ShowCursor(TRUE) < 0);
    }
}
