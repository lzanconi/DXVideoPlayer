#pragma once
#include <vector>
#include <string>
#include "customtypes.h"

class VideoSource;

class IApp 
{
public:
	virtual ~IApp() = default;

    virtual VideoSource* GetBackgroundVideo() = 0;
    virtual std::vector<float> GetPositions() = 0;
    virtual double GetLastPTS() = 0;
    virtual int64_t GetBGCaptureTimeNS() = 0;
	virtual AppState& GetAppState() = 0;
    virtual void SetClientSocket(int socket) = 0;
    virtual void HandleNetworkCommand(const std::string& jsonStr) = 0;
	virtual void TriggerSequenceItem(const DeferredCommand& cmd) = 0;
};
