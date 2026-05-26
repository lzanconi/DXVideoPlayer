#pragma once
#include <windows.h>
#include "customtypes.h"
#include <string>
#include <mutex>
#include <queue>
#include "IApp.h"
#include "VideoTrack.h"

class IRenderer;
class VideoSource;
class ContentManager;
class DXShader;
class NetworkManager;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;

class App : public IApp
{
public:
	static AppState state;

private:
	bool isFullscreen = false;
	bool spaceBarPressed = false;
	bool tKeyPressed = false;
	bool fgActive = false;
	int clientSocket = -1;	
	ContentManager* contentMgr = nullptr;
	IRenderer* renderer = nullptr;
	DXShader* videoShader = nullptr;
	AVBufferRef* hw_ctx;
	AVPacket* raw_packet;
	AVFrame* frame;
	WNDCLASS wndClass = { 0 };
	MSG msg = { 0 };
	HWND window = nullptr;
	// Stores window position before going fullscreen
	RECT windowRect = { 0 };
	WINDOWPLACEMENT windowPlacement = { sizeof(WINDOWPLACEMENT) };

	std::unique_ptr<VideoTrack> bgTrack;
	std::unique_ptr<VideoTrack> fgTrack;

	//Deferred command queue and mutex for thread-safe communication between the NetworkManager thread and the main thread
	std::queue<DeferredCommand> commandQueue;

	//Mutex to protect access to the command queue when enqueuing commands from the NetworkManager thread and processing them in the main thread
	std::mutex queueMutex;

public:
	App(int width, int height);
	~App();

	void Run();
	void SendTCPMessage(const std::string& message);
	

	VideoSource* GetBackgroundVideo() override;
	std::vector<float> GetPositions() override;
	double GetLastPTS() override;
	int64_t GetBGCaptureTimeNS() override;
	void SetClientSocket(int socket) override;
	void HandleNetworkCommand(const std::string& jsonStr) override;

private:
	void LoadVideoSources(ID3D11Device* device, ID3D11DeviceContext* context);
	void InitVideoTracks();
	void ProcessDeferredCommands();
	void StopForegroundActivities();
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	void ToggleFullscreen(HWND hwnd);
};

