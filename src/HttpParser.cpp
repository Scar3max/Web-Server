#include "HttpParser.h"
#include <sstream>
#include <algorithm>

using namespace std;

HttpRequest parseRequest(const string& rawRequest) {
    HttpRequest request;
    request.rawRequest = rawRequest;
    istringstream requestStream(rawRequest);

    string firstLine;
    if (getline(requestStream, firstLine)) {
        // Strip trailing \r if present
        if (!firstLine.empty() && firstLine.back() == '\r') {
            firstLine.pop_back();
        }
        istringstream lineStream(firstLine);
        string rawPath;
        lineStream >> request.method >> rawPath >> request.version;

        size_t qPos = rawPath.find('?');
        if (qPos != string::npos) {
            request.path = rawPath.substr(0, qPos);
            request.query = rawPath.substr(qPos + 1);
        } else {
            request.path = rawPath;
        }
    }

    string line;
    while (getline(requestStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break; // Header section end
        }

        size_t colon = line.find(':');
        if (colon != string::npos) {
            string headerName = line.substr(0, colon);
            // Lowercase header name
            transform(headerName.begin(), headerName.end(), headerName.begin(),
                      [](unsigned char c) { return tolower(c); });

            size_t valStart = colon + 1;
            while (valStart < line.size() && (line[valStart] == ' ' || line[valStart] == '\t')) {
                valStart++;
            }
            string headerVal = line.substr(valStart);

            if (headerName == "accept") {
                request.accept = headerVal;
            } else if (headerName == "user-agent") {
                request.userAgent = headerVal;
            }
        }
    }

    return request;
}
