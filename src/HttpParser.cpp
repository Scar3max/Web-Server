#include "HttpParser.h"
#include <sstream>

using namespace std;

HttpRequest parseRequest(const string& rawRequest){
    HttpRequest request;
    istringstream requestStream(rawRequest);
    string firstLine;
    getline(
        requestStream,
        firstLine
    );
    istringstream lineStream(firstLine);

    lineStream
        >>request.method
        >>request.path
        >>request.version;
    
    return request;
}