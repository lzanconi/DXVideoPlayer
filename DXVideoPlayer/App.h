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
class PlaybackManager;
class ConfigManager;	
class Sequence;
class Logger;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;

class App : public IApp
{
public:
	static AppState state;

private:
	bool isFullscreen = false;
	bool fgActive = false;
	bool coverActive = false;
	int clientSocket = -1;	
	PlaybackManager* playbackMgr = nullptr;
	ContentManager* contentMgr = nullptr;
	ConfigManager* configMgr = nullptr;	
	IRenderer* renderer = nullptr;
	DXShader* videoShader = nullptr;
	Logger* logger = nullptr;
	AVBufferRef* hw_ctx;
	AVPacket* raw_packet;
	AVFrame* frame;
	WNDCLASS wndClass = { 0 };
	MSG msg = { 0 };
	HWND window = nullptr;
	// Stores window position before going fullscreen
	RECT windowRect = { 0 };
	WINDOWPLACEMENT windowPlacement = { sizeof(WINDOWPLACEMENT) };

public:
	App(int width, int height);
	~App();

	void Run();
	

	VideoSource* GetBackgroundVideo() override;
	std::vector<float> GetPositions() override;
	double GetLastPTS() override;
	int64_t GetBGCaptureTimeNS() override;
	AppState& GetAppState() override;
	Config& GetConfig() override;
	PlaybackManager* GetPlaybackManager() override;
	bool IsBackgroundPlaying() override;
	bool InTransitionMode() override;
	bool IsStoppingPhase() override;
	bool IsCoverActive() override;
	float GetTransitionPosition() override;
	int GetTransitionId() override;
	void SetClientSocket(int socket) override;
	void HandleNetworkCommand(const std::string& jsonStr) override;
	void SendTCPMessage(const std::string& message) override;	
	void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "") override;

private:
	void LoadVideoSources(ID3D11Device* device, ID3D11DeviceContext* context);
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	void ToggleFullscreen(HWND hwnd);
};

