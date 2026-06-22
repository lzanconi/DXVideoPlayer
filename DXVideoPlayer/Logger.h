#pragma once
#include <string>
#include <mutex>
#include <fstream>
#include "customtypes.h"

class Logger
{
private:
	static std::mutex logMutex;

public:
	static void ClearLog();
	static void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "");

};

