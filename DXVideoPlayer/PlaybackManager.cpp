#include "PlaybackManager.h"
#include <iostream>
#include <format>
#include "App.h"
#include "utils.h"
#include "Sequence.h"
#include "DXRenderer.h"
#include "DXShader.h"
#include "Logger.h"
#include "NetworkManager.h"

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
	std::string autoRunFilename = appInterface->GetConfig().autorun_filename;
	autoRunFilename = GetFilenameFromPath(autoRunFilename);

	//Initializes the video tracks for background, foreground, and cover layers using the first three video sources from the AppState.
	backgroundTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);
	foregroundTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);
	coverTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);

	//Enable blending for the foreground and cover tracks to allow for proper alpha compositing during fade-in and fade-out transitions, 
	//while keeping blending disabled for the background track since it will always be fully opaque.
	backgroundTrack->SetBlending(true);
	foregroundTrack->SetBlending(true);
	coverTrack->SetBlending(true);

	backgroundActive = true;
	backgroundTrack->Looped(true);

	//Starts the playback of the background track immediately without any fade-in effect, using the current time as the starting point for synchronization.
	backgroundTrack->Play(GetTimeStd());

	//Sets the first sequence in the AppState as the active sequence if there are any sequences loaded, allowing it to be triggered later when a PlaySequence command is received.
	if (!state.sequences.empty())
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

	HandleBackgroundEvents();
	
	HandlePendingBackgroundCmd(state);

	//Manages the active states of the foreground, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetForegroundLayer();

	//Advances to the next sequence item when a sequence is active
	AdvanceSequence();

	//Manages the active states of the cover, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetCoverLayer();

	HandlePendingChoreographyCmd(state);

	//Checks if there is a pending command to play a new cover video and if the cover layer is not active, and if so, processes the pending command to start playing the new cover video.
	HandlePendingCoverCmd(state);

	//Checks if there is a pending command to play a new foreground video and if the foreground layer is not active, and if so, processes the pending command to start playing the new foreground video.
	HandlePendingForegroundCmd(state);

	//Checks if there is a pending command to stop the current sequence and if the foreground layer is not active, and if so, stops the active sequence to prevent it from advancing to the next item in the sequence.
	HandleSequenceShutdown();

	//Checks if there is a pending command to play a new sequence and if the foreground layer is not active, and if so, processes the pending command to start playing the new sequence.
	HandlePendingSequenceCmd();
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

void PlaybackManager::HandlePendingBackgroundCmd(AppState& state)
{
	if (backgroundActive && backgroundTrack && !backgroundTrack->IsActive())
	{
		backgroundActive = false;
	}

	if (backgroundTrack && !backgroundTrack->IsActive())
	{
		if (hasPendingBackgroundCmd)
		{
			hasPendingBackgroundCmd = false;
			int foundVideo = state.sourcesMap.count(pendingBackgroundCmd.filename);
			if (foundVideo)
			{
				PlayTrackOnLayer(pendingBackgroundCmd.filename, backgroundTrack, backgroundActive, LayerType::Background, &pendingBackgroundCmd);
			}
		}
	}
}

void PlaybackManager::HandleBackgroundEvents()
{
	if (!backgroundTrack || !backgroundTrack->IsActive()) 
		return;

	VideoSource* bgSource = backgroundTrack->GetSource();
	if (!bgSource) 
		return;

	double backgroundPTS = bgSource->internalPTS;
	AppState& state = appInterface->GetAppState();

	// -------------------------------------------------------------
	// PHASE 1: EVALUATE TRIGGER CONDITIONS FOR SCHEDULED EVENTS
	// -------------------------------------------------------------

	//Loops through all the events loaded from -events.txt file 
	for (auto& evt : bgSource->events)
	{
		//Ensures that the event hasn't been fired yet and that the background playhead has reached or passed the event start time
		if (!evt.triggered && backgroundPTS >= evt.startTime)
		{
			//Provies a small grace period of 1 second after the event start time preventing missed triggers due to timing discrepancies
			if (backgroundPTS <= (evt.startTime + 1.0))
			{
				//Marks the event as triggered to prevent it from firing again in subsequent iterations of the loop
				evt.triggered = true;

				//Check if the event is a sequence
				bool isSequence = (evt.filename.find(".txt") != std::string::npos);

				//SEQUENCE
				if (isSequence)
				{
					//Creates a local DeferredCommand to play the sequence
					DeferredCommand cmd;
					cmd.type = NetworkCommandType::PlaySequence;
					cmd.filename = evt.filename;
					cmd.fadeInDuration = evt.fadeInDuration;
					cmd.fadeOutDuration = evt.fadeOutDuration;
					cmd.looped = false;

					std::cout << "[Timeline Event] Firing automated sequence file: " << evt.filename
						<< " at background playhead pos: " << backgroundPTS << "s" << std::endl;

					EnqueueNetworkCommand(cmd);
				}
				//VIDEO
				else
				{
					//Find the index of the video source matching the event filename
					//int matchIdx = FindVideoSourceIndexByFilename(evt.filename, state.sources);
					int foundVideo = state.sourcesMap.count(evt.filename);
					if (foundVideo > 0)
					{
						//Creates a local DeferredCommand to play the foreground video
						DeferredCommand cmd;
						cmd.type = NetworkCommandType::PlayForeground;
						cmd.filename = evt.filename;

						if (evt.fadeInDuration < 0.0f)
						{
							cmd.fadeInDuration = state.sourcesMap[evt.filename]->fadeInDuration;
						}
						else
						{
							cmd.fadeInDuration = evt.fadeInDuration;
						}
						
						cmd.fadeOutDuration = evt.fadeOutDuration;
						cmd.looped = false;

						Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "HandleBackgroundEvents", "Firing foreground layer item: " + evt.filename + " at playhead pos: " + std::to_string(backgroundPTS) + "s");
						PlayTrackOnLayer(evt.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
					}
				}
			}
		}
	}

	// -------------------------------------------------------------
	// PHASE 2: STOPS VIDEOS AND SEQUENCES THAT HAVE REACHED THEIR DURATION THRESHOLDS
	// -------------------------------------------------------------

	//STOPS VIDEO
	if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
	{
		VideoSource* fgSource = foregroundTrack->GetSource();
		if (fgSource)
		{
			for (const auto& evt : bgSource->events)
			{
				if (evt.filename == fgSource->filename && evt.triggered)
				{
					if (evt.duration <= 0.0f)
						continue;

					if (fgSource->internalPTS >= evt.duration && !fgSource->isFadingOut)
					{
						std::cout << "[Timeline Event] Duration reached for " << evt.filename
							<< ". Injecting automatic fade out." << std::endl;
						foregroundTrack->StartForcedFadeOut(evt.fadeOutDuration);
					}
				}
			}
		}
	}

	//STOPS SEQUENCES

	//Check if a sequence is alive and active
	if (activeSequence && activeSequence->isActive)
	{
		//Loops through all the events in from the -events.txt file
		for (const auto& evt : bgSource->events)
		{
			//Checks if the event is associated with the active sequence and has been triggered
			if (evt.filename == activeSequence->name && evt.triggered)
			{
				//If the event has a duration of <= 0, it means it has no duration so it can run indefinitely
				//This is necessary is sequence has an element set to loop
				if (evt.duration <= 0.0f)
					continue;

				//Calculates how many seconds are passed since the sequence has started playing
				double elapsedSequenceTime = backgroundPTS - evt.startTime;

				//If it's more >= than the event duration...
				if (elapsedSequenceTime >= evt.duration)
				{
					std::cout << "[Timeline Event] Duration threshold reached for active sequence: " << activeSequence->name
						<< ". Killing running foreground channels." << std::endl;

					//Halts the active sequence advancement, deactivates sequence flags and triggers a clean fade out (if set) on current 
					//sequence video that is playing
					ForceStopForegroundLayers(evt.fadeOutDuration);
				}
			}
		}
	}
}

/*
* Example:
* VIDEO A is playing in the foreground, a new command to play VIDEO B is received:
*	1.Video A starts a forced fade-out 
*	2.While Video A is fading out, Video B must wait in line
*	3.Once Video A finsihes (foregroundActive = false) it stops, then Video B instantly start plaing (usually with a fading-in)

* Checks if a previous foreground video has just finished (foregroundActive = false) and if there is a pending command to play a new foreground video. 
* If both conditions are met, it processes the pending command by finding the corresponding video source index and instantiating the foreground track with the new video.
*/
void PlaybackManager::HandlePendingForegroundCmd(AppState& state)
{
	if (!foregroundActive && hasPendingForegroundCmd)
	{
		hasPendingForegroundCmd = false;

		int foundVideo = state.sourcesMap.count(pendingForegroundCmd.filename);
		if (foundVideo)
		{
			// Instantly instantiate Video B and reset its properties ready to be played the next time we enter the Main Loop.
			// The actual playback of Video B will be triggered in the next iteration of the Run loop once the current foreground video has fully finished and foregroundActive becomes false.
			PlayTrackOnLayer(pendingForegroundCmd.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &pendingForegroundCmd);
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
			// The fade out of the current sequence video is called in ForceStopForegroundLayers() when a new foreground video command is received while a sequence is active.
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
		int foundVideo = state.sourcesMap.count(pendingCoverCmd.filename);
		if (foundVideo > 0)
		{
			PlayTrackOnLayer(pendingCoverCmd.filename, coverTrack, coverActive, LayerType::Cover, &pendingCoverCmd);
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
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "HandlePendingSequenceCmd", "Booting pending sequence: " + pendingSequenceCmd.filename);

		Sequence* targetSequence = nullptr;	
		for (auto seq : appInterface->GetAppState().sequences)
		{
			if (seq->name == pendingSequenceCmd.filename)
			{
				targetSequence = seq;
				break;
			}
		}

		if (targetSequence)
		{
			activeSequence = targetSequence;

			activeSequence->Stop(); // Reset the sequence state to ensure it starts from the beginning
			activeSequence->Play(pendingSequenceCmd.fadeInDuration);
		}
		else
		{
			Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "HandlePendingSequenceCmd", "Failed to boot pending sequence. File not matched: " + pendingSequenceCmd.filename);
		}
	}
}

void PlaybackManager::HandlePendingChoreographyCmd(AppState& state)
{
	//Ensures that the foreground layer is not active and that there is a pending choreography command to process.
	//If both conditions are met, it proceeds to handle the pending choreography command.
	if (!foregroundActive && hasPendingChoreographyCmd)
	{
		//Resets the flag indicating that there is a pending choreography command, preventing it from being processed again in subsequent iterations of the loop.
		hasPendingChoreographyCmd = false;
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "HandlePendingChoreographyCmd",
			"Foreground cleared. Booting pending choreography ID: " + std::to_string(pendingChoreographyCmd.choreoID));

		//Ensures that the choreography ID from the pending command exists in the choresMap, which maps choreography IDs to their corresponding filenames.
		if (state.choresMap.count(pendingChoreographyCmd.choreoID) > 0)
		{
			//Retrieves the filename associated with the choreography ID from the choresMap, which will be used to play the corresponding choreography video.
			std::string filename = state.choresMap[pendingChoreographyCmd.choreoID];

			//Calls the PlayChoreography method to initiate the playback of the choreography video with the specified parameters from the pending command.
			PlayChoreography(filename,
				pendingChoreographyCmd.fgFadeOutDuration,
				pendingChoreographyCmd.fadeInDuration,
				pendingChoreographyCmd.fadeOutDuration,
				pendingChoreographyCmd.choreoID,
				pendingChoreographyCmd.looped, 
				pendingChoreographyCmd.forceCoverOnExit);
		}
	}
}

void PlaybackManager::PlayTrackOnLayerIndex(int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
{
	AppState& state = appInterface->GetAppState();

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

void PlaybackManager::PlayTrackOnLayer(const std::string& videoName, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
{
	AppState& state = appInterface->GetAppState();

	if (state.sourcesMap.find(videoName) == state.sourcesMap.end())
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "PlayTrackOnLayer", "Video source " + videoName + " not found!");
		return;
	}

	Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayTrackOnLayer", "Playing video: " + videoName + " on layer: " + LayerTypeToStr(layerType));
	
	//Update the video source properties based on the command parameters if provided
	if (cmd)
	{
		state.sourcesMap[videoName]->fadeInDuration = cmd->fadeInDuration;
		state.sourcesMap[videoName]->fadeOutDuration = cmd->fadeOutDuration;
		state.sourcesMap[videoName]->looped = cmd->looped;
	}

	//Set initial alpha to 0 for fade-in effect
	state.sourcesMap[videoName]->alpha = 0.0f;

	VideoSource* source = state.sourcesMap[videoName];

	// Dynamically update the passed track unique_ptr pointer structure
	targetTrack = std::make_unique<VideoTrack>(state.sourcesMap[videoName]);
	targetTrack->SetBlending(true);
	targetTrack->Rewind();
	targetTrack->Play(GetTimeStd());

	// Force wrapper synchronization status explicitly
	targetTrack->SetActive(true);

	// Bootstrap first frame mapping context
	ID3D11DeviceContext* ctx = renderer->GetContext();
	state.sourcesMap[videoName]->GetNextFrame(ctx);

	if (state.sourcesMap[videoName]->isFadingIn)
	{
		state.sourcesMap[videoName]->ComputeFadeIn();
	}
	else if (state.sourcesMap[videoName]->fadeInDuration > 0.0f)
	{
		state.sourcesMap[videoName]->alpha = 0.0f;
	}

	// Toggle the specific layer visibility state flag on
	targetActiveFlag = true;
}

void PlaybackManager::PlaySequenceItem(DeferredCommand& cmd)
{
	AppState& state = appInterface->GetAppState();
	//int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);
	int foundVideo = state.sourcesMap.count(GetFilenameFromPath(cmd.filename));
	if (foundVideo)
	{
		//PlayTrackOnLayerIndex(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
		PlayTrackOnLayer(GetFilenameFromPath(cmd.filename), foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
	}
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

/*
* Triggers an immediate forced fade-out of the foreground and cover videos if they are active, and resets all pending command flags.
*/
void PlaybackManager::ForceStopForegroundLayers(float duration)
{
	hasPendingForegroundCmd = false;
	hasPendingSequenceCmd = false;
	hasPendingCoverCmd = false;

	if (activeSequence && activeSequence->isActive)
	{
		activeSequence->Stop();
	}

	if (foregroundActive && foregroundTrack)
	{
		foregroundTrack->StartForcedFadeOut(duration);
	}

	if (coverActive && coverTrack)
	{
		coverTrack->StartForcedFadeOut(duration);
	}
}

void PlaybackManager::ForceStopBackgroundLayer(float duration)
{
	// Clear any pending commands queued to start a new background video
	hasPendingBackgroundCmd = false;

	// If the background track is actively rendering, command its underlying source to start a forced fade-out
	if (backgroundActive && backgroundTrack)
	{
		backgroundTrack->StartForcedFadeOut(duration);
	}
}

void PlaybackManager::EnqueueNetworkCommand(const DeferredCommand& cmd)
{
	std::lock_guard<std::mutex> lock(queueMutex);
	commandQueue.push(cmd);
}

void PlaybackManager::ProcessDeferredCommands()
{
	std::queue<DeferredCommand> localQueue;
	AppState& state = appInterface->GetAppState();

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (commandQueue.empty()) return;
		std::swap(commandQueue, localQueue);
	}

	while (!localQueue.empty())
	{
		DeferredCommand cmd = localQueue.front();
		localQueue.pop();

		switch (cmd.type)
		{
			case NetworkCommandType::Stop:
			{
				std::cout << ">>> [PlaybackManager] Processing deferred 'stop' action." << std::endl;
				//ForceStopBackgroundLayer(1.0f);
				ForceStopForegroundLayers(1.0f);
				break;
			}

			case NetworkCommandType::PlayBackground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_background': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);

				if (foundVideo > 0)
				{
					if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
					{
						std::cout << "[PlaybackManager] Background event video active on foreground layer. Forcing fade out." << std::endl;
						foregroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}

					if (activeSequence && activeSequence->isActive)
					{
						activeSequence->Stop();
					}

					if (backgroundTrack && backgroundTrack->IsActive())
					{
						pendingBackgroundCmd = cmd;
						hasPendingBackgroundCmd = true;

						backgroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						hasPendingBackgroundCmd = false;
						PlayTrackOnLayer(cmd.filename, backgroundTrack, backgroundActive, LayerType::Background, &cmd);
					}
				}

				break;
			}

			case NetworkCommandType::PlayForeground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_foreground': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);

				if (foundVideo > 0)
				{
					if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
					{
						pendingForegroundCmd = cmd;
						hasPendingForegroundCmd = true;

						if (activeSequence && activeSequence->isActive)
						{
							activeSequence->Stop();
						}
						foregroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						if (activeSequence && activeSequence->isActive)
						{
							activeSequence->Stop();
						}
						PlayTrackOnLayer(cmd.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
					}
				}
				break;
			}
			case NetworkCommandType::PlaySequence:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_sequence': " << cmd.filename << std::endl;

				Sequence* targetSequence = nullptr;	
				//Find the sequence matching the command filename
				for (auto seq : state.sequences)
				{
					if (seq->name == GetFilenameFromPath(cmd.filename))
					{
						targetSequence = seq;
						break;
					}
				}

				if (!targetSequence)
				{
					std::cerr << "[Main Thread] Error: Requested sequence file not found: " << cmd.filename << std::endl;
					break;
				}

				if (foregroundActive)
				{
					std::cout << "[PlaybackManager] Foreground busy. Buffering sequence, forcing fade out." << std::endl;

					pendingSequenceCmd = cmd;
					hasPendingSequenceCmd = true;

					if (activeSequence && activeSequence->isActive)
					{
						activeSequence->Stop();
					}

					// Only trigger the fade-out if the track is still actively rendering.
					// If it has already ended, ResetForegroundLayer() will clear foregroundActive
					// on the next UpdateLayers() call, which then triggers HandlePendingSequenceCmd().
					if (foregroundTrack && foregroundTrack->IsActive())
					{
						foregroundTrack->StartForcedFadeOut(1.0f);
					}
				}
				else
				{
					hasPendingSequenceCmd = false;
					activeSequence = targetSequence;

					if (activeSequence && !activeSequence->items.empty())
					{
						activeSequence->Stop();
						activeSequence->Play(cmd.fadeInDuration);
					}
				}

				break;
			}
			case NetworkCommandType::PlayCover:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_cover': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);
				if (foundVideo > 0)
				{
					if (coverActive && coverTrack && coverTrack->IsActive())
					{
						pendingCoverCmd = cmd;
						hasPendingCoverCmd = true;
						coverTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						//PlayTrackOnLayerIndex(matchIdx, coverTrack, coverActive, LayerType::Cover, &cmd);
						PlayTrackOnLayer(cmd.filename, coverTrack, coverActive, LayerType::Cover, &cmd);	
					}
				}
				break;
			}

			case NetworkCommandType::HideCover:
			{
				std::cout << ">>> [PlaybackManager] Processing deferred 'hide_cover' action." << std::endl;

				// Clear any pending commands queued to start a new cover video
				hasPendingCoverCmd = false;

				// If the cover track is actively rendering, command its underlying source to start a forced fade-out
				if (coverActive && coverTrack)
				{
					coverTrack->StartForcedFadeOut(1.0f);
				}
				break;
			}

			case NetworkCommandType::PlayChoreography:
			{
				Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "ProcessDeferredCommands", "Processing deferred 'play_choreography' with ID: " + std::to_string(cmd.choreoID));	
				int foundChoreo = state.choresMap.count(cmd.choreoID);

				if (foundChoreo > 0)
				{
					//If foregroun layer is active (sequences video or simple foreground video), we need to wait until it finishes to fade out 
					if (foregroundActive)
					{
						//Tells the system that there is a pending choreography command to be processed once the foreground layer is cleared
						hasPendingChoreographyCmd = true;
						pendingChoreographyCmd = cmd;

						//If there is an active sequence, stop it to prevent it from advancing to the next item in the sequence
						if (activeSequence && activeSequence->isActive)
						{
							activeSequence->Stop();
						}

						//If the foreground layer is active, start a forced fade-out transition using the specified duration from the command
						if (foregroundTrack && foregroundTrack->IsActive())
						{
							foregroundTrack->StartForcedFadeOut(cmd.fgFadeOutDuration);
						}
					}
					//If the foreground layer is not active, we can immediately process the choreography command
					else
					{
						//Tells the system that there is no longer a pending choreography command to be processed
						hasPendingChoreographyCmd = false;
						//Gets the filename associated with the choreography ID from the choresMap, which will be used to play the corresponding choreography video.
						std::string filename = state.choresMap[cmd.choreoID];
						//Calls the PlayChoreography method to initiate the playback of the choreography video with the specified parameters from the command.
						PlayChoreography(filename, cmd.fgFadeOutDuration, cmd.fadeInDuration, cmd.fadeOutDuration, cmd.choreoID, cmd.forceCoverOnExit);
					}
				}
				

				break;
			}
		}
	}
}

void PlaybackManager::PlayChoreography(const std::string& filename, float fgFadeOut, float fadeIn, float fadeOut, int idVal, bool loopVid, bool forceCoverOnExit)
{
	AppState& state = appInterface->GetAppState();
	Config& config = appInterface->GetConfig();

	Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Playing choreography file: " + filename + " with ID: " + std::to_string(idVal) + 
		" fgFadeOut: " + std::to_string(fgFadeOut) + " fadeIn: " + std::to_string(fadeIn) + " fadeOut: " + std::to_string(fadeOut) + " loopVid: " + (loopVid? + "true" : + "false") + 
		" forceCoverOnExit: " + (forceCoverOnExit ? +"true" : +"false"));

	// 1. Stores this ID in lastChoreoID to track which choreography sequence is currently running
	if (idVal != -1)
	{
		lastChoreoID = idVal;
	}

	// 2. Evaluate evaluation conditions: check if the previous sequence or foreground requested a cover force
	bool forceCover = forceCoverOnExitActive;
	forceCoverOnExitActive = forceCoverOnExit;

	// 3. Verifies if filename associated withg the choreography exists in the map
	if (state.sourcesMap.find(filename) == state.sourcesMap.end())
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "PlayChoreography", "Choreography video source " + filename + " not found!");
		return;
	}

	// 4. Extracts a reference to the metadata of the target video. This includes position data, event data, and other relevant information.
	VideoSource* videoSource = state.sourcesMap[GetFilenameFromPath(filename)];

	// 5. If the video has no position data, this video is a standard background video.
	if (videoSource->positions.empty())
	{
		//Plays a standard background
		PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);
		return;
	}

	// 6. 
	//Extracts the very first position coordinate from the CSV positions file associated with the choreography video
	float first_pos = videoSource->positions.front();
	//Extracts the very last position coordinate from CSV positions file associated with the choreography video
	float last_pos = videoSource->positions.back();
	//Retrieves the current physical position
	float current_pos = state.lastSentPosition;

	// 7. DISTANCE CHECK
	// Calculates the absolute delta between the current position and the video's expected target starting position.
	float distance = std::abs(current_pos - first_pos);

	// 8a. PLAY WITHOUT COVER
	//If the distance from the current position to the first position is less than 1 millimeter (std::abs(current_pos - first_pos), plays the choreography video immediately
	if ((!forceCover && distance < 1.0f) || config.disable_cover)
	{
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Starting choreography from the beginning. Distance to first position: " + std::to_string(distance) + "s");
		
		//If the loop property loopVid is true, it routes the video to play directly on the background track natively.
		if (loopVid)
		{
			this->PlayTrackOnLayer(filename, this->backgroundTrack, this->backgroundActive, LayerType::Background);
		}
		else if (coverActive && coverTrack)
		{
			//This sets up the video completely opaque(alpha = 1.0f), ensuring it is ready for a seamless reveal when the cover drops.
			/*this->ShowBgLastFrame(filename, idVal);*/
			this->ShowBgLastFrame(filename, idVal);
		}
		//If the screen is entirely unmasked (no cover track playing), starting a video at alpha = 1.0f would cause a harsh, single-frame visual pop before standard updates can calculate blending. 
		//To solve this, it packages a temporary DeferredCommand instructing the layer to begin smoothly with an alpha fade-in duration.
		else
		{
			DeferredCommand bgCmd;
			bgCmd.type = NetworkCommandType::PlayBackground;
			bgCmd.filename = filename;
			bgCmd.fadeInDuration = fadeIn;
			bgCmd.fadeOutDuration = fadeOut;
			bgCmd.looped = false;
			this->PlayTrackOnLayer(filename, this->backgroundTrack, this->backgroundActive, LayerType::Background, &bgCmd);
		}

		//COVER HIDE
		//The current_pos is near enough to the first position in the choreography video, so we can start playing it immediately. 
		//However, if the cover layer is active, we need to handle it properly.
		if (coverActive && coverTrack)
		{
			Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Cover layer is active. Injecting immediate forced fade out.");

			// Turn off looping on the cover track so it finishes playing out
			if (coverTrack->GetSource())
			{
				coverTrack->GetSource()->looped = false;
			}

			// Force a fade-out transition using the requested timing parameter
			//float coverFadeOut = (fadeOut == 0.0f) ? config.cover_fade_out_time : fadeOut;
			float coverFadeOut = (fadeOut >= 0.0f) ? fadeOut : config.cover_fade_out_time;
			coverTrack->StartForcedFadeOut(coverFadeOut);
		}
	}
	// 8b. PLAY WITH COVER
	else
	{
		std::string targetFile = filename;

		// Must undergo smooth motion transition phase through cover asset layer
		TransitionTo(first_pos, fadeIn, fadeOut, idVal, fgFadeOut);

	}
}

void PlaybackManager::TransitionTo(float targetPos, float coverfadeIn, float coverfadeOut, int idVal, float fgFadeOut)
{
	AppState& state = appInterface->GetAppState();
	Config& config = appInterface->GetConfig();

	//Verifies if cover video source exists in the map
	std::string coverVideo = GetFilenameFromPath(config.cover_filename);
	if (state.sourcesMap.find(coverVideo) == state.sourcesMap.end())
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "TransitionTo", "Cover video source " + coverVideo + " not found! Aborting transition.");
		return;
	}

	//Updates the NetworkManager to make it ready for the Brake-Move-To transition
	if (state.networkMgr)
	{
		state.networkMgr->SetupTransition(targetPos, idVal);
	}

	state.transitionId = idVal;

	//Determines the cover fade-in and fade-out durations to be used for this transition. 
	//If the incoming parameters are set to a non-negative value, it uses those values; otherwise, it falls back to the default durations specified in the configuration.
	float coverFadeIn = (coverfadeIn >= 0.0f) ? coverfadeIn : config.cover_fade_in_time;
	float coverFadeOut = (coverfadeOut >= 0.0f) ? coverfadeOut : config.cover_fade_out_time;

	DeferredCommand coverCmd;
	coverCmd.type = NetworkCommandType::PlayCover;
	coverCmd.filename = coverVideo;
	coverCmd.fadeInDuration = coverFadeIn;
	coverCmd.fadeOutDuration = coverFadeOut;
	coverCmd.looped = true;

	PlayTrackOnLayer(coverVideo, coverTrack, coverActive, LayerType::Cover, &coverCmd);
}

/*
* This method is primarily called during choreography updates when a cover video transition completes. 
* Its goal is to bring the background video to full visibility instantly underneath a fading cover layer so that the transition looks smooth.
*/
void PlaybackManager::ShowBgLastFrame(const std::string& filename, int idVal)
{
	//This effectively starts the setup and initial frame decoding loop for the background video.
	PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);

	//A safety condition checking if the backgroundTrack object was successfully created and initialized by the preceding PlayTrackOnLayer function call
	if (backgroundTrack)
	{
		//Set the background track to active and playing state, ensuring it is ready for rendering in the next frame update.
		backgroundTrack->SetActive(true);
		backgroundTrack->state = VideoTrackState::Playing;

		VideoSource* bgSource = backgroundTrack->GetSource();
		if (bgSource)
		{
			//By initializing it fully opaque immediately, it prevents the video from trying to perform its own standard fade-in behavior
			bgSource->alpha = 1.0f;   
			//Ensure it reaches its actual ending naturally without restarting
			bgSource->looped = false;  
		}
	}
}
