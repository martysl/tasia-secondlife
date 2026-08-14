/**
 * @file llmp3batchupload.cpp
 * @brief MP3 conversion/segmentation and normal-cost sound uploads.
 */
#include "llviewerprecompiledheaders.h"
#include "llmp3batchupload.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llfilepicker.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llviewernetwork.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llstatusbar.h"
#include "lluploaddialog.h"
#include "llviewerassetupload.h"
#include "llviewermenufile.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llvorbisencode.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#if LL_LINUX
# include <sys/wait.h>
# include <unistd.h>
#endif

void create_new_item(const std::string& name, const LLUUID& parent_id,
                     LLAssetType::EType asset_type, LLInventoryType::EType inv_type,
                     U32 next_owner_perm, std::function<void(const LLUUID&)> created_cb);

namespace
{
const F32 MP3_SEGMENT_MARGIN_SECONDS = 1.0f;

struct BatchPart
{
    S32 ordinal;
    std::string filename;
    LLUUID asset_id;
};

class MP3BatchContext;
typedef std::shared_ptr<MP3BatchContext> MP3BatchContextPtr;

class MP3BatchSoundUploadInfo final : public LLNewFileResourceUploadInfo
{
public:
    MP3BatchSoundUploadInfo(const BatchPart& part, const MP3BatchContextPtr& context, S32 cost);
    LLUUID finishUpload(LLSD& result) override;
    bool failedUpload(LLSD& result, std::string& reason) override;
private:
    BatchPart mPart;
    MP3BatchContextPtr mContext;
};

class MP3BatchContext : public std::enable_shared_from_this<MP3BatchContext>
{
public:
    explicit MP3BatchContext(std::vector<BatchPart> parts) : mParts(std::move(parts)), mPending((S32)mParts.size()) {}
    ~MP3BatchContext() { cleanup(); }

    void start(S32 cost)
    {
        for (const BatchPart& part : mParts)
        {
            LLResourceUploadInfo::ptr_t info = std::make_shared<MP3BatchSoundUploadInfo>(part, shared_from_this(), cost);
            upload_new_resource(info);
        }
    }

    void succeeded(const BatchPart& part, const LLSD& result)
    {
        // Only server-confirmed asset IDs belong in the report.
        const LLUUID inventory_id = result["new_inventory_item"].asUUID();
        const LLUUID asset_id = result["new_asset"].asUUID();
        if (inventory_id.notNull() && asset_id.notNull())
        {
            BatchPart confirmed(part);
            confirmed.asset_id = asset_id;
            mSucceeded.push_back(confirmed);
        }
        completeOne();
    }

    void failed(const std::string& reason)
    {
        ++mFailed;
        LL_WARNS("MP3BatchUpload") << "Part upload failed: " << reason << LL_ENDL;
        completeOne();
    }

private:
    void completeOne()
    {
        if (--mPending == 0)
        {
            if (!mSucceeded.empty()) createReportNotecard();
            LLSD args;
            args["SUCCESS"] = (S32)mSucceeded.size();
            args["FAILED"] = mFailed;
            LLNotificationsUtil::add("MP3BatchSoundUploadFinished", args);
            cleanup();
        }
    }

    void createReportNotecard()
    {
        std::sort(mSucceeded.begin(), mSucceeded.end(), [](const BatchPart& a, const BatchPart& b) { return a.ordinal < b.ordinal; });
        std::ostringstream text;
        text << "MP3 batch sound upload results (server-confirmed assets only)\n\n";
        for (const BatchPart& part : mSucceeded)
            text << "Part " << (part.ordinal + 1) << ": " << part.asset_id.asString() << "\n";
        LLNotecard card(LLNotecard::MAX_SIZE);
        card.setText(text.str());
        std::stringstream serialized;
        card.exportStream(serialized);
        const std::string contents = serialized.str();
        const MP3BatchContextPtr self = shared_from_this();
        create_new_item("MP3 Upload Results", gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD),
            LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD, 0,
            [self, contents](const LLUUID& item_id)
            {
                LLViewerRegion* region = gAgent.getRegion();
                if (!region || item_id.isNull()) return;
                const std::string url = region->getCapability("UpdateNotecardAgentInventory");
                if (url.empty())
                {
                    LL_WARNS("MP3BatchUpload") << "Notecard capability unavailable; results item was not populated." << LL_ENDL;
                    return;
                }
                LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(item_id, LLAssetType::AT_NOTECARD,
                    contents, nullptr, nullptr);
                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            });
    }

    void cleanup()
    {
        for (const BatchPart& part : mParts)
            if (!part.filename.empty()) LLFile::remove(part.filename);
        mParts.clear();
    }

    std::vector<BatchPart> mParts;
    std::vector<BatchPart> mSucceeded;
    S32 mPending;
    S32 mFailed = 0;
};

MP3BatchSoundUploadInfo::MP3BatchSoundUploadInfo(const BatchPart& part, const MP3BatchContextPtr& context, S32 cost)
    : LLNewFileResourceUploadInfo(part.filename, "MP3 part " + std::to_string(part.ordinal + 1), "Converted MP3 segment", 0,
        LLFolderType::FT_SOUND, LLInventoryType::IT_SOUND,
        LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"),
        LLFloaterPerms::getEveryonePerms("Uploads"), cost), mPart(part), mContext(context) {}

LLUUID MP3BatchSoundUploadInfo::finishUpload(LLSD& result)
{
    LLUUID item_id = LLNewFileResourceUploadInfo::finishUpload(result);
    mContext->succeeded(mPart, result);
    return item_id;
}

bool MP3BatchSoundUploadInfo::failedUpload(LLSD& result, std::string& reason)
{
    mContext->failed(reason);
    return false;
}

bool run_ffmpeg_segment(const std::string& input, const std::string& pattern, F32 segment_seconds)
{
#if LL_LINUX
    std::string executable = gDirUtilp->getExecutableDir();
    gDirUtilp->append(executable, "tasia-ffmpeg");
    llstat st;
    if (LLFile::stat(executable, &st) != 0)
    {
        LLNotificationsUtil::add("MP3BatchSoundFfmpegMissing");
        return false;
    }
    const pid_t pid = fork();
    if (pid == 0)
    {
        execl(executable.c_str(), executable.c_str(), "-y", "-v", "error", "-i", input.c_str(), "-ar", "44100", "-ac", "2",
              "-c:a", "pcm_s16le", "-f", "segment", "-segment_time", llformat("%.2f", segment_seconds).c_str(), pattern.c_str(),
              static_cast<char*>(NULL));
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
    LLNotificationsUtil::add("MP3BatchSoundFfmpegMissing");
    return false;
#endif
}

void begin_confirmed_batch(const MP3BatchContextPtr& context, S32 cost, const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0) context->start(cost);
}

void convert_and_confirm(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;
    const std::string input = filenames.front();
    std::string extension = gDirUtilp->getExtension(input);
    LLStringUtil::toLower(extension);
    if (extension != "mp3") { LLNotificationsUtil::add("MP3BatchSoundNotMp3"); return; }

    const F32 maximum = LLGridManager::instance().isInSecondLife() ? LLVORBIS_CLIP_MAX_TIME : LLVORBIS_CLIP_MAX_TIME_OPENSIM;
    const std::string prefix = gDirUtilp->getTempFilename() + "_mp3part_";
    const std::string pattern = prefix + "%03d.wav";
    if (!run_ffmpeg_segment(input, pattern, maximum - MP3_SEGMENT_MARGIN_SECONDS)) return;

    std::vector<BatchPart> parts;
    for (S32 ordinal = 0; ; ++ordinal)
    {
        const std::string part = prefix + llformat("%03d.wav", ordinal);
        if (!gDirUtilp->fileExists(part)) break;
        std::string error;
        if (check_for_invalid_wav_formats(part, error, LLGridManager::instance().isInSecondLife()))
        {
            LLFile::remove(part);
            for (const BatchPart& accepted : parts) LLFile::remove(accepted.filename);
            LLSD args; args["FILE"] = part; args["MAX_LENGTH"] = llformat("%.0f", maximum);
            LLNotificationsUtil::add(error, args);
            return;
        }
        parts.push_back({ ordinal, part, LLUUID::null });
    }
    if (parts.empty()) { LLNotificationsUtil::add("MP3BatchSoundConversionFailed"); return; }

    S32 unit_cost = 0;
    LLAssetType::EType sound_type = LLAssetType::AT_SOUND;
    if (!LLAgentBenefitsMgr::current().findUploadCost(sound_type, unit_cost))
    {
        for (const BatchPart& part : parts) LLFile::remove(part.filename);
        LLNotificationsUtil::add("MP3BatchSoundCostUnavailable"); return;
    }
    const S32 total_cost = unit_cost * (S32)parts.size();
    if (total_cost > gStatusBar->getBalance())
    {
        for (const BatchPart& part : parts) LLFile::remove(part.filename);
        LLSD args; args["COST"] = total_cost; args["COUNT"] = (S32)parts.size(); args["BALANCE"] = gStatusBar->getBalance();
        LLNotificationsUtil::add("NotEnoughMoneyForBulkUpload", args); return;
    }
    MP3BatchContextPtr context = std::make_shared<MP3BatchContext>(parts);
    LLSD args; args["COUNT"] = (S32)parts.size(); args["UNIT_COST"] = unit_cost; args["TOTAL_COST"] = total_cost;
    LLNotificationsUtil::add("ConfirmMP3BatchSoundUpload", args, LLSD(), boost::bind(&begin_confirmed_batch, context, unit_cost, _1, _2));
}
} // namespace

void mp3_batch_sound_file_picked(const std::vector<std::string>& filenames) { convert_and_confirm(filenames); }
void start_mp3_batch_sound_upload()
{
    if (gAgentCamera.cameraMouselook()) gAgentCamera.changeCameraToDefault();
    LLFilePickerReplyThread::startPicker(boost::bind(&mp3_batch_sound_file_picked, _1), LLFilePicker::FFLOAD_ALL, false);
}
