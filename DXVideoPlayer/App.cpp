#include "App.h"
#include "IRenderer.h"
#include "DXRenderer.h"
#include "DXShader.h"
#include "VideoSource.h"
#include "utils.h"
#include "NetworkManager.h"
#include "ContentManager.h"
#include <iostream>
#include <json.hpp>
#include "Sequence.h"

using json = nlohmann::json;

// Initialize the static AppState member
AppState App::state;

App::App(int width, int height)
{
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

	InitVideoTracks();

	bgTrack->Play(GetTimeStd());

	ShowWindow(window, SW_SHOW);
	ToggleFullscreen(window);

    state.networkMgr = new NetworkManager("127.0.0.1", 5555, this);
	state.networkMgr->Start();

    if (!state.sequences.empty())
    {
        activeSequence = state.sequences[0];
	}
}

App::~App()
{
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

		//Processes any pending commands that were safely enqueued by the NetworkManager thread through the HandleNetworkCommand method.
		//This ensures that all Direct3D resource manipulations and state changes triggered by network commands are executed in the main thread
		ProcessDeferredCommands();
        
		//DEBUG PURPOSE: 
        //Pressing spacebar will trigger the playback of a foreground video
        if (spaceBarPressed)
        {
			spaceBarPressed = false;
            UpdateAndPlayFG(1);
        }

		//Pressing 'T' key will trigger an immediate forced fade-out of the foreground video if it is active
        if (tKeyPressed)
        {
            tKeyPressed = false;
            StopForegroundActivities();
		}

        // =================================================================
        // PHASE 1: NON-BLOCKING UPDATE STAGE 
        // In PHASE 1:
		// -Decodes the next video frame for the active tracks (background always, foreground if active)
		// -Computes the dynamic alpha values for fade-in and fade-out effects 
		// -Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
        // =================================================================

		//Retrieves the Direct3D device context from the renderer, which is required for updating video frames and copying decoded data into GPU textures.
        ID3D11DeviceContext* ctx = renderer->GetContext();

        //Updates the background video track (decoding, alpha computation, texture updates)
        if (bgTrack) 
            bgTrack->UpdateFrame(ctx);
        
		//If a foregreound track is active, updates it as well (decoding, alpha computation, texture updates)
        if (fgActive && fgTrack) 
            fgTrack->UpdateFrame(ctx);

		//If the foreground track is active but has reached the end of the video or completed its fade-out, it will automatically deactivate and stop rendering
        if (fgActive && fgTrack && !fgTrack->IsActive())
        {
            fgActive = false; // Layer is now clear

            // Advance to next video item ONLY if we aren't waiting to start a whole new sequence
            if (!hasPendingSeqCommand && activeSequence && activeSequence->isActive)
            {
                activeSequence->AdvanceSequence();
            }
        }

        //VIDEO A is playing in the foreground, a new command to play VIDEO B is received.
        //1.Video A starts a forced fade-out 
        //2.While Video A is fading out, Video B must wait in line
        //3.Once Video A finsihes (fgActive = false) it stops, then Video B instantly start plaing (usually with a fading-in)
		//It detects the precise moment an old foreground video finishes (fgActive becomes false)  
        //
        //This condition checks if a previous foreground video has just finished (fgActive = false) and if there is a pending foreground 
        //command waiting to be played (hasPendingFGCommand = true).
        if (!fgActive && hasPendingFGCommand)
        {
            hasPendingFGCommand = false;
            int matchIdx = FindVideoSourceIndexByFilename(pendingFGCommand.filename, state.sources);
            if (matchIdx != -1)
            {
                UpdateAndPlayFG(matchIdx, &pendingFGCommand);
            }
        }
        
        if (!fgActive && hasPendingSeqCommand)
        {
            hasPendingSeqCommand = false; // Clear flag
            std::cout << "[Main Thread] Foreground track cleared perfectly. Booting pending sequence now." << std::endl;

            if (activeSequence)
            {
                activeSequence->Stop();
                activeSequence->Play(pendingSeqCommand.looped);
            }
        }

        // =================================================================
        // PHASE 2: DIRECT3D RENDERING STAGE 
		// 1.Retrieve the current dimensions of the application window's client 
        //   area to ensure that the video content is rendered correctly 
        // 2.Prepares the Direct3D rendering context and clears the back buffer to solid black
        // 3.Finally, renders the decoded textures for background layer and foreground layer (if active)
        // =================================================================
        
        //Get current window dimensions
        RECT rc; 
        GetClientRect(window, &rc);
        float w = (float)(rc.right - rc.left);
        float h = (float)(rc.bottom - rc.top);

        //Prepare Direct3D rendering context
		renderer->BeginFrame();

        //Render background layer
        if (bgTrack) bgTrack->Render(renderer, videoShader, w, h);

		//If foreground layer is active, render it on top of the background layer
        if (fgActive)
        {
			//If the foreground track has become inactive (e.g., video ended or finished fading out) during this frame, it will stop rendering and reset fgActive to false
            if (!fgTrack->IsActive())
            {
                fgActive = false;
            }
            else
            {
                //Render foreground layer
                fgTrack->Render(renderer, videoShader, w, h);
            }
        }

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
    return bgTrack ? bgTrack->GetSource() : nullptr;
}

std::vector<float> App::GetPositions()
{
    return bgTrack->GetSource()->positions;
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
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"stop\"}";
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
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(cmd);

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
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(cmd);

            commandProcessed = true;
            responseMessage = "{\"status\":\"acknowledged\",\"command\":\"play_sequence\"}";
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

void App::TriggerSequenceItem(const DeferredCommand& cmd)
{
	int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);
    if (matchIdx != -1)
    {
        UpdateAndPlayFG(matchIdx, const_cast<DeferredCommand*>(&cmd));
    }
    else
    {
        std::cerr << "[App] Sequence error: Video file not found: " << cmd.filename << std::endl;
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

void App::InitVideoTracks()
{
    bgTrack = std::make_unique<VideoTrack>(state.sources[0]);
    fgTrack = std::make_unique<VideoTrack>(state.sources[1]);


    //fgTrack->GetSource()->fadeInDuration = 0.0f; // Background video starts immediately without fading
    //fgTrack->GetSource()->fadeOutDuration = 0.0f; // Background video does not fade out naturally

	//Disable blending for the background track since it will always be fully opaque
    bgTrack->SetBlending(false);
    //Enable blending for the foreground track to allow for proper alpha compositing during fade-in and fade-out transitions
    fgTrack->SetBlending(true);
}

void App::ProcessDeferredCommands()
{
    std::queue<DeferredCommand> localQueue;

    std::lock_guard<std::mutex> lock(queueMutex);
    if (commandQueue.empty())
        return;

    std::swap(commandQueue, localQueue);

    while (!localQueue.empty())
    {
        DeferredCommand cmd = localQueue.front();
        localQueue.pop();

        switch (cmd.type)
        {
        case NetworkCommandType::Stop:
        {
            std::cout << ">>> [Main Thread] Processing deferred 'stop' action." << std::endl;
            StopForegroundActivities();
            break;
        }

        case NetworkCommandType::PlayForeground:
        {
            std::cout << "[Main Thread] Processing deferred 'play_foreground' action: " << cmd.filename << std::endl;
            int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);

            if (matchIdx != -1)
            {
                if (fgActive && fgTrack && fgTrack->IsActive())
                {
                    pendingFGCommand = cmd;
                    hasPendingFGCommand = true;
                    fgTrack->StartForcedFadeOut();
                }
                else
                {
                    UpdateAndPlayFG(matchIdx, &cmd);
                }
            }
            break;
        }

        case NetworkCommandType::PlaySequence:
        {
            std::cout << "[Main Thread] Processing deferred 'play_sequence' action: " << cmd.filename << std::endl;

            // Check if a foreground track is actively displaying
            if (fgActive && fgTrack && fgTrack->IsActive())
            {
                std::cout << "[Main Thread] Foreground is active. Storing pending sequence and forcing fade out." << std::endl;

                pendingSeqCommand = cmd;
                hasPendingSeqCommand = true;

                // Stop any existing sequence logic running without clearing the pending flag
                if (activeSequence && activeSequence->isActive)
                {
                    activeSequence->Stop();
                }

                // Force ONLY the current video track to fade out
                fgTrack->StartForcedFadeOut();
            }
            else
            {
                // Clean start: nothing is occupying the foreground track layer
                hasPendingSeqCommand = false;
                if (activeSequence)
                {
                    activeSequence->Stop();
                    activeSequence->Play(cmd.looped);
                }
            }
            break;
        }
        }
    }
}

void App::StopForegroundActivities()
{
    // Clear any pending video request if a global stop is called
    hasPendingFGCommand = false;
	hasPendingSeqCommand = false;

    if (activeSequence && activeSequence->isActive)
    {
        std::cout << "[Main Thread] Stopping active sequence: " << activeSequence->name << std::endl;
        activeSequence->Stop();
	}

    if (fgActive && fgTrack)
    {
        fgTrack->StartForcedFadeOut();
    }
}

void App::UpdateAndPlayFG(int videoSourceIdx, DeferredCommand* cmd)
{
    if (videoSourceIdx < 0 || videoSourceIdx >= static_cast<int>(state.sources.size()))
    {
        std::cerr << "Invalid video source index: " << videoSourceIdx << std::endl;
        return;
    }

    std::cout << "[Main Thread] Swapping foreground video to index: " << videoSourceIdx << " (" << state.sources[videoSourceIdx]->filename << ")" << std::endl;

    if (cmd)
    {
        state.sources[videoSourceIdx]->fadeInDuration = cmd->fadeInDuration;
        state.sources[videoSourceIdx]->fadeOutDuration = cmd->fadeOutDuration;
        state.sources[videoSourceIdx]->looped = cmd->looped;
    }

    state.sources[videoSourceIdx]->alpha = 0.0f;

    fgTrack = std::make_unique<VideoTrack>(state.sources[videoSourceIdx]);
    fgTrack->SetBlending(true);
    fgTrack->Rewind();
    fgTrack->Play(GetTimeStd());

    // Force wrapper synchronization status explicitly
    fgTrack->SetActive(true);

    ID3D11DeviceContext* ctx = renderer->GetContext();
    state.sources[videoSourceIdx]->GetNextFrame(ctx);

    if (state.sources[videoSourceIdx]->isFadingIn)
    {
        state.sources[videoSourceIdx]->ComputeFadeIn();
    }
    else if (state.sources[videoSourceIdx]->fadeInDuration > 0.0f)
    {
        state.sources[videoSourceIdx]->alpha = 0.0f;
    }

    fgActive = true;
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
