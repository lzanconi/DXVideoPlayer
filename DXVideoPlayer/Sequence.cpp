#include "Sequence.h"
#include <iostream>
#include "PlaybackManager.h"
#include "Logger.h"

Sequence::Sequence(std::string& name, const std::vector<SequenceItem>& items, PlaybackManager* playbackMgr)
	: name(name), items(items), playbackMgr(playbackMgr)
{ }

void Sequence::Play(float overrideFadeIn)
{
	if (items.empty())
	{
		//std::cerr << "[Sequence] No items to play in sequence: " << name << std::endl;
		//Logger::LogMessage(MESSAGE_TYPE::ERRORS, "Sequence", "Play", "No items to play in sequence: " + name);
		return;
	}

	std::cout << "[Sequence] Starting sequence: " << name << " with " << items.size() << " items." << std::endl;
	isActive = true;
	currentIndex = 0;
	
	DeferredCommand cmd;
	cmd.type = NetworkCommandType::PlayForeground;
	cmd.filename = items[currentIndex].filename;

	if (overrideFadeIn >= 0.0f)
	{
		//Logger::LogMessage(MESSAGE_TYPE::INFO, "Sequence", "Play", "Applying automated timeline event override fade-in: " + std::to_string(overrideFadeIn) + "s");
		cmd.fadeInDuration = overrideFadeIn;
	}
	else
	{
		cmd.fadeInDuration = items[currentIndex].fadeInDuration;
	}
	cmd.fadeOutDuration = items[currentIndex].fadeOutDuration;
	cmd.looped = items[currentIndex].looped;

	playbackMgr->PlaySequenceItem(cmd);
}

void Sequence::Stop()
{
	//Logger::LogMessage(MESSAGE_TYPE::INFO, "Sequence", "Stop", "Manually stopping sequence: " + name);
	isActive = false;
	currentIndex = -1;
}	

void Sequence::AdvanceSequence()
{
	if (!isActive)
		return;

	// 1. Check if the CURRENT item is configured to loop internally
	if (currentIndex >= 0 && currentIndex < static_cast<int>(items.size()))
	{
		if (items[currentIndex].looped)
		{
			//Logger::LogMessage(MESSAGE_TYPE::INFO, "Sequence", "AdvanceSequence", "Repeating single item due to item-level loop configuration: " + items[currentIndex].filename + " (Index: " + std::to_string(currentIndex) + ")");

			DeferredCommand cmd;
			cmd.type = NetworkCommandType::PlayForeground;
			cmd.filename = items[currentIndex].filename;

			// Strictly use the values written in the .txt file as requested
			cmd.fadeInDuration = items[currentIndex].fadeInDuration;
			cmd.fadeOutDuration = items[currentIndex].fadeOutDuration;
			cmd.looped = true;

			playbackMgr->PlaySequenceItem(cmd);
			return; // Stay on the same index and exit immediately
		}
	}

	// 2. If the current item doesn't loop, advance to the next item normally
	currentIndex++;

	// 3. Handle the natural end of the sequence (no sequence-level looping)
	if (currentIndex >= static_cast<int>(items.size()))
	{
		//Logger::LogMessage(MESSAGE_TYPE::INFO, "Sequence", "AdvanceSequence", "Sequence completed naturally: " + name);
		isActive = false;
		currentIndex = -1;
		return;
	}

	// 4. Play the next item using exactly the parameters from the file
	//Logger::LogMessage(MESSAGE_TYPE::INFO, "Sequence", "AdvanceSequence", "Advancing to next item in sequence: " + name + " (Index: " + std::to_string(currentIndex) + ")");

	DeferredCommand cmd;
	cmd.type = NetworkCommandType::PlayForeground;
	cmd.filename = items[currentIndex].filename;
	cmd.fadeInDuration = items[currentIndex].fadeInDuration;
	cmd.fadeOutDuration = items[currentIndex].fadeOutDuration;
	cmd.looped = items[currentIndex].looped;

	playbackMgr->PlaySequenceItem(cmd);
}