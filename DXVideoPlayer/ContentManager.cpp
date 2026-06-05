#include "ContentManager.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include "App.h"
#include "utils.h"
#include "customtypes.h"
#include "Sequence.h"

namespace fs = std::filesystem;

ContentManager::ContentManager(IApp* appInterface) : appInterface(appInterface) {}

void ContentManager::LoadContentsFromFolder(const std::string& folderPath)
{
    try
    {
        if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
        {
            std::cerr << "Directory does not exist: " << folderPath << std::endl;
            return;
        }

        LoadVideoContents(folderPath);

    }
    catch (const std::exception& e)
    {
        std::cerr << "Error loading contents from folder: " << e.what() << std::endl;
    }
}

void ContentManager::LoadVideoContents(const std::string& folderPath)
{
	videoContents.clear();

    try
    {
        //Validates that the provided path exists and is a directory
        if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
        {
            std::cerr << "Directory does not exist: " << folderPath << std::endl;
            return;
        }

        //Iterates through every file inside the specified directory
        for (const auto& entry : fs::directory_iterator(folderPath))
        {
            //Checks if the entry is a file and has an .mp4 extension
            if (entry.is_regular_file() && entry.path().extension() == ".mp4")
            {
                VideoContent content;
                //Stores the full system path of the video file
                content.filename = entry.path().string();

                //Assigns a default fade-in duration
                content.fadeInDuration = 1.0f;
                //Assigns a default fade-out duration
                content.fadeOutDuration = 1.0f;
                //Sets the video to play once
                content.looped = false;

                //Creates a potential path for a matching CSV by swapping the extension
                fs::path csvPath = entry.path();
                csvPath.replace_extension(".csv");

                //Checks if a .csv file with the same name as the video exists
                if (fs::exists(csvPath))
                {
                    std::cout << "ContentManager: Found matching CSV for " << entry.path().filename() << std::endl;
                    //Calls the helper to parse CSV coordinates into the content object
                    LoadCSVPositions(content, csvPath.string());
                }

				//Creates a potential path for a matching events text file by appending "-events" to the base name
                fs::path eventsPath = entry.path();
                std::string baseName = eventsPath.stem().string();
                eventsPath.replace_filename(baseName + "-events.txt");

                if (fs::exists(eventsPath))
                {
                    std::cout << "ContentManager: Found matching events file for " << entry.path().filename() << std::endl;
                    LoadBackgroundEvents(eventsPath.string(), content);
                }

                //Adds the fully prepared video metadata to the list
                videoContents.push_back(content);
            }
        }

        //Searches for the first video containing "bg" in its name to serve as the background
        auto it = std::find_if(videoContents.begin(), videoContents.end(), [](const VideoContent& v)
            {
                return v.filename.find("bg") != std::string::npos;
            });

        // If a background video is found, moves it to the very front of the list
        if (it != videoContents.end())
        {
            std::rotate(videoContents.begin(), it, it + 1);
        }

        // Ensures there are at least two videos before applying specific slot behaviors
        if (videoContents.size() >= 2)
        {
            //Sets the background (index 0) to loop indefinitely.
            videoContents.at(0).looped = true;
            //Disables fade-in for the background for immediate playback
            videoContents.at(0).fadeInDuration = 0.0f;
            //Disables fade-out for the background
            videoContents.at(0).fadeOutDuration = 2.0f;

            //JUST FOR TESTING
            //Sets the first foreground candidate (index 1) to loop indefinitely
            //videoContents.at(1).looped = true;
            ////Disables fade-in for this foreground slot
            //videoContents.at(1).fadeInDuration = 2.0f;
            ////Disables fade-out for this foreground slot
            //videoContents.at(1).fadeOutDuration = 2.0f;
        }

        std::cout << "ContentManager: Loaded " << videoContents.size() << " videos." << std::endl;
    }
    catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }
}

const std::vector<VideoContent>& ContentManager::GetVideoContents() const
{
    return videoContents;
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

void ContentManager::LoadBackgroundEvents(const std::string& filePath, VideoContent& content)
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
    std::string sequencePath = "";

    // First pass: scan the folder to see if a sequence text file exists
    for (const auto& entry : fs::directory_iterator(folderPath))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
        {
            std::string filename = entry.path().filename().string();
            if (filename.find("sequence") != std::string::npos)
            {
                sequencePath = entry.path().string();
                std::cout << ">> Found sequence file: " << sequencePath << std::endl;
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
    std::cout << ">> Parsing sequence file: " << filePath << std::endl;

    // Attempts to open the sequence definition text file at the provided file path.
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        std::cerr << "Failed to open sequence file: " << filePath << std::endl;
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

                if (key == "fadeIn") {
                    seqItem.fadeInDuration = std::stof(val);
                }
                else if (key == "fadeOut") {
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
