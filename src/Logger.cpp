#include "Logger.h"

#include<fstream>
#include<iostream>
#include <chrono>
#include <ctime>

using namespace std;

void Logger::log(const string& message){
    ofstream logFile("server.log",ios::app);
    if(logFile.is_open()){
        logFile<<getTime()<<" : "<<message<<endl;
    }
}

string Logger::getTime(){   
    auto now = chrono::system_clock::now();
    time_t current = chrono::system_clock::to_time_t(now);
    return ctime(&current);
}