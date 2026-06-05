#pragma once
#include <windows.h>
#include <memory>
#include <queue>
#include <mutex>
#include <string>
#include "customtypes.h"
#include "VideoTrack.h"

class IApp;
class IRenderer;
class DXShader;
class Sequence;
struct ID3D11DeviceContext;

class PlaybackManager
{
private:
	IApp* appInterface;
	IRenderer* renderer;
	DXShader* videoShader;

	std::queue<DeferredCommand> commandQueue;
	std::mutex queueMutex;

	bool hasPendingBackgroundCmd = false;
	DeferredCommand pendingBackgroundCmd;

	bool hasPendingForegroundCmd = false;
	DeferredCommand pendingForegroundCmd;

	bool hasPendingSequenceCmd = false;
	DeferredCommand pendingSequenceCmd;

	bool hasPendingCoverCmd = false;
	DeferredCommand pendingCoverCmd;

public:
	std::unique_ptr<VideoTrack> backgroundTrack;
	std::unique_ptr<VideoTrack> foregroundTrack;
	std::unique_ptr<VideoTrack> coverTrack;

	bool backgroundActive = false;
	bool foregroundActive = false;
	bool coverActive = false;
	bool transitionMode = false;
	bool stopping = false;
	std::chrono::time_point<std::chrono::steady_clock> transitionStartTime;
	float transitionStartPosition = 0.0f;
	float transitionTargetPosition = 0.0f;
	float transitionDuration = 0.0f;
	Sequence* activeSequence = nullptr;

public:
	PlaybackManager(IApp* appInterface, DXShader* videoShader);
	~PlaybackManager() = default;

	void InitializeVideoTracks();
	
	//PHASE 1 Methods (non-blocking decoding, texture update and layer state management)
	void DecodeVideoFrameTextures(ID3D11DeviceContext* context);
	void UpdateLayers(ID3D11DeviceContext* context);

	void ResetForegroundLayer();
	void AdvanceSequence();
	void ResetCoverLayer();

	void HandlePendingBackgroundCmd(AppState& state);
	void HandleBackgroundEvents();
	void HandlePendingForegroundCmd(AppState& state);
	void HandleSequenceShutdown();
	void HandlePendingCoverCmd(AppState& state);
	void HandlePendingSequenceCmd();
	void PlayTrackOnLayer(int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd = nullptr);
	void PlaySequenceItem(DeferredCommand& cmd);

	//PHASE 2 Methods (Direct3D rendering)
	void RenderLayers(float winW, float winH);
	void ForceStopForegroundLayers();
	void ForceStopBackgroundLayer();

	void EnqueueNetworkCommand(const DeferredCommand& cmd);
	void ProcessDeferredCommands();
};

