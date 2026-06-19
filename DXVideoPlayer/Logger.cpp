#include "Logger.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include "utils.h"

void Logger::SetAppInterface(IApp* appInterface)
{
	this->appInterface = appInterface;
}

void Logger::LogMessage(MESSAGE_TYPE type, const std::string& className, const std::string& methodName, const std::string& message)
{
	const std::string RESET = "\033[0m";
	const std::string RED = "\033[31m";
	const std::string GREEN = "\033[32m";
	const std::string YELLOW = "\033[33m";
	const std::string BLUE = "\033[34m";
	const std::string MAGENTA = "\033[35m";
	const std::string CYAN = "\033[36m";

	std::string messageTypeStr = (type == MESSAGE_TYPE::ERRORS) ? "ERROR" : "MSG";
	
	std::string timeStamp = GetTimestampStr();
	
	std::string logMsg = "---------------------------------------------------------------------------------------------------------\n"
		"[" + messageTypeStr + " -> " + timeStamp + " - " + className + "::" + methodName + "()]:\n " + message + "\n";
	
	if (type == MESSAGE_TYPE::ERRORS)
	{
		std::cerr << RED << logMsg << RESET << std::endl;
	}
	else
	{
		std::cout << GREEN << logMsg << RESET << std::endl;
	}
}
