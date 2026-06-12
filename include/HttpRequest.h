#pragma once //include only once
#include<string>
using namespace std;
class HttpRequest
{
    public:
    string method;
    string path;
    string version;
};