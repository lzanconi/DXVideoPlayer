#pragma once
#include <string>
#include "customtypes.h"

class Logger
{
private:

public:
	static void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "");
};

