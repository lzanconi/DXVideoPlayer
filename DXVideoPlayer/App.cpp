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

using json = nlohmann::json;

// Initialize the static AppState member
AppState App::state;

App::App(int width, int height)
{
	configMgr = new ConfigManager();
	configMgr->LoadConfig(".\\conf.json"); 

    contentMgr = new ContentManager(this);
    contentMgr->LoadContentsFromFolder(".\\Videos");

    if (contentMgr->GetVideoContents().empty())
    {
        /*std::cerr << "No .mp4 files found." << std::endl;*/
		MessageBoxA(nullptr, "No .mp4 files found in the Videos folder.", "Error", MB_ICONERROR);
    }

    wndClass.lpfnWndProc = WndProc; 
    wndClass.lpszClassName = L"VP"; 
    wndClass.hInstance = GetModuleHandle(NULL);
    RegisterClass(&wndClass);
    window = CreateWindow(L"VP", L"OOP Video Player", WS_OVERLAPPEDWINDOW, 100, 100, width, height, 0, 0, wndClass.hInstance, this);

    DXRenderer* dxRenderer = new DXRenderer();
    dxRenderer->Initialize(window);
    renderer = dxRenderer;
    state.renderer = renderer;

    videoShader = new DXShader();
    videoShader->LoadFromFile(renderer->GetDevice(), L"shaders.hlsl");

	LoadVideoSources(renderer->GetDevice(), renderer->GetContext());

	playbackMgr = new PlaybackManager(this, videoShader);
	playbackMgr->InitializeVideoTracks();

	contentMgr->LoadSequences(".\\Videos", playbackMgr);

	ShowWindow(window, SW_SHOW);
	ToggleFullscreen(window);

    state.networkMgr = new NetworkManager("127.0.0.1", 5555, this);
	state.networkMgr->Start();
}

App::~App()
{
    if (configMgr)
		delete configMgr;

    if (playbackMgr)
		delete playbackMgr;

    if (state.networkMgr)
    {
        state.networkMgr->Stop();
        delete state.networkMgr;
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
        
		//DEBUG PURPOSE: 
        //Pressing spacebar will trigger the playback of a foreground video
        if (spaceBarPressed)
        {
			spaceBarPressed = false;
			playbackMgr->PlayTrackOnLayer(1, playbackMgr->foregroundTrack, playbackMgr->foregroundActive, LayerType::Foreground);
        }

		//Pressing 'T' key will trigger an immediate forced fade-out of the foreground video if it is active
        if (tKeyPressed)
        {
            tKeyPressed = false;
			playbackMgr->ForceStopForegroundLayers();   
		}

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
            std::cerr << "App::HandleCommand failed to send response back to client." << std::endl;
        }
    }
}

VideoSource* App::GetBackgroundVideo()
{
    return playbackMgr->backgroundTrack ? playbackMgr->backgroundTrack->GetSource() : nullptr;
}

std::vector<float> App::GetPositions()
{
    return playbackMgr->backgroundTrack->GetSource()->positions;
}

double App::GetLastPTS()
{
	return state.sources[0]->lastPTS;
}

int64_t App::GetBGCaptureTimeNS()
{
	return state.sources[0]->bg_capture_time_ns;
}

AppState& App::GetAppState()
{
	return state;
}

Config& App::GetConfig()
{
	return configMgr->config;  
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
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_sequence\"}";
        }

		//PLAY COVER COMMAND:
        if (j.contains("play_cover"))
        {
            cmd.type = NetworkCommandType::PlayCover;
            cmd.filename = j["play_cover"].get<std::string>();

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

        //If the command was successfully parsed and recognized, send an acknowledgment response back to the client
        if (clientSocket > 0 && commandProcessed)
        {
            SendTCPMessage(responseMessage);
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error handling network command: " << ex.what() << std::endl;
	}
}

void App::LoadVideoSources(ID3D11Device* device, ID3D11DeviceContext* context)
{
    for (const auto& videoContent : contentMgr->GetVideoContents())
    {
        VideoSource* videoSource = new VideoSource();
        if (videoSource->OpenFile(videoContent.filename, renderer->GetDevice(), renderer->GetContext()))
        {
            videoSource->fadeInDuration = videoContent.fadeInDuration;
            videoSource->fadeOutDuration = videoContent.fadeOutDuration;
            videoSource->looped = videoContent.looped;
            videoSource->positions = videoContent.positions;
			videoSource->events = videoContent.events;
            state.sources.push_back(videoSource);
        }
        else
        {
            std::cerr << "Failed to open video: " << videoContent.filename << std::endl;
            delete videoSource;
        }
    }

    for (const auto& source : state.sources)
    {
        std::cout << "VideoSource: " << source->filename << " Duration: " << GetDurationMinSec(static_cast<int>(source->duration)) << std::endl;
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
        spaceBarPressed = true;
    
    if (msg == WM_KEYDOWN && wp == 'F')
        ToggleFullscreen(hwnd);

    if (msg == WM_KEYDOWN && wp == 'T')
		tKeyPressed = true;

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
