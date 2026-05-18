/**
 * @file llgiphyclient.cpp
 * @brief GIPHY API client for Tasia Viewer chat GIF features.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, Tasia Viewer Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llgiphyclient.h"

#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llmath.h"
#include "llsd.h"
#include "llstring.h"
#include "lltasia_giphy_key.h"
#include "lluri.h"
#include "llviewercontrol.h"

#include <boost/bind.hpp>
#include <sstream>

namespace
{
const std::string GIPHY_API_BASE_URL = "https://api.giphy.com/v1/gifs/";
const std::string GIPHY_NOT_CONFIGURED = "GIPHY is not configured.";
const S32 GIPHY_MIN_LIMIT = 1;
const S32 GIPHY_MAX_LIMIT = 50;
const S32 GIPHY_MAX_OFFSET = 4999;

std::string getString(const LLSD& data, const std::string& key)
{
    return data.has(key) ? data[key].asString() : std::string();
}

S32 getS32(const LLSD& data, const std::string& key)
{
    return data.has(key) ? data[key].asInteger() : 0;
}

std::string getRating()
{
    std::string rating = gSavedSettings.getString("TasiaGiphyRating");
    LLStringUtil::trim(rating);
    LLStringUtil::toLower(rating);

    if (rating == "g" || rating == "pg" || rating == "pg-13" || rating == "r")
    {
        return rating;
    }

    return "pg-13";
}

void fillImage(const LLSD& images, const std::string& image_key, std::string& url, S32* width = NULL, S32* height = NULL)
{
    if (!images.has(image_key))
    {
        return;
    }

    const LLSD& image = images[image_key];
    url = getString(image, "url");
    if (width)
    {
        *width = getS32(image, "width");
    }
    if (height)
    {
        *height = getS32(image, "height");
    }
}

LLGiphyClient::results_t parseResults(const LLSD& response)
{
    LLGiphyClient::results_t results;
    if (!response.has("data") || !response["data"].isArray())
    {
        return results;
    }

    const LLSD& data = response["data"];
    for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
    {
        const LLSD& item = *it;
        LLGiphyClient::Result gif;
        gif.id = getString(item, "id");
        gif.title = getString(item, "title");
        gif.page_url = getString(item, "url");

        if (gif.id.empty() || !item.has("images"))
        {
            continue;
        }

        const LLSD& images = item["images"];
        fillImage(images, "preview_gif", gif.preview_gif_url, &gif.preview_width, &gif.preview_height);
        fillImage(images, "fixed_width", gif.fixed_width_gif_url);
        fillImage(images, "downsized", gif.downsized_gif_url);
        fillImage(images, "original", gif.original_gif_url);

        if (gif.preview_gif_url.empty())
        {
            gif.preview_gif_url = !gif.fixed_width_gif_url.empty() ? gif.fixed_width_gif_url : gif.downsized_gif_url;
        }
        if (gif.preview_gif_url.empty())
        {
            gif.preview_gif_url = gif.original_gif_url;
        }
        if (gif.page_url.empty())
        {
            gif.page_url = "https://giphy.com/gifs/" + gif.id;
        }

        results.push_back(gif);
    }

    return results;
}

void sendResponse(const LLGiphyClient::response_callback_t& callback,
                  bool success,
                  const std::string& message,
                  const LLGiphyClient::results_t& results = LLGiphyClient::results_t())
{
    if (callback)
    {
        callback(success, message, results);
    }
}

std::string buildRequestURL(const std::string& endpoint,
                            const std::string& api_key,
                            const std::string& query,
                            S32 limit,
                            S32 offset)
{
    std::ostringstream url;
    url << GIPHY_API_BASE_URL << endpoint
        << "?api_key=" << LLURI::escape(api_key)
        << "&limit=" << llclamp(limit, GIPHY_MIN_LIMIT, GIPHY_MAX_LIMIT)
        << "&offset=" << llclamp(offset, 0, GIPHY_MAX_OFFSET)
        << "&rating=" << LLURI::escape(getRating())
        << "&bundle=messaging_non_clips";

    if (!query.empty())
    {
        url << "&q=" << LLURI::escape(query)
            << "&lang=en";
    }

    return url.str();
}
}

// static
const std::string& LLGiphyClient::notConfiguredMessage()
{
    return GIPHY_NOT_CONFIGURED;
}

// static
bool LLGiphyClient::isConfigured()
{
    return gSavedSettings.getBOOL("TasiaGiphyEnabled") && LLTasiaGiphyKey::hasConfiguredAPIKey();
}

// static
void LLGiphyClient::search(const std::string& query, response_callback_t callback, S32 limit, S32 offset)
{
    if (!gSavedSettings.getBOOL("TasiaGiphyEnabled"))
    {
        sendResponse(callback, false, "GIPHY is disabled.");
        return;
    }

    std::string trimmed_query = query;
    LLStringUtil::trim(trimmed_query);
    if (trimmed_query.empty())
    {
        sendResponse(callback, false, "Enter a search term.");
        return;
    }

    const std::string api_key = LLTasiaGiphyKey::getConfiguredAPIKey();
    if (api_key.empty())
    {
        sendResponse(callback, false, notConfiguredMessage());
        return;
    }

    LLCoros::instance().launch("LLGiphyClient::searchCoro",
        boost::bind(&LLGiphyClient::requestCoro,
                    buildRequestURL("search", api_key, trimmed_query, limit, offset),
                    callback));
}

// static
void LLGiphyClient::trending(response_callback_t callback, S32 limit, S32 offset)
{
    if (!gSavedSettings.getBOOL("TasiaGiphyEnabled"))
    {
        sendResponse(callback, false, "GIPHY is disabled.");
        return;
    }

    const std::string api_key = LLTasiaGiphyKey::getConfiguredAPIKey();
    if (api_key.empty())
    {
        sendResponse(callback, false, notConfiguredMessage());
        return;
    }

    LLCoros::instance().launch("LLGiphyClient::trendingCoro",
        boost::bind(&LLGiphyClient::requestCoro,
                    buildRequestURL("trending", api_key, std::string(), limit, offset),
                    callback));
}

// static
void LLGiphyClient::requestCoro(std::string url, response_callback_t callback)
{
    LLCore::HttpRequest::ptr_t http_request(new LLCore::HttpRequest);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t http_adapter(
        new LLCoreHttpUtil::HttpCoroutineAdapter("GiphyFetch", LLCore::HttpRequest::DEFAULT_POLICY_ID));
    LLCore::HttpHeaders::ptr_t http_headers(new LLCore::HttpHeaders);
    LLCore::HttpOptions::ptr_t http_options(new LLCore::HttpOptions);

    http_options->setTimeout(15);
    http_options->setTransferTimeout(15);
    http_options->setRetries(1);
    http_headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD response = http_adapter->getJsonAndSuspend(http_request, url, http_options, http_headers);
    LLSD http_results = response[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
    if (!status)
    {
        LL_WARNS("GIPHY") << "GIPHY request failed: " << status.toString() << LL_ENDL;
        sendResponse(callback, false, "GIPHY request failed.");
        return;
    }

    if (response.has("meta") && response["meta"].has("status") && response["meta"]["status"].asInteger() >= 400)
    {
        const std::string message = response["meta"].has("msg") ? response["meta"]["msg"].asString() : "GIPHY request failed.";
        LL_WARNS("GIPHY") << "GIPHY API returned an error: " << message << LL_ENDL;
        sendResponse(callback, false, message);
        return;
    }

    LLGiphyClient::results_t results = parseResults(response);
    sendResponse(callback, true, results.empty() ? "No GIFs found." : std::string(), results);
}
