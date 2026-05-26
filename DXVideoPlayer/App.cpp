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

using json = nlohmann::json;

// Initialize the static AppState member
AppState App::state;

App::App(int width, int height)
{
    ContentManager contentMgr;
    contentMgr.LoadVideoContentFromFolder(".\\Videos");
    if (contentMgr.GetVideoContents().empty())
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

    for (const auto& videoContent : contentMgr.GetVideoContents())
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

    bgTrack = std::make_unique<VideoTrack>(state.sources[0]);
    fgTrack = std::make_unique<VideoTrack>(state.sources[1]);


    //fgTrack->GetSource()->fadeInDuration = 0.0f; // Background video starts immediately without fading
    //fgTrack->GetSource()->fadeOutDuration = 0.0f; // Background video does not fade out naturally

    bgTrack->SetBlending(false);
    fgTrack->SetBlending(true);

	bgTrack->Play(GetTimeStd());

	ShowWindow(window, SW_SHOW);
	ToggleFullscreen(window);

    state.networkMgr = new NetworkManager("127.0.0.1", 5555, this);
	state.networkMgr->Start();
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

    if (renderer)
        delete renderer;

    if (videoShader)
		delete videoShader;
}

void App::Run()
{
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            DispatchMessage(&msg);
            continue;
        }

		ProcessDeferredCommands();

        if (spaceBarPressed)
        {
			spaceBarPressed = false;
            fgActive = true;
            fgTrack->Rewind();
            fgTrack->Play(GetTimeStd());
			//fgTrack->StartFadeIn();
        }

        RECT rc; 
        GetClientRect(window, &rc);
        float w = (float)(rc.right - rc.left);
        float h = (float)(rc.bottom - rc.top);

		renderer->BeginFrame();
		bgTrack->Render(renderer, videoShader, w, h);

        if (fgActive)
        {
            // If the foreground track finishes playback naturally, flag active rendering loop to false
            if (!fgTrack->IsActive())
            {
                fgActive = false;
            }
            else
            {
                fgTrack->Render(renderer, videoShader, w, h);
            }
        }
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

void App::ProcessDeferredCommands()
{
    //To minimize the time spent holding the queue mutex, we swap the main command queue with a local queue and process 
    //the commands outside of the locked section.
    std::queue<DeferredCommand> localQueue;

    //Lock the queue mutex to safely access and swap the command queue
    std::lock_guard<std::mutex> lock(queueMutex);
    if (commandQueue.empty())
        return;

    //Swap the main command queue with an empty local queue to quickly transfer ownership of the pending commands
    std::swap(commandQueue, localQueue);

    while (!localQueue.empty())
    {
        DeferredCommand cmd = localQueue.front();
        localQueue.pop();

        //Process the command based on its type, executing the corresponding actions in the main thread's context
        switch (cmd.type)
        {
            //STOP COMMAND: 
            //Triggers an immediate fade-out of any active foreground video and halts any ongoing sequences
            case NetworkCommandType::Stop:
            {
                std::cout << ">>> [Main Thread] Processing deferred 'stop' action." << std::endl;
                StopForegroundActivities();
                
                break;
            }
        }
    }
}

void App::StopForegroundActivities()
{
    if (fgActive && fgTrack)
    {
        fgTrack->StartForcedFadeOut();
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
		StopForegroundActivities();

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
