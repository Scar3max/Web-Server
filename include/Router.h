#pragma once

#include<string>
#include<unordered_map>
#include<functional>

class Router{
    private:
        std::unordered_map<std::string,std::function<std::string()>> routes;
    public:
        void addRoute(const std::string& path,std::function<std::string()>handler);
        std::string route(const std::string& path);
};