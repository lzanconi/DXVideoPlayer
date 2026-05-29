#include "PlaybackManager.h"
#include <iostream>
#include "App.h"
#include "utils.h"
#include "Sequence.h"
#include "DXRenderer.h"
#include "DXShader.h"

PlaybackManager::PlaybackManager(IApp* appInterface, DXShader* videoShader) 
	: appInterface(appInterface), renderer(appInterface->GetAppState().renderer), videoShader(videoShader)
{ }

/*
* Initializes the video tracks for background, foreground, and cover layers based on the video sources loaded in the AppState.
*/
void PlaybackManager::InitializeVideoTracks()
{
	//Retrieves the current application state from the IApp interface, which contains the loaded video sources and sequences
	AppState& state = appInterface->GetAppState();

	//Initializes the video tracks for background, foreground, and cover layers using the first three video sources from the AppState.
	backgroundTrack = std::make_unique<VideoTrack>(state.sources[0]);
	foregroundTrack = std::make_unique<VideoTrack>(state.sources[1]);
	coverTrack = std::make_unique<VideoTrack>(state.sources[2]);

	//Enable blending for the foreground and cover tracks to allow for proper alpha compositing during fade-in and fade-out transitions, 
	//while keeping blending disabled for the background track since it will always be fully opaque.
	backgroundTrack->SetBlending(false);
	foregroundTrack->SetBlending(true);
	coverTrack->SetBlending(true);

	//Starts the playback of the background track immediately without any fade-in effect, using the current time as the starting point for synchronization.
	backgroundTrack->Play(GetTimeStd());

	//Sets the first sequence in the AppState as the active sequence if there are any sequences loaded, allowing it to be triggered later when a PlaySequence command is received.
	if (state.sequences.empty())
	{
		activeSequence = state.sequences[0];
	}
}

/*
* Computes:
*	1.The next video frame for the active tracks (background always, foreground and cover if active)
*	2.The dynamic alpha values for fade-in and fade-out effects
*	3.Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
*/
void PlaybackManager::DecodeVideoFrameTextures(ID3D11DeviceContext* context)
{
	//1.Background video 
	if (backgroundTrack)
	{
		backgroundTrack->UpdateFrame(context);
	}

	//2.Foreground video
	if (foregroundActive && foregroundTrack)
	{
		foregroundTrack->UpdateFrame(context);
	}

	//3.Cover video
	if (coverActive && coverTrack)
	{
		coverTrack->UpdateFrame(context);
	}
}

/*
===========================================================================================================================
PHASE 1: NON-BLOCKING DECODING, TEXTURE UPDATE AND LAYER STATE MANAGEMENT
In PHASE 1:
	-Computes the next video frame for the active tracks (background always, foreground if active)
	-Computes the dynamic alpha values for fade-in and fade-out effects
	-Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
	-Manages the active states of the foreground and cover layers, automatically deactivating them when they have finished playing or completed their fade-out transitions.
	-Manages the automatic advancement of sequence items when a sequence is active

*/
void PlaybackManager::UpdateLayers(ID3D11DeviceContext* context)
{
	AppState& state = appInterface->GetAppState();

	//Decodes the next video frame for the active tracks (background always, foreground and cover if active), updates alpha values and updates their textures for rendering in PHASE 2
	DecodeVideoFrameTextures(context);

	//Manages the active states of the foreground, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetForegroundLayer();

	//Advances to the next sequence item when a sequence is active
	AdvanceSequence();

	//Manages the active states of the cover, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetCoverLayer();

	//Checks if there is a pending command to play a new foreground video and if the foreground layer is not active, and if so, processes the pending command to start playing the new foreground video.
	HandlePendingForegroundCmd(state);

	//Checks if there is a pending command to stop the current sequence and if the foreground layer is not active, and if so, stops the active sequence to prevent it from advancing to the next item in the sequence.
	HandleSequenceShutdown();
}

/*
* Checks if the foreground track is active but has finished playing or completed its fade-out transition, and if so, resets the foreground layer by deactivating it. 
* This ensures that the foreground layer is automatically turned off when it is no longer visible, allowing for proper management of layer states without blocking the main thread.
*/
void PlaybackManager::ResetForegroundLayer()
{
	if (foregroundActive && foregroundTrack && !foregroundTrack->IsActive())
	{
		//Turns off the foreground layer
		foregroundActive = false;
	}
}

/*
* Triggered precisely when the foreground track has finished playing (if active), provided no other override network commands are pending and there is an active sequence. 
*/
void PlaybackManager::AdvanceSequence()
{
	if (!foregroundActive && !hasPendingForegroundCmd && !hasPendingSequenceCmd && activeSequence && activeSequence->isActive)
	{
		activeSequence->AdvanceSequence();
	}
}

/*
* Checks if the cover track is active but has finished playing or completed its fade-out transition, and if so, resets the cover layer by deactivating it.
*/
void PlaybackManager::ResetCoverLayer()
{
	if (coverActive && coverTrack && !coverTrack->IsActive())
	{
		coverActive = false;
	}
}

/*
* Example:
* VIDEO A is playing in the foreground, a new command to play VIDEO B is received:
*	1.Video A starts a forced fade-out 
*	2.While Video A is fading out, Video B must wait in line
*	3.Once Video A finsihes (fgActive = false) it stops, then Video B instantly start plaing (usually with a fading-in)

* Checks if a previous foreground video has just finished (foregroundActive = false) and if there is a pending command to play a new foreground video. 
* If both conditions are met, it processes the pending command by finding the corresponding video source index and instantiating the foreground track with the new video.
*/
void PlaybackManager::HandlePendingForegroundCmd(AppState& state)
{
	if (!foregroundActive && hasPendingForegroundCmd)
	{
		hasPendingForegroundCmd = false;

		int matchIdx = FindVideoSourceIndexByFilename(pendingForegroundCmd.filename, state.sources);
		if (matchIdx != -1)
		{
			// Instantly instantiate Video B and reset its properties ready to be played the next time we enter the Main Loop.
			// The actual playback of Video B will be triggered in the next iteration of the Run loop once the current foreground video has fully finished and fgActive becomes false.
			PlayTrackOnLayer(state, matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &pendingForegroundCmd);
		}
	}
}

/*
* Checks if there is a pending command to stop the current sequence and if the foreground layer is not active. 
* If both conditions are met, it stops the active sequence to prevent it from advancing to the next item in the sequence when we enter the Main Loop.
*/
void PlaybackManager::HandleSequenceShutdown()
{
	if (!foregroundActive && hasPendingForegroundCmd)
	{
		if (activeSequence && activeSequence->isActive)
		{
			// This does not stop the current sequence video immediately, it just stops the sequence preventing it to advance to the next item we enter the Main Loop.
			// The fade out of the current sequence video is called in StopForegroundActivities() when a new foreground video command is received while a sequence is active.
			activeSequence->Stop();
		}
	}
}

/*
* Checks if a previous cover video has just finished (coverActive = false) and if there is a pending command to play a new cover video.
*/
void PlaybackManager::HandlePendingCoverCmd(AppState& state)
{
	if (!coverActive && hasPendingCoverCmd)
	{
		hasPendingCoverCmd = false;
		int matchIdx = FindVideoSourceIndexByFilename(pendingCoverCmd.filename, state.sources);
		if (matchIdx != -1)
		{
			PlayTrackOnLayer(state, matchIdx, coverTrack, coverActive, LayerType::Cover, &pendingCoverCmd);
		}
	}
}

/*
* Checks if a previous foreground video has just finished (foregroundActive = false) and if there is a pending command to play a new sequence.
*/
void PlaybackManager::HandlePendingSequenceCmd()
{
	if (!foregroundActive && hasPendingSequenceCmd)
	{
		hasPendingSequenceCmd = false;
		std::cerr << "[Main Thread] Foreground track cleared perfectly. Booting pending sequence now." << std::endl;

		//Verifies that there is an active sequence
		if (activeSequence)
		{
			//Even though the sequence might already be idle, calling Stop() is a safety measure that forcefully resets its properties
			activeSequence->Stop();
			//Prepares the first item of the sequence to be played in the next iterations of the Run loop
			activeSequence->Play(pendingSequenceCmd.looped);
		}
	}
}

void PlaybackManager::PlayTrackOnLayer(AppState& state, int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
{
	if (videoSourceIdx < 0 || videoSourceIdx >= static_cast<int>(state.sources.size()))
	{
		std::cerr << "Invalid video source index for " << LayerTypeToStr(layerType) << ": " << videoSourceIdx << std::endl;
		return;
	}

	std::cout << "[Main Thread] Swapping " << LayerTypeToStr(layerType) << " layer video to index: "
		<< videoSourceIdx << " (" << state.sources[videoSourceIdx]->filename << ")" << std::endl;

	//Update the video source properties based on the command parameters if provided
	if (cmd)
	{
		state.sources[videoSourceIdx]->fadeInDuration = cmd->fadeInDuration;
		state.sources[videoSourceIdx]->fadeOutDuration = cmd->fadeOutDuration;
		state.sources[videoSourceIdx]->looped = cmd->looped;
	}

	//Set initial alpha to 0 for fade-in effect
	state.sources[videoSourceIdx]->alpha = 0.0f;

	// Dynamically update the passed track unique_ptr pointer structure
	targetTrack = std::make_unique<VideoTrack>(state.sources[videoSourceIdx]);
	targetTrack->SetBlending(true);
	targetTrack->Rewind();
	targetTrack->Play(GetTimeStd());

	// Force wrapper synchronization status explicitly
	targetTrack->SetActive(true);

	// Bootstrap first frame mapping context
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

	// Toggle the specific layer visibility state flag on
	targetActiveFlag = true;
}

/*
===========================================================================================================================
PHASE 2: DIRECT3D RENDERING STAGE 
In PHASE 2:
	-Finally, renders the decoded textures for background layer, foreground and cover layer (if active)

*/
void PlaybackManager::RenderLayers(float winW, float winH)
{
	//Render background video texture
	if (backgroundTrack)
		backgroundTrack->Render(renderer, videoShader, winW, winH);

	//Render foreground video texture
	if (foregroundActive)
	{
		if (!foregroundTrack->IsActive())
		{
			foregroundActive = false;
		}
		else
		{
			foregroundTrack->Render(renderer, videoShader, winW, winH);	
		}
	}

	//Render cover video texture
	if (coverActive)
	{
		if (!coverTrack->IsActive())
		{
			coverActive = false;
		}
		else
		{
			coverTrack->Render(renderer, videoShader, winW, winH);
		}
	}
}
