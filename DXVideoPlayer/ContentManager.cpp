#include "ContentManager.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include "App.h"
#include "utils.h"
#include "customtypes.h"
#include "Sequence.h"
#include <json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

ContentManager::ContentManager(IApp* appInterface) : appInterface(appInterface) {}

void ContentManager::LoadContents()
{
	//LOAD VIDEO CONTENTS FROM CONFIG FILE CHOREOGRAPHIES 
	LoadVideoContentsFromConfig();

    //LOAD VIDEO CONTENTS FROM CONFIG FILE ASSETS PATH
	LoadVideoContentsFromFolder();
}

void ContentManager::LoadVideoContentsFromConfig()
{
    const Config& config = appInterface->GetConfig();
	AppState& state = appInterface->GetAppState();
    std::string choreosConfigFile = config.choreos_config_file;
    std::string errorMsg;

    if (!fs::exists(choreosConfigFile))
    {
		appInterface->LogMessage(MESSAGE_TYPE::ERRORS, "ContentManager", "LoadVideoContentsFromConfig", "Choreography config file " + choreosConfigFile + " not found!");
        
        return;
    }

    fs::path baseDir = fs::path(choreosConfigFile).parent_path();

    json j;
    try
    {
        std::ifstream fstream(choreosConfigFile);
        j = json::parse(fstream);
    }
    catch (const std::exception& e)
    {
		appInterface->LogMessage(MESSAGE_TYPE::ERRORS, "ContentManager", "LoadVideoContentsFromConfig", "Error parsing choreos config file " + choreosConfigFile + ": " + e.what());
        
        return;
    }

    if (!j.contains("choreographies") || !j["choreographies"].is_array())
    {
		appInterface->LogMessage(MESSAGE_TYPE::ERRORS, "ContentManager", "LoadVideoContentsFromConfig", "No 'choreographies' array in choreos config file: " + choreosConfigFile);
        return;
    }

	videoContentsMap.clear();

    for (const auto& choreo : j["choreographies"])
    {
        if (!choreo.contains("videoFile"))
            continue;

        std::string videoFile = choreo["videoFile"].get<std::string>();
        fs::path videoPath(videoFile);
        videoPath = (baseDir / videoPath).lexically_normal();

        VideoContent content;
        content.id = choreo.value("id", -1);
        content.name = GetFilenameFromPath(videoPath.string());
        content.filename = videoPath.string();

		state.choresMap[content.id] = content.name;

        //LOAD CSV POSITIONS
        if (choreo.contains("file"))
        {
            std::string csvFile = choreo["file"].get<std::string>();
            fs::path csvPath(csvFile);
            csvPath = (baseDir / csvPath).lexically_normal();
            LoadCSVPositions(content, csvPath.string());
        }

        //LOAD EVENTS
        if (choreo.contains("eventsFile"))
        {
            std::string eventsFile = choreo["eventsFile"].get<std::string>();
            fs::path eventsPath(eventsFile);
            eventsPath = (baseDir / eventsPath).lexically_normal();
            LoadBackgroundEvents(content, eventsPath.string());
        }

        videoContentsMap[content.name] = content;
        /*videoContents.push_back(content);*/
    }
}

void ContentManager::LoadVideoContentsFromFolder()
{
    const Config& config = appInterface->GetConfig();
    std::string assetsPath = config.assets_path;
    if (fs::exists(assetsPath))

    {
        for (const auto& entry : fs::recursive_directory_iterator(assetsPath))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".mp4")
                continue;

            std::string videoName = GetFilenameFromPath(entry.path().string());

            if (videoContentsMap.count(videoName))
                continue;

            VideoContent content;
			content.name = videoName;
            content.filename = entry.path().string();

			std::string coverFilename = GetFilenameFromPath(config.cover_filename);
            if (content.name == coverFilename)
            {
                content.fadeInDuration = config.cover_fade_in_time;
                content.fadeOutDuration = config.cover_fade_out_time;
			}

            //LOAD CSV POSITIONS
            fs::path csvPath = entry.path();
            csvPath.replace_extension(".csv");
			if (fs::exists(csvPath))
                LoadCSVPositions(content, csvPath.string());

            //LOAD EVENTS
            std::string stem = entry.path().stem().string();
            fs::path eventsPath = entry.path().parent_path() / (stem + "-events.txt");
            if (fs::exists(eventsPath))
				LoadBackgroundEvents(content, eventsPath.string());

            /*std::string baseName = eventsPath.stem().string();
            eventsPath.replace_filename(baseName + "-events.txt");*/

			videoContentsMap[content.name] = content;
        }
    }
}

void ContentManager::LoadCSVPositions(VideoContent& content, const std::string& csvPath)
{
    // Attempts to open the CSV file at the provided file path.
    std::ifstream file(csvPath);

    if (!file.is_open())
        return;

    content.positions.clear();

    //Reads the file line by line until the end of the document is reached
    std::string line;
    while (std::getline(file, line))
    {
        //Wraps the current line in a stringstream to facilitate comma-based parsing
        std::stringstream ss(line);
        std::string value;

        //Breaks each line into individual strings using the comma (',') as a delimiter
        while (std::getline(ss, value, ','))
        {
            try
            {
                if (!value.empty())
                {
                    //Converts the string to a floating-point number
                    float pos = std::stof(value);
                    content.positions.push_back(pos);
                }
            }
            catch (...) {
                // Skip invalid numeric entries
            }
        }
    }
}

void ContentManager::LoadBackgroundEvents(VideoContent& content, const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open()) 
        return;

    content.events.clear();
    std::string line;

    while (std::getline(file, line))
    {
        // Ignore empty lines or comment lines
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        std::string token;
        BackgroundEvent evt;

        evt.fadeInDuration = -1.0f;

        // Parse key-value pairs split by commas: file=3.mp4, start-time=17.00 ...
        while (std::getline(ss, token, ','))
        {
            // Simple trim lambda
            token.erase(0, token.find_first_not_of(" \t\r\n"));
            token.erase(token.find_last_not_of(" \t\r\n") + 1);

            size_t eqIdx = token.find('=');
            if (eqIdx == std::string::npos) 
                continue;

            std::string key = token.substr(0, eqIdx);
            std::string val = token.substr(eqIdx + 1);

            if (key == "file")             
                evt.filename = val;
            else if (key == "start-time")    
                evt.startTime = std::stof(val);
            else if (key == "fade-in-time")  
                evt.fadeInDuration = std::stof(val);
            else if (key == "fade-out-time") 
                evt.fadeOutDuration = std::stof(val);
            else if (key == "duration")      
                evt.duration = std::stof(val);
        }

        if (!evt.filename.empty())
        {
            content.events.push_back(evt);
        }
    }
}

void ContentManager::LoadSequences(const std::string& folderPath, PlaybackManager* playbackMgr)
{
	const Config& config = appInterface->GetConfig();
	std::string assetsPath = config.assets_path;
	assetsPath = (fs::path(assetsPath) / folderPath).lexically_normal().string();

    std::string sequencePath = "";

    // First pass: scan the folder to see if a sequence text file exists
    for (const auto& entry : fs::directory_iterator(assetsPath))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
        {
            std::string filename = entry.path().filename().string();
            if (filename.find("sequence") != std::string::npos)
            {
                sequencePath = entry.path().string();
				appInterface->LogMessage(MESSAGE_TYPE::INFO, "ContentManager", "LoadSequences", "Found sequence file: " + sequencePath);
                std::string seqFilename = GetFilenameFromPath(sequencePath);

                std::vector<SequenceItem> sequenceItems;
                ParseSequenceFile(sequencePath, sequenceItems);
				Sequence* sequence = new Sequence(seqFilename, sequenceItems, playbackMgr);
				appInterface->GetAppState().sequences.push_back(sequence);
                /*Sequence* sequence = new Sequence(seqFilename, sequenceItems, appInterface);
                appInterface->GetAppState().sequences.push_back(sequence);*/
            }
        }
    }
}

void ContentManager::ParseSequenceFile(const std::string& filePath, std::vector<SequenceItem>& outItems)
{
	appInterface->LogMessage(MESSAGE_TYPE::INFO, "ContentManager", "ParseSequenceFile", "Parsing sequence file: " + filePath);

    // Attempts to open the sequence definition text file at the provided file path.
    std::ifstream file(filePath);
    if (!file.is_open())
    {
		appInterface->LogMessage(MESSAGE_TYPE::ERRORS, "ContentManager", "ParseSequenceFile", "Failed to open sequence file: " + filePath);
        return;
    }

    //Reads the file line by line until the end of the document is reached
    std::string line;
    while (std::getline(file, line))
    {
        // Ignores empty lines and lines starting with '#' which are treated as comments
        if (line.empty() || line[0] == '#')
            continue;

        std::stringstream ss(line);
        std::string videoFilename;

        //If the line doesn't contain a comma or is improperly formatted, it will be skipped
        if (!std::getline(ss, videoFilename, ','))
            continue;

        //Trims whitespace from both ends of the extracted video filename to ensure accurate matching
        auto trim = [](std::string& str) {
            str.erase(0, str.find_first_not_of(" \t\r\n"));
            str.erase(str.find_last_not_of(" \t\r\n") + 1);
            };

        trim(videoFilename);

        //Checks if the extracted video filename is not empty after trimming
        if (videoFilename.empty())
            continue;

        SequenceItem seqItem;
        seqItem.filename = videoFilename;

        //Processes the remaining part of the line to extract key-value pairs for fade durations and loop settings
        std::string propertyToken;
        while (std::getline(ss, propertyToken, ','))
        {
            trim(propertyToken);
            if (propertyToken.empty())
                continue;

            size_t assignmentIdx = propertyToken.find('=');
            if (assignmentIdx != std::string::npos)
            {
                std::string key = propertyToken.substr(0, assignmentIdx);
                std::string val = propertyToken.substr(assignmentIdx + 1);

                trim(key);
                trim(val);

                if (key == "fade-in") {
                    seqItem.fadeInDuration = std::stof(val);
                }
                else if (key == "fade-out") {
                    seqItem.fadeOutDuration = std::stof(val);
                }
                else if (key == "loop") {
                    seqItem.looped = (val == "true" || val == "1");
                }
            }
        }

        outItems.push_back(seqItem);
    }
}

const std::map<std::string, VideoContent>& ContentManager::GetVideoContentsMap() const
{
    return videoContentsMap;
}
