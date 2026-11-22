#pragma once

#include <drogon/HttpResponse.h>
#include "job/Job.h"

namespace jobq::server
{

    drogon::HttpResponsePtr apiError(drogon::HttpStatusCode status, const std::string &code, const std::string &message);
    drogon::HttpResponsePtr apiData(Json::Value data, drogon::HttpStatusCode status = drogon::k200OK);
    Json::Value jobJson(const JobSnapshot &job);

}
