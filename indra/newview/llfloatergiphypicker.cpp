/**
 * @file llfloatergiphypicker.cpp
 * @brief GIPHY picker floater for Tasia Viewer chat GIF features.
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

#include "llfloatergiphypicker.h"

#include "llbutton.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "llstring.h"
#include "lltextbox.h"

#include <boost/bind.hpp>

LLFloaterGiphyPicker::LLFloaterGiphyPicker(const LLSD& key)
    : LLFloater(key)
{
}

bool LLFloaterGiphyPicker::postBuild()
{
    mSearchEditor = getChild<LLLineEditor>("search_edit");
    mResultsList = getChild<LLScrollListCtrl>("results_list");
    mStatusText = getChild<LLTextBox>("status_text");
    mSearchButton = getChild<LLButton>("search_btn");
    mTrendingButton = getChild<LLButton>("trending_btn");
    mUseButton = getChild<LLButton>("use_btn");

    mSearchButton->setClickedCallback(boost::bind(&LLFloaterGiphyPicker::onSearchClicked, this));
    mTrendingButton->setClickedCallback(boost::bind(&LLFloaterGiphyPicker::onTrendingClicked, this));
    mUseButton->setClickedCallback(boost::bind(&LLFloaterGiphyPicker::onUseClicked, this));
    mResultsList->setCommitCallback(boost::bind(&LLFloaterGiphyPicker::onResultSelected, this));

    setStatus("Search GIPHY or load trending GIFs.");
    refreshControls();
    return true;
}

void LLFloaterGiphyPicker::onOpen(const LLSD& key)
{
    if (mResultsList && mResultsList->isEmpty())
    {
        requestTrending();
    }
}

void LLFloaterGiphyPicker::onClose(bool app_quitting)
{
    mSelectionCallback = selection_callback_t();
}

// static
LLFloaterGiphyPicker* LLFloaterGiphyPicker::show(selection_callback_t callback)
{
    LLFloaterGiphyPicker* floater = LLFloaterReg::showTypedInstance<LLFloaterGiphyPicker>("giphy_picker");
    if (floater)
    {
        floater->setSelectionCallback(callback);
    }
    return floater;
}

void LLFloaterGiphyPicker::setSelectionCallback(selection_callback_t callback)
{
    mSelectionCallback = callback;
}

void LLFloaterGiphyPicker::requestSearch()
{
    std::string query = mSearchEditor ? mSearchEditor->getValue().asString() : std::string();
    LLStringUtil::trim(query);
    if (query.empty())
    {
        setStatus("Enter a search term.");
        refreshControls();
        return;
    }

    setLoading(true);
    setStatus("Searching GIPHY...");
    const S32 request_id = ++mRequestId;
    LLGiphyClient::search(query,
        boost::bind(&LLFloaterGiphyPicker::onGiphyResults, getHandle(), request_id, _1, _2, _3));
}

void LLFloaterGiphyPicker::requestTrending()
{
    setLoading(true);
    setStatus("Loading trending GIFs...");
    const S32 request_id = ++mRequestId;
    LLGiphyClient::trending(
        boost::bind(&LLFloaterGiphyPicker::onGiphyResults, getHandle(), request_id, _1, _2, _3));
}

void LLFloaterGiphyPicker::populateResults(const LLGiphyClient::results_t& results)
{
    if (!mResultsList)
    {
        return;
    }

    mResultsList->deleteAllItems();
    for (LLGiphyClient::results_t::const_iterator it = results.begin(); it != results.end(); ++it)
    {
        LLScrollListItem::Params item;
        item.value(it->page_url);
        item.columns.add().column("title").value(it->title.empty() ? it->id : it->title);
        item.columns.add().column("url").value(it->page_url);
        mResultsList->addRow(item);
    }
}

void LLFloaterGiphyPicker::setStatus(const std::string& status)
{
    if (mStatusText)
    {
        mStatusText->setText(status);
    }
}

void LLFloaterGiphyPicker::setLoading(bool loading)
{
    mLoading = loading;
    refreshControls();
}

void LLFloaterGiphyPicker::refreshControls()
{
    if (mSearchButton)
    {
        mSearchButton->setEnabled(!mLoading);
    }
    if (mTrendingButton)
    {
        mTrendingButton->setEnabled(!mLoading);
    }
    if (mUseButton)
    {
        mUseButton->setEnabled(!mLoading && !getSelectedURL().empty());
    }
    if (mSearchEditor)
    {
        mSearchEditor->setEnabled(!mLoading);
    }
}

std::string LLFloaterGiphyPicker::getSelectedURL() const
{
    if (!mResultsList || !mResultsList->getFirstSelected())
    {
        return std::string();
    }
    return mResultsList->getSelectedValue().asString();
}

void LLFloaterGiphyPicker::onSearchClicked()
{
    requestSearch();
}

void LLFloaterGiphyPicker::onTrendingClicked()
{
    requestTrending();
}

void LLFloaterGiphyPicker::onUseClicked()
{
    const std::string url = getSelectedURL();
    if (url.empty())
    {
        return;
    }

    if (mSelectionCallback)
    {
        mSelectionCallback(url);
        closeFloater();
        return;
    }

    LLWString wide_url = utf8str_to_wstring(url);
    LLClipboard::instance().copyToClipboard(wide_url, 0, static_cast<S32>(wide_url.size()));
    setStatus("GIF URL copied to clipboard.");
}

void LLFloaterGiphyPicker::onResultSelected()
{
    refreshControls();
}

// static
void LLFloaterGiphyPicker::onGiphyResults(LLHandle<LLFloater> handle,
                                          S32 request_id,
                                          bool success,
                                          const std::string& message,
                                          const LLGiphyClient::results_t& results)
{
    LLFloaterGiphyPicker* floater = dynamic_cast<LLFloaterGiphyPicker*>(handle.get());
    if (!floater || floater->mRequestId != request_id)
    {
        return;
    }

    floater->setLoading(false);
    if (success)
    {
        floater->populateResults(results);
        floater->setStatus(message.empty() ? "Select a GIF. Powered by GIPHY." : message);
    }
    else
    {
        floater->populateResults(LLGiphyClient::results_t());
        floater->setStatus(message.empty() ? "GIPHY request failed." : message);
    }
    floater->refreshControls();
}
