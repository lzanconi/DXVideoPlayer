#pragma once
#include <string>

enum class MESSAGE_TYPE
{
    INFO,
    ERRORS
};

class Logger
{
public:
	static void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "");
};

