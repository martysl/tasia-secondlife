/**
 * @file llfloatertasiafeed.cpp
 * @brief TasiaFeed snapshot upload floater
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

#include "llfloatertasiafeed.h"

#include "llagent.h"
#include "llagentui.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "llgridmanager.h"
#include "lllineeditor.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llslurl.h"
#include "llsnapshotlivepreview.h"
#include "lltrans.h"
#include "llui.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerparcelmgr.h"
#include "llweb.h"

#include "tasiafeedconnect.h"

// --------------------------------------------------------------------------
// LLFloaterTasiaFeed
// --------------------------------------------------------------------------

LLFloaterTasiaFeed::LLFloaterTasiaFeed(const LLSD& key)
    : LLFloater(key)
    , mTitleEditor(nullptr)
    , mDescriptionEditor(nullptr)
    , mVisibilityCombo(nullptr)
    , mMaturityCombo(nullptr)
    , mUploading(false)
{
    mCommitCallbackRegistrar.add("TasiaFeed.Send",       boost::bind(&LLFloaterTasiaFeed::onSend, this));
    mCommitCallbackRegistrar.add("TasiaFeed.Cancel",     boost::bind(&LLFloaterTasiaFeed::onCancel, this));
}

LLFloaterTasiaFeed::~LLFloaterTasiaFeed()
{
}

bool LLFloaterTasiaFeed::postBuild()
{
    mTitleEditor = getChild<LLLineEditor>("title_edit");
    mDescriptionEditor = getChild<LLLineEditor>("description_edit");
    mVisibilityCombo = getChild<LLComboBox>("visibility_combo");
    mMaturityCombo = getChild<LLComboBox>("maturity_combo");
    return true;
}

void LLFloaterTasiaFeed::onOpen(const LLSD& key)
{
    mUploading = false;
    mResultURL.clear();
    mResultMessage.clear();
    mResultData = LLSD();

    // Set default values from metadata
    if (mTitleEditor)
    {
        // Try to use region name as default title
        LLViewerRegion* region = gAgent.getRegion();
        if (region)
        {
            mTitleEditor->setValue(region->getName());
        }
    }

    // Set default visibility
    if (mVisibilityCombo)
    {
        mVisibilityCombo->selectByValue("public");
    }

    // Set default maturity
    if (mMaturityCombo)
    {
        mMaturityCombo->selectByValue("general");
    }
}

void LLFloaterTasiaFeed::draw()
{
    LLFloater::draw();

    // Update button state based on upload progress
    LLButton* send_btn = findChild<LLButton>("send_btn");
    if (send_btn)
    {
        send_btn->setEnabled(!mUploading);
    }

    // Show result
    if (!mResultURL.empty())
    {
        LLTextBase* result_text = getChild<LLTextBase>("result_text");
        if (result_text)
        {
            result_text->setText(mResultMessage);
            result_text->setVisible(true);
        }
    }
}

// static
void LLFloaterTasiaFeed::update()
{
    // Force redraw to update preview
    LLFloaterTasiaFeed* floater = LLFloaterReg::findTypedInstance<LLFloaterTasiaFeed>("tasiafeed");
    if (floater)
    {
        floater->draw();
    }
}

void LLFloaterTasiaFeed::onSend()
{
    if (mUploading) return;

    mUploading = true;
    mResultURL.clear();
    mResultMessage.clear();

    // Get the snapshot preview
    LLSnapshotLivePreview* previewp = getPreviewView();
    if (!previewp)
    {
        LLNotificationsUtil::add("TasiaFeedUploadFailed");
        mUploading = false;
        return;
    }

    // Build metadata
    LLSD metadata;

    metadata["title"] = mTitleEditor ? mTitleEditor->getValue().asString() : "";
    metadata["description"] = mDescriptionEditor ? mDescriptionEditor->getValue().asString() : "";
    metadata["visibility"] = mVisibilityCombo ? mVisibilityCombo->getValue().asString() : "public";
    metadata["maturity"] = mMaturityCombo ? mMaturityCombo->getValue().asString() : "general";

    // Avatar name
    metadata["avatar_name"] = LLSLURL("agent", gAgent.getID(), "inventory").getSLURLString();
    // Get just the display name
    std::string avatar_name;
    if (LLAgentUI::buildFullname(avatar_name))
    {
        metadata["avatar_name"] = avatar_name;
    }

    // Grid info
    metadata["grid_name"] = LLGridManager::instance().getGridLabel();

    // Region and position
    LLViewerRegion* region = gAgent.getRegion();
    if (region)
    {
        metadata["region_name"] = region->getName();
        LLVector3d pos = gAgent.getPositionGlobal();
        std::string pos_str = llformat("%.1f,%.1f,%.1f", pos.mdX, pos.mdY, pos.mdZ);
        metadata["position"] = pos_str;
    }

    // User UUID for attribution
    metadata["user_uuid"] = gAgent.getID().asString();

    // Viewer version info
    metadata["viewer_ver"] = LLVersionInfo::instance().getChannelAndVersion();
    metadata["commit_sha"] = LLVersionInfo::getGitHash();
    metadata["build_num"] = stringize(LLVersionInfo::instance().getBuild());

    // Upload
    TasiaFeedUpload::uploadSnapshot(metadata, previewp->getFormattedImage().get(),
        boost::bind(&LLFloaterTasiaFeed::uploadCallback, this, _1, _2));
}

void LLFloaterTasiaFeed::onCancel()
{
    closeFloater();
}

void LLFloaterTasiaFeed::uploadCallback(bool success, const LLSD& response)
{
    mUploading = false;

    if (success)
    {
        mResultURL = response["post_url"].asString();
        mResultMessage = "Snapshot uploaded!\n" + mResultURL;
        LLSD args;
        args["URL"] = mResultURL;
        LLNotificationsUtil::add("TasiaFeedUploadComplete", args);
    }
    else
    {
        std::string err_msg = response.has("message") ? response["message"].asString() : "Upload failed";
        mResultMessage = "Upload failed: " + err_msg;
        LLSD args;
        args["MESSAGE"] = err_msg;
        LLNotificationsUtil::add("TasiaFeedUploadFailed", args);
    }
}

LLSnapshotLivePreview* LLFloaterTasiaFeed::getPreviewView()
{
    // Try to get the preview from the snapshot floater
    LLFloater* snapshot_floater = LLFloaterReg::findInstance("snapshot");
    if (snapshot_floater)
    {
        // Find the preview panel within the snapshot floater
        LLPanel* preview_panel = snapshot_floater->findChild<LLPanel>("preview_panel", true);
        if (preview_panel)
        {
            return dynamic_cast<LLSnapshotLivePreview*>(preview_panel);
        }
    }
    return nullptr;
}
