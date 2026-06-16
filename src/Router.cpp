#include "Router.h"
using namespace std;
void Router::addRoute(const string& path,function<string()>handler){
    routes[path]=handler;
}

string Router::route(const string& path){
    auto it=routes.find(path);
    if(it!=routes.end())
        return it->second();
    return R"(
<html>
<body>
<h1>404 Not Found</h1>
</body>
</html>
)";
}