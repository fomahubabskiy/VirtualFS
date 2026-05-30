#pragma once
#include <string>
#include <atomic>   
bool ExtractAll(const std::string& archivePath,const std::string& outDir,std::atomic<float>& progress);