#pragma once

#include<string>
#include<unordered_map>
#include<functional>

using namespace std;
class Router{
    private:
        unordered_map<string,function<string()>> routes;
    public:
        void addRoute(const string& path,function<string()>handler);
        string route(const string& path);
};