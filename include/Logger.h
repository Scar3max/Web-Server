#pragma once
#include <string>

using namespace std;
class Logger{
    public:
        static void log(const string& message);
        static string getTime();
};