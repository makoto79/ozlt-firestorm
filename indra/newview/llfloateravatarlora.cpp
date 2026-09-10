/**
 * @file llfloateravatarlora.cpp
 * @brief Floater for automated avatar capture series (LoRA training datasets)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2026
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

#include "llfloateravatarlora.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldate.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llimagepng.h"
#include "lllineeditor.h"
#include "llmutelist.h"
#include "llprogressbar.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

#include <cctype>

// Only avatars this close to us are offered as capture targets; same range the
// poser uses for its avatar list.
static const F32 LORA_NEARBY_RANGE = 50.f;

// Framing heights, as a fraction of the measured body height.
static const F32 FRAME_FULLBODY = 1.15f;
static const F32 FRAME_UPPERBODY = 0.50f;
static const F32 FRAME_MIDBODY = 0.40f;
static const F32 FRAME_LEGS = 0.55f;

// Close-up framing heights in meters, for an avatar of REFERENCE_HEIGHT.
static const F32 REFERENCE_HEIGHT = 1.8f;
static const F32 FRAME_HEAD = 0.35f;
static const F32 FRAME_HANDS = 0.30f;
static const F32 FRAME_FEET = 0.32f;

// A subject is never wider than this fraction of the height we frame it at.
// Used so portrait output does not crop the sides.
static const F32 SUBJECT_WIDTH_RATIO = 0.75f;

// Detail shots fan out this far to either side of the straight side view.
static const F32 DETAIL_AZIMUTH_SPREAD = 55.f;
static const F32 HANDS_ELEVATION = 15.f;
static const F32 FEET_ELEVATION = 20.f;

// Distances from the last joint to the actual silhouette, at reference scale.
static const F32 ANKLE_TO_SOLE = 0.04f;
static const F32 HEAD_TO_CROWN = 0.13f;
static const F32 EYE_ABOVE_HEAD_JOINT = 0.08f;

// Render overrides while a series runs. LOD is normally computed for the
// user's own camera, not ours, so close-ups would otherwise be captured at
// whatever detail level the real camera distance implies.
static const F32 CAPTURE_VOLUME_LOD = 4.f;
static const F32 CAPTURE_AVATAR_LOD = 1.f;
static const U32 CAPTURE_MAX_NON_IMPOSTORS = 64;
static const F32 CAPTURE_NEAR_CLIP = 0.1f;
static const S32 CAPTURE_RENDER_PASSES = 2;

const LLFloaterAvatarLora::GroupDesc LLFloaterAvatarLora::sGroups[SHOT_GROUP_COUNT] =
{   //  group           ctrl suffix    count  per side  levels
    { SHOT_FULLBODY,  "fullbody",       8,    false,    true  },
    { SHOT_UPPERBODY, "upperbody",      8,    false,    false },
    { SHOT_MIDBODY,   "midbody",        8,    false,    false },
    { SHOT_LEGS,      "legs",           8,    false,    false },
    { SHOT_HEAD,      "head",          16,    false,    false },
    { SHOT_HANDS,     "hands",          3,    true,     false },
    { SHOT_FEET,      "feet",           3,    true,     false },
};

const LLFloaterAvatarLora::ResolutionPreset LLFloaterAvatarLora::sResolutionPresets[] =
{
    { "1024 x 1024 (SDXL / Flux)", 1024, 1024 },
    { "768 x 768 (SD 1.5)",         768,  768 },
    { "512 x 512",                  512,  512 },
    { "1536 x 1536",               1536, 1536 },
    { "2048 x 2048",               2048, 2048 },
    { "Custom",                       0,    0 },
};

const S32 LLFloaterAvatarLora::sNumPresets = sizeof(sResolutionPresets) / sizeof(sResolutionPresets[0]);

LLFloaterAvatarLora::LLFloaterAvatarLora(const LLSD& key)
    : LLFloater(key)
    , mAvatarList(nullptr)
    , mRefreshBtn(nullptr)
    , mAvatarInfoText(nullptr)
    , mLevelsSpinner(nullptr)
    , mFovBodySpinner(nullptr)
    , mFovDetailSpinner(nullptr)
    , mMarginSpinner(nullptr)
    , mElevationHighSpinner(nullptr)
    , mElevationLowSpinner(nullptr)
    , mResolutionCombo(nullptr)
    , mCustomWidthSpinner(nullptr)
    , mCustomHeightSpinner(nullptr)
    , mOutputDirEditor(nullptr)
    , mBrowseBtn(nullptr)
    , mFolderStructureCombo(nullptr)
    , mRepeatsSpinner(nullptr)
    , mTriggerEditor(nullptr)
    , mClassEditor(nullptr)
    , mExtraTagsEditor(nullptr)
    , mWriteCaptionsCheck(nullptr)
    , mTotalImagesText(nullptr)
    , mEstimatedSizeText(nullptr)
    , mPreviewPrevBtn(nullptr)
    , mPreviewNextBtn(nullptr)
    , mPreviewLabel(nullptr)
    , mStartBtn(nullptr)
    , mStopBtn(nullptr)
    , mProgressBar(nullptr)
    , mStatusText(nullptr)
    , mDelaySpinner(nullptr)
    , mPreviewIndex(0)
    , mPreviewActive(false)
    , mSavedDefaultFov(0.f)
    , mSavedCameraConstraints(false)
    , mCapturing(false)
    , mCountdownActive(false)
    , mCurrentShot(0)
    , mImageWidth(1024)
    , mImageHeight(1024)
    , mSavedImages(0)
    , mCaptureCamFov(0.f)
    , mCaptureCamAspect(1.f)
    , mCaptureCamNear(0.25f)
    , mRenderStateApplied(false)
    , mSavedDepthOfField(false)
    , mSavedVolumeLOD(1.f)
    , mSavedAvatarLOD(1.f)
    , mSavedMaxNonImpostors(12)
    , mSavedNameTagMode(0)
{
    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        mGroupCounter[i] = 0;
    }

    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        mGroupEnable[i] = nullptr;
        mGroupCount[i] = nullptr;
    }
}

LLFloaterAvatarLora::~LLFloaterAvatarLora()
{
    if (mCapturing)
    {
        stopCapture(false);
    }
}

bool LLFloaterAvatarLora::postBuild()
{
    mAvatarList = getChild<LLScrollListCtrl>("avatar_list");
    mRefreshBtn = getChild<LLButton>("refresh_avatars_btn");
    mAvatarInfoText = getChild<LLTextBox>("avatar_info");

    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        mGroupEnable[i] = getChild<LLCheckBoxCtrl>(std::string("shot_") + sGroups[i].mCtrlSuffix);
        mGroupCount[i] = getChild<LLSpinCtrl>(std::string("count_") + sGroups[i].mCtrlSuffix);

        mGroupEnable[i]->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
        mGroupCount[i]->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
    }

    mLevelsSpinner = getChild<LLSpinCtrl>("fullbody_levels");
    mFovBodySpinner = getChild<LLSpinCtrl>("fov_body");
    mFovDetailSpinner = getChild<LLSpinCtrl>("fov_detail");
    mMarginSpinner = getChild<LLSpinCtrl>("framing_margin");
    mElevationHighSpinner = getChild<LLSpinCtrl>("elevation_high");
    mElevationLowSpinner = getChild<LLSpinCtrl>("elevation_low");
    mDelaySpinner = getChild<LLSpinCtrl>("capture_delay");

    mResolutionCombo = getChild<LLComboBox>("resolution_combo");
    mCustomWidthSpinner = getChild<LLSpinCtrl>("custom_width");
    mCustomHeightSpinner = getChild<LLSpinCtrl>("custom_height");
    mOutputDirEditor = getChild<LLLineEditor>("output_dir");
    mBrowseBtn = getChild<LLButton>("browse_btn");
    mFolderStructureCombo = getChild<LLComboBox>("folder_structure");
    mRepeatsSpinner = getChild<LLSpinCtrl>("kohya_repeats");
    mTriggerEditor = getChild<LLLineEditor>("trigger_word");
    mClassEditor = getChild<LLLineEditor>("class_token");
    mExtraTagsEditor = getChild<LLLineEditor>("extra_tags");
    mWriteCaptionsCheck = getChild<LLCheckBoxCtrl>("write_captions");

    mTotalImagesText = getChild<LLTextBox>("total_images_result");
    mEstimatedSizeText = getChild<LLTextBox>("estimated_size");
    mPreviewPrevBtn = getChild<LLButton>("preview_prev_btn");
    mPreviewNextBtn = getChild<LLButton>("preview_next_btn");
    mPreviewLabel = getChild<LLTextBox>("preview_label");
    mStartBtn = getChild<LLButton>("start_btn");
    mStopBtn = getChild<LLButton>("stop_btn");
    mProgressBar = getChild<LLProgressBar>("capture_progress");
    mStatusText = getChild<LLTextBox>("status_text");

    mAvatarList->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onAvatarSelected, this));
    mRefreshBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::refreshAvatarList, this));
    mBrowseBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onBrowseBtn, this));
    mResolutionCombo->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onResolutionPreset, this));
    mCustomWidthSpinner->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
    mCustomHeightSpinner->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
    mLevelsSpinner->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
    mOutputDirEditor->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));
    mFolderStructureCombo->setCommitCallback(boost::bind(&LLFloaterAvatarLora::updateUIState, this));

    mPreviewPrevBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onPreviewStep, this, -1));
    mPreviewNextBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onPreviewStep, this, 1));
    mStartBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onStartBtn, this));
    mStopBtn->setCommitCallback(boost::bind(&LLFloaterAvatarLora::onStopBtn, this));

    mProgressBar->setValue(0.0);

    return true;
}

void LLFloaterAvatarLora::onOpen(const LLSD& key)
{
    loadSettings();
    refreshAvatarList();
    onResolutionPreset();
    updateUIState();

    mStatusText->setText(getString("status_ready"));
}

void LLFloaterAvatarLora::onClose(bool app_quitting)
{
    if (mCapturing)
    {
        stopCapture(!app_quitting);
    }

    stopPreview();
    saveSettings();
}

//---------------------------------------------------------------------------
// Target avatar
//---------------------------------------------------------------------------

// static
LLVOAvatar* LLFloaterAvatarLora::getAvatarByUuid(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return nullptr;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (character->getID() != avatar_id)
            continue;

        return dynamic_cast<LLVOAvatar*>(character);
    }

    return nullptr;
}

// static
bool LLFloaterAvatarLora::isEligibleAvatar(LLVOAvatar* avatar)
{
    if (!avatar || avatar->isDead())
        return false;

    // Animesh has a skeleton too, but its proportions are arbitrary - out of
    // scope for a humanoid shot plan.
    if (avatar->isControlAvatar())
        return false;

    if (LLMuteList::getInstance()->isMuted(avatar->getID()))
        return false;

    if (avatar->isSelf())
        return true;

    if (!isAgentAvatarValid())
        return false;

    LLVector3 separation = avatar->getCharacterPosition() - gAgentAvatarp->getCharacterPosition();
    return separation.magVec() < LORA_NEARBY_RANGE;
}

void LLFloaterAvatarLora::refreshAvatarList()
{
    LLUUID previously_selected = getSelectedAvatarId();
    if (previously_selected.isNull())
    {
        previously_selected = mSelectedAvatarId;
    }

    mAvatarList->deleteAllItems();

    // Own avatar first, everyone else after it - the common case is capturing
    // yourself, and the list should not reorder itself as people walk around.
    std::vector<LLVOAvatar*> targets;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!isEligibleAvatar(avatar))
            continue;

        if (avatar->isSelf())
        {
            targets.insert(targets.begin(), avatar);
        }
        else
        {
            targets.push_back(avatar);
        }
    }

    for (LLVOAvatar* avatar : targets)
    {
        const LLUUID& avatar_id = avatar->getID();

        std::string name;
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(avatar_id, &av_name))
        {
            name = av_name.getDisplayName();
        }
        else
        {
            name = avatar->getFullname();
        }

        if (name.empty())
        {
            name = avatar_id.asString();
        }

        if (avatar->isSelf())
        {
            name += " " + getString("self_suffix");
        }

        LLSD row;
        row["value"] = avatar_id;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;

        mAvatarList->addElement(row);
    }

    if (previously_selected.notNull())
    {
        mAvatarList->selectByValue(LLSD(previously_selected));
    }

    if (!mAvatarList->getFirstSelected())
    {
        mAvatarList->selectFirstItem();
    }

    onAvatarSelected();
}

void LLFloaterAvatarLora::onAvatarSelected()
{
    LLUUID selected = getSelectedAvatarId();
    if (selected.notNull())
    {
        mSelectedAvatarId = selected;
    }

    // The plan is measured per avatar, so it has to be rebuilt after a switch.
    mShots.clear();
    mPreviewIndex = 0;

    updateAvatarInfo();
    updateUIState();
}

LLUUID LLFloaterAvatarLora::getSelectedAvatarId() const
{
    LLScrollListItem* item = mAvatarList->getFirstSelected();
    if (!item)
        return LLUUID::null;

    return item->getValue().asUUID();
}

LLVOAvatar* LLFloaterAvatarLora::getSelectedAvatar() const
{
    return getAvatarByUuid(getSelectedAvatarId());
}

void LLFloaterAvatarLora::updateAvatarInfo()
{
    LLVOAvatar* avatar = getSelectedAvatar();
    if (!avatar)
    {
        mAvatarInfoText->setText(getString("info_no_avatar"));
        return;
    }

    AvatarMetrics metrics;
    bool measured = measureAvatar(avatar, metrics);

    LLStringUtil::format_map_t args;
    args["HEIGHT"] = llformat("%.2f", measured ? metrics.mHeight : avatar->mBodySize.mV[VZ]);
    args["SKELETON"] = measured ? getString("skeleton_ok") : getString("skeleton_missing");

    mAvatarInfoText->setText(getString("info_avatar", args));
}

//---------------------------------------------------------------------------
// Skeleton measurement and shot plan
//---------------------------------------------------------------------------

bool LLFloaterAvatarLora::measureAvatar(LLVOAvatar* avatar, AvatarMetrics& metrics) const
{
    if (!avatar || avatar->isDead())
        return false;

    LLJoint* head = avatar->getJoint("mHead");
    LLJoint* chest = avatar->getJoint("mChest");
    LLJoint* pelvis = avatar->getJoint("mPelvis");
    LLJoint* wrist[2] = { avatar->getJoint("mWristLeft"), avatar->getJoint("mWristRight") };
    LLJoint* elbow[2] = { avatar->getJoint("mElbowLeft"), avatar->getJoint("mElbowRight") };
    LLJoint* ankle[2] = { avatar->getJoint("mAnkleLeft"), avatar->getJoint("mAnkleRight") };
    LLJoint* hip[2] = { avatar->getJoint("mHipLeft"), avatar->getJoint("mHipRight") };

    if (!head || !chest || !pelvis || !wrist[0] || !wrist[1] || !elbow[0] || !elbow[1]
        || !ankle[0] || !ankle[1] || !hip[0] || !hip[1])
    {
        return false;
    }

    // mBodySize is derived from the rest pose, so it stays a stable size
    // reference even while the avatar is posed.
    F32 reference = avatar->mBodySize.mV[VZ];
    metrics.mScale = (reference > 0.5f) ? reference / REFERENCE_HEIGHT : 1.f;

    LLVector3 head_pos = head->getWorldPosition();
    metrics.mChest = chest->getWorldPosition();
    metrics.mPelvis = pelvis->getWorldPosition();

    for (S32 side = 0; side < 2; ++side)
    {
        metrics.mWrist[side] = wrist[side]->getWorldPosition();
        metrics.mElbow[side] = elbow[side]->getWorldPosition();
        metrics.mAnkle[side] = ankle[side]->getWorldPosition();
    }
    metrics.mHipMid = (hip[0]->getWorldPosition() + hip[1]->getWorldPosition()) * 0.5f;

    // Top of the head: the same sqrt(2) skull approximation the viewer uses to
    // compute body size, falling back to a fixed offset if mSkull is missing.
    F32 head_top = head_pos.mV[VZ] + HEAD_TO_CROWN * metrics.mScale;
    if (LLJoint* skull = avatar->getJoint("mSkull"))
    {
        F32 estimate = head_pos.mV[VZ] + F_SQRT2 * (skull->getWorldPosition().mV[VZ] - head_pos.mV[VZ]);
        if (estimate > head_pos.mV[VZ])
        {
            head_top = estimate;
        }
    }
    metrics.mHeadTopZ = head_top;

    F32 lowest = llmin(metrics.mAnkle[0].mV[VZ], metrics.mAnkle[1].mV[VZ]);
    if (LLJoint* foot_l = avatar->getJoint("mFootLeft"))
    {
        lowest = llmin(lowest, foot_l->getWorldPosition().mV[VZ]);
    }
    if (LLJoint* foot_r = avatar->getJoint("mFootRight"))
    {
        lowest = llmin(lowest, foot_r->getWorldPosition().mV[VZ]);
    }
    metrics.mSoleZ = lowest - ANKLE_TO_SOLE * metrics.mScale;

    metrics.mHeight = metrics.mHeadTopZ - metrics.mSoleZ;
    if (metrics.mHeight < 0.3f)
    {
        return false;
    }

    // Head shots are centred on the eyes, which puts the crown and the chin
    // inside the frame with a little headroom.
    LLJoint* eye_l = avatar->getJoint("mEyeLeft");
    LLJoint* eye_r = avatar->getJoint("mEyeRight");
    if (eye_l && eye_r)
    {
        metrics.mHeadCenter = (eye_l->getWorldPosition() + eye_r->getWorldPosition()) * 0.5f;
    }
    else
    {
        metrics.mHeadCenter = head_pos;
        metrics.mHeadCenter.mV[VZ] += EYE_ABOVE_HEAD_JOINT * metrics.mScale;
    }

    LLVector3 forward = LLVector3::x_axis * avatar->getRenderRotation();
    forward.mV[VZ] = 0.f;
    if (forward.magVecSquared() < 0.0001f)
    {
        forward = LLVector3::x_axis;
    }
    forward.normalize();
    metrics.mForward = forward;

    metrics.mAxis = metrics.mPelvis;
    metrics.mValid = true;

    return true;
}

F32 LLFloaterAvatarLora::currentAspect() const
{
    S32 width = getCurrentWidth();
    S32 height = getCurrentHeight();
    if (width <= 0 || height <= 0)
    {
        return 1.f;
    }

    return (F32)width / (F32)height;
}

F32 LLFloaterAvatarLora::distanceForFrameHeight(F32 frame_height, F32 fov_rad) const
{
    F32 margin = 1.f + llmax(0.f, (F32)mMarginSpinner->get()) * 0.01f;
    F32 tan_half = tanf(fov_rad * 0.5f);
    if (tan_half < 0.0001f)
    {
        tan_half = 0.0001f;
    }

    F32 distance = (frame_height * 0.5f * margin) / tan_half;

    // Portrait output has a narrower horizontal angle than vertical, so back
    // off far enough that the subject's width fits as well.
    F32 aspect = currentAspect();
    F32 half_width = frame_height * SUBJECT_WIDTH_RATIO * 0.5f * margin;
    F32 distance_h = half_width / (tan_half * llmax(0.1f, aspect));

    return llmax(llmax(distance, distance_h), 0.25f);
}

LLVector3 LLFloaterAvatarLora::cameraOffsetFor(const LLVector3& forward, F32 azimuth_deg, F32 elevation_deg, F32 distance) const
{
    LLQuaternion yaw(azimuth_deg * DEG_TO_RAD, LLVector3::z_axis);
    LLVector3 direction = forward * yaw;

    F32 elevation_rad = elevation_deg * DEG_TO_RAD;
    LLVector3 offset = direction * (distance * cosf(elevation_rad));
    offset.mV[VZ] += distance * sinf(elevation_rad);

    return offset;
}

void LLFloaterAvatarLora::addOrbit(EShotGroup group, S32 steps, const LLVector3& target,
                                   F32 frame_height, F32 fov_rad, const std::vector<F32>& elevations,
                                   const AvatarMetrics& metrics)
{
    steps = llmax((S32)1, steps);
    F32 distance = distanceForFrameHeight(frame_height, fov_rad);
    LLVector3d target_global = gAgent.getPosGlobalFromAgent(target);

    for (F32 elevation : elevations)
    {
        for (S32 i = 0; i < steps; ++i)
        {
            LoraShot shot;
            shot.mGroup = group;
            shot.mSide = -1;
            shot.mAzimuthDeg = 360.f * (F32)i / (F32)steps;
            shot.mElevationDeg = elevation;
            shot.mTargetGlobal = target_global;
            shot.mDistance = distance;
            shot.mFovRad = fov_rad;
            shot.mCameraGlobal = target_global +
                LLVector3d(cameraOffsetFor(metrics.mForward, shot.mAzimuthDeg, elevation, distance));

            mShots.push_back(shot);
        }
    }
}

void LLFloaterAvatarLora::addDetailShots(EShotGroup group, S32 steps_per_side, const LLVector3 targets[2],
                                         F32 frame_height, F32 fov_rad, F32 elevation_deg,
                                         const AvatarMetrics& metrics)
{
    steps_per_side = llmax((S32)1, steps_per_side);
    F32 distance = distanceForFrameHeight(frame_height, fov_rad);

    for (S32 side = 0; side < 2; ++side)
    {
        // Left limbs are shot from the avatar's left (+90 degrees), right ones
        // from its right; the fan mirrors itself through the sign of the base.
        F32 base_azimuth = (side == 0) ? 90.f : -90.f;
        LLVector3d target_global = gAgent.getPosGlobalFromAgent(targets[side]);

        for (S32 i = 0; i < steps_per_side; ++i)
        {
            F32 spread = (steps_per_side == 1) ? 0.f
                : ((F32)i / (F32)(steps_per_side - 1) * 2.f - 1.f) * DETAIL_AZIMUTH_SPREAD;

            LoraShot shot;
            shot.mGroup = group;
            shot.mSide = side;
            shot.mAzimuthDeg = base_azimuth + spread;
            shot.mElevationDeg = elevation_deg;
            shot.mTargetGlobal = target_global;
            shot.mDistance = distance;
            shot.mFovRad = fov_rad;
            shot.mCameraGlobal = target_global +
                LLVector3d(cameraOffsetFor(metrics.mForward, shot.mAzimuthDeg, elevation_deg, distance));

            mShots.push_back(shot);
        }
    }
}

bool LLFloaterAvatarLora::buildShotList()
{
    mShots.clear();

    AvatarMetrics metrics;
    if (!measureAvatar(getSelectedAvatar(), metrics))
    {
        return false;
    }

    // Kept for the manifest: these are the measurements the plan was built from.
    mCaptureMetrics = metrics;

    F32 fov_body = llclamp((F32)mFovBodySpinner->get(), 5.f, 120.f) * DEG_TO_RAD;
    F32 fov_detail = llclamp((F32)mFovDetailSpinner->get(), 5.f, 120.f) * DEG_TO_RAD;
    F32 height = metrics.mHeight;

    // Full body is shot from several heights; everything else stays level.
    std::vector<F32> body_levels;
    S32 num_levels = llmax((S32)1, (S32)mLevelsSpinner->get());
    F32 low = (F32)mElevationLowSpinner->get();
    F32 high = (F32)mElevationHighSpinner->get();
    for (S32 level = 0; level < num_levels; ++level)
    {
        F32 t = (num_levels == 1) ? 0.f : (F32)level / (F32)(num_levels - 1);
        body_levels.push_back((num_levels == 1) ? 0.f : low + (high - low) * t);
    }

    const std::vector<F32> level_only(1, 0.f);

    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        if (!mGroupEnable[i]->get())
            continue;

        S32 steps = llmax((S32)1, (S32)mGroupCount[i]->get());
        LLVector3 target = metrics.mAxis;

        switch (sGroups[i].mGroup)
        {
        case SHOT_FULLBODY:
            target.mV[VZ] = (metrics.mSoleZ + metrics.mHeadTopZ) * 0.5f;
            addOrbit(SHOT_FULLBODY, steps, target, FRAME_FULLBODY * height, fov_body, body_levels, metrics);
            break;

        case SHOT_UPPERBODY:
            // Frame reaches from roughly the waist to above the head.
            target.mV[VZ] = metrics.mHeadTopZ - FRAME_UPPERBODY * height * 0.5f;
            addOrbit(SHOT_UPPERBODY, steps, target, FRAME_UPPERBODY * height, fov_body, level_only, metrics);
            break;

        case SHOT_MIDBODY:
            target.mV[VZ] = (metrics.mPelvis.mV[VZ] + metrics.mChest.mV[VZ]) * 0.5f;
            addOrbit(SHOT_MIDBODY, steps, target, FRAME_MIDBODY * height, fov_body, level_only, metrics);
            break;

        case SHOT_LEGS:
            target.mV[VZ] = (metrics.mHipMid.mV[VZ] + metrics.mSoleZ) * 0.5f;
            addOrbit(SHOT_LEGS, steps, target, FRAME_LEGS * height, fov_body, level_only, metrics);
            break;

        case SHOT_HEAD:
            addOrbit(SHOT_HEAD, steps, metrics.mHeadCenter, FRAME_HEAD * metrics.mScale, fov_detail, level_only, metrics);
            break;

        case SHOT_HANDS:
        {
            // Aim between elbow and wrist so the forearm stays in frame.
            LLVector3 targets[2];
            for (S32 side = 0; side < 2; ++side)
            {
                targets[side] = metrics.mWrist[side] + (metrics.mElbow[side] - metrics.mWrist[side]) * 0.25f;
            }
            addDetailShots(SHOT_HANDS, steps, targets, FRAME_HANDS * metrics.mScale, fov_detail, HANDS_ELEVATION, metrics);
            break;
        }

        case SHOT_FEET:
        {
            LLVector3 targets[2];
            for (S32 side = 0; side < 2; ++side)
            {
                targets[side] = metrics.mAnkle[side];
                targets[side].mV[VZ] -= ANKLE_TO_SOLE * metrics.mScale;
            }
            addDetailShots(SHOT_FEET, steps, targets, FRAME_FEET * metrics.mScale, fov_detail, FEET_ELEVATION, metrics);
            break;
        }

        default:
            break;
        }
    }

    return !mShots.empty();
}

//---------------------------------------------------------------------------
// Framing preview
//---------------------------------------------------------------------------

std::string LLFloaterAvatarLora::shotLabel(const LoraShot& shot) const
{
    std::string label = getString(std::string("group_") + sGroups[shot.mGroup].mCtrlSuffix);

    if (shot.mSide >= 0)
    {
        label += " " + getString(shot.mSide == 0 ? "side_left" : "side_right");
    }

    return label;
}

void LLFloaterAvatarLora::updatePreviewLabel()
{
    if (mShots.empty() || mPreviewIndex < 0 || mPreviewIndex >= (S32)mShots.size())
    {
        mPreviewLabel->setText(LLStringUtil::null);
        return;
    }

    const LoraShot& shot = mShots[mPreviewIndex];

    LLStringUtil::format_map_t args;
    args["INDEX"] = llformat("%d", mPreviewIndex + 1);
    args["TOTAL"] = llformat("%d", (S32)mShots.size());
    args["SHOT"] = shotLabel(shot);
    args["AZ"] = llformat("%.0f", shot.mAzimuthDeg);
    args["EL"] = llformat("%.0f", shot.mElevationDeg);
    args["DIST"] = llformat("%.2f", shot.mDistance);

    mPreviewLabel->setText(getString("preview_info", args));
}

void LLFloaterAvatarLora::previewShot(S32 index)
{
    if (mShots.empty())
        return;

    mPreviewIndex = llclamp(index, 0, (S32)mShots.size() - 1);
    const LoraShot& shot = mShots[mPreviewIndex];

    if (!mPreviewActive)
    {
        mSavedCameraGlobal = gAgentCamera.getCameraPositionGlobal();
        mSavedFocusGlobal = gAgentCamera.getFocusGlobal();
        mSavedDefaultFov = LLViewerCamera::getInstance()->getDefaultFOV();

        // Low shots put the camera below foot level, where the usual clamp to
        // half a meter above the land would silently raise it and show a
        // different framing than the capture (which drives the camera directly).
        mSavedCameraConstraints = gSavedSettings.getBOOL("DisableCameraConstraints");
        gSavedSettings.setBOOL("DisableCameraConstraints", true);

        mPreviewActive = true;
    }

    gAgentCamera.setFocusOnAvatar(false, false);

    // setFocusGlobal clears a leftover FOV zoom factor from earlier object
    // focusing; updateCamera would otherwise scale our camera distance by it.
    gAgentCamera.setFocusGlobal(shot.mTargetGlobal, LLUUID::null);
    gAgentCamera.setCameraPosAndFocusGlobal(shot.mCameraGlobal, shot.mTargetGlobal, LLUUID::null);
    gAgentCamera.stopCameraAnimation();
    LLViewerCamera::getInstance()->setDefaultFOV(shot.mFovRad);

    updatePreviewLabel();
}

void LLFloaterAvatarLora::stopPreview()
{
    if (!mPreviewActive)
        return;

    LLViewerCamera::getInstance()->setDefaultFOV(mSavedDefaultFov);
    gAgentCamera.setCameraPosAndFocusGlobal(mSavedCameraGlobal, mSavedFocusGlobal, LLUUID::null);
    gSavedSettings.setBOOL("DisableCameraConstraints", mSavedCameraConstraints);

    mPreviewActive = false;
}

void LLFloaterAvatarLora::onPreviewStep(S32 delta)
{
    // Rebuilt on every step so slider changes and avatar movement show up
    // immediately - the plan is only frozen once a capture starts.
    S32 previous_index = mPreviewIndex;
    if (!buildShotList())
    {
        mStatusText->setText(getString("status_measure_failed"));
        mPreviewLabel->setText(LLStringUtil::null);
        return;
    }

    S32 total = (S32)mShots.size();
    S32 index = mPreviewActive ? previous_index + delta : 0;
    if (index < 0)
    {
        index = total - 1;
    }
    else if (index >= total)
    {
        index = 0;
    }

    previewShot(index);
    mStatusText->setText(getString("status_preview"));
}

//---------------------------------------------------------------------------
// Settings
//---------------------------------------------------------------------------

static std::string capitalized(const char* text)
{
    std::string result(text);
    if (!result.empty())
    {
        result[0] = (char)std::toupper((unsigned char)result[0]);
    }
    return result;
}

std::string LLFloaterAvatarLora::enableSettingName(S32 group_index) const
{
    return "FSLoraCaptureEnable" + capitalized(sGroups[group_index].mCtrlSuffix);
}

std::string LLFloaterAvatarLora::countSettingName(S32 group_index) const
{
    return "FSLoraCaptureCount" + capitalized(sGroups[group_index].mCtrlSuffix);
}

void LLFloaterAvatarLora::loadSettings()
{
    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        mGroupEnable[i]->set(gSavedSettings.getBOOL(enableSettingName(i)));
        mGroupCount[i]->set((F32)gSavedSettings.getS32(countSettingName(i)));
    }

    mLevelsSpinner->set((F32)gSavedSettings.getS32("FSLoraCaptureLevels"));
    mFovBodySpinner->set(gSavedSettings.getF32("FSLoraCaptureFovBody"));
    mFovDetailSpinner->set(gSavedSettings.getF32("FSLoraCaptureFovDetail"));
    mMarginSpinner->set(gSavedSettings.getF32("FSLoraCaptureMargin"));
    mElevationHighSpinner->set(gSavedSettings.getF32("FSLoraCaptureElevationHigh"));
    mElevationLowSpinner->set(gSavedSettings.getF32("FSLoraCaptureElevationLow"));
    mDelaySpinner->set(gSavedSettings.getF32("FSLoraCaptureDelay"));

    S32 resolution_index = llclamp(gSavedSettings.getS32("FSLoraCaptureResolution"), 0, sNumPresets - 1);
    mResolutionCombo->setCurrentByIndex(resolution_index);
    mCustomWidthSpinner->set((F32)gSavedSettings.getS32("FSLoraCaptureCustomWidth"));
    mCustomHeightSpinner->set((F32)gSavedSettings.getS32("FSLoraCaptureCustomHeight"));

    mOutputDirEditor->setValue(gSavedSettings.getString("FSLoraCaptureOutputDir"));
    mFolderStructureCombo->setCurrentByIndex(llclamp(gSavedSettings.getS32("FSLoraCaptureFolderStructure"), 0, 1));
    mRepeatsSpinner->set((F32)gSavedSettings.getS32("FSLoraCaptureKohyaRepeats"));
    mTriggerEditor->setValue(gSavedSettings.getString("FSLoraCaptureTrigger"));
    mClassEditor->setValue(gSavedSettings.getString("FSLoraCaptureClassToken"));
    mExtraTagsEditor->setValue(gSavedSettings.getString("FSLoraCaptureExtraTags"));
    mWriteCaptionsCheck->set(gSavedSettings.getBOOL("FSLoraCaptureWriteCaptions"));
}

void LLFloaterAvatarLora::saveSettings()
{
    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        gSavedSettings.setBOOL(enableSettingName(i), mGroupEnable[i]->get());
        gSavedSettings.setS32(countSettingName(i), (S32)mGroupCount[i]->get());
    }

    gSavedSettings.setS32("FSLoraCaptureLevels", (S32)mLevelsSpinner->get());
    gSavedSettings.setF32("FSLoraCaptureFovBody", (F32)mFovBodySpinner->get());
    gSavedSettings.setF32("FSLoraCaptureFovDetail", (F32)mFovDetailSpinner->get());
    gSavedSettings.setF32("FSLoraCaptureMargin", (F32)mMarginSpinner->get());
    gSavedSettings.setF32("FSLoraCaptureElevationHigh", (F32)mElevationHighSpinner->get());
    gSavedSettings.setF32("FSLoraCaptureElevationLow", (F32)mElevationLowSpinner->get());
    gSavedSettings.setF32("FSLoraCaptureDelay", (F32)mDelaySpinner->get());

    gSavedSettings.setS32("FSLoraCaptureResolution", mResolutionCombo->getCurrentIndex());
    gSavedSettings.setS32("FSLoraCaptureCustomWidth", (S32)mCustomWidthSpinner->get());
    gSavedSettings.setS32("FSLoraCaptureCustomHeight", (S32)mCustomHeightSpinner->get());

    gSavedSettings.setString("FSLoraCaptureOutputDir", mOutputDirEditor->getValue().asString());
    gSavedSettings.setS32("FSLoraCaptureFolderStructure", mFolderStructureCombo->getCurrentIndex());
    gSavedSettings.setS32("FSLoraCaptureKohyaRepeats", (S32)mRepeatsSpinner->get());
    gSavedSettings.setString("FSLoraCaptureTrigger", mTriggerEditor->getValue().asString());
    gSavedSettings.setString("FSLoraCaptureClassToken", mClassEditor->getValue().asString());
    gSavedSettings.setString("FSLoraCaptureExtraTags", mExtraTagsEditor->getValue().asString());
    gSavedSettings.setBOOL("FSLoraCaptureWriteCaptions", mWriteCaptionsCheck->get());
}

//---------------------------------------------------------------------------
// Capture run
//---------------------------------------------------------------------------

void LLFloaterAvatarLora::onStartBtn()
{
    if (mCapturing)
        return;

    // Freeze the plan once, here: everything from now on works off this list.
    if (!buildShotList())
    {
        mStatusText->setText(getString("status_measure_failed"));
        return;
    }

    mOutputDir = mOutputDirEditor->getValue().asString();
    if (!prepareOutputDirs())
    {
        mStatusText->setText(getString("status_dir_failed"));
        return;
    }

    saveSettings();
    startCapture();
}

void LLFloaterAvatarLora::onStopBtn()
{
    if (!mCapturing)
        return;

    // A partial run is still a usable dataset, so describe what was written.
    if (mSavedImages > 0)
    {
        writeManifest();
    }

    stopCapture(true);
    mStatusText->setText(getString("status_aborted"));
}

void LLFloaterAvatarLora::startCapture()
{
    // The preview owns the camera through gAgentCamera; the capture drives
    // LLViewerCamera directly, so hand it back first.
    stopPreview();

    mImageWidth = getCurrentWidth();
    mImageHeight = getCurrentHeight();
    if (mImageWidth < 16 || mImageHeight < 16)
    {
        mStatusText->setText(getString("status_dir_failed"));
        return;
    }

    mRawImage = new LLImageRaw(mImageWidth, mImageHeight, 3);
    mCurrentShot = 0;
    mSavedImages = 0;
    mManifestShots = LLSD::emptyArray();
    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        mGroupCounter[i] = 0;
    }

    LLAvatarName av_name;
    if (LLAvatarNameCache::get(mSelectedAvatarId, &av_name))
    {
        mCaptureAvatarName = av_name.getUserName();
    }
    else if (LLVOAvatar* avatar = getSelectedAvatar())
    {
        mCaptureAvatarName = avatar->getFullname();
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mCaptureCamOrigin = camera->getOrigin();
    mCaptureCamLookAt = mCaptureCamOrigin + camera->getAtAxis() * 10.f;
    mCaptureCamUp = camera->getUpAxis();
    mCaptureCamFov = camera->getView();
    mCaptureCamAspect = camera->getAspect();
    mCaptureCamNear = camera->getNear();

    mCapturing = true;

    F32 delay = llmax(0.f, (F32)mDelaySpinner->get());
    mCountdownActive = (delay > 0.f);
    if (mCountdownActive)
    {
        mCountdownTimer.resetWithExpiry(delay);
    }

    mProgressBar->setValue(0.0);
    updateUIState();

    gIdleCallbacks.addFunction(onIdle, this);
}

void LLFloaterAvatarLora::stopCapture(bool restore_camera)
{
    if (!mCapturing)
        return;

    gIdleCallbacks.deleteFunction(onIdle, this);

    mCapturing = false;
    mCountdownActive = false;

    freezeWorld(false);
    setCaptureRenderState(false);

    if (restore_camera)
    {
        restoreCaptureCamera();
    }

    mRawImage = nullptr;

    updateUIState();
}

void LLFloaterAvatarLora::finishCapture()
{
    S32 saved = mSavedImages;
    std::string directory = mOutputDir;

    if (saved > 0)
    {
        writeManifest();
    }

    stopCapture(true);

    LLStringUtil::format_map_t args;
    args["COUNT"] = llformat("%d", saved);
    args["DIR"] = directory;
    mStatusText->setText(getString("status_done", args));
    mProgressBar->setValue(100.0);
}

// static
void LLFloaterAvatarLora::onIdle(void* user_data)
{
    LLFloaterAvatarLora* self = (LLFloaterAvatarLora*)user_data;
    if (!self || !self->mCapturing)
        return;

    if (self->mCountdownActive)
    {
        if (!self->mCountdownTimer.hasExpired())
        {
            LLStringUtil::format_map_t args;
            args["SECONDS"] = llformat("%.0f", llmax(0.f, self->mCountdownTimer.getTimeToExpireF32()));
            self->mStatusText->setText(self->getString("status_countdown", args));
            return;
        }

        self->mCountdownActive = false;
        self->setCaptureRenderState(true);
        self->freezeWorld(true);
    }

    self->captureNextShot();
}

void LLFloaterAvatarLora::captureNextShot()
{
    if (mCurrentShot >= (S32)mShots.size())
    {
        finishCapture();
        return;
    }

    const LoraShot& shot = mShots[mCurrentShot];
    S32 index_in_group = mGroupCounter[shot.mGroup]++;
    std::string file_name = shotFileName(shot, index_in_group);

    applyCaptureCamera(shot);

    if (gViewerWindow->simpleSnapshot(mRawImage, mImageWidth, mImageHeight, CAPTURE_RENDER_PASSES))
    {
        if (saveShotImage(file_name))
        {
            std::string caption = buildCaption(shot);
            if (mWriteCaptionsCheck->get())
            {
                writeCaption(file_name, caption);
            }

            recordManifestEntry(shot, file_name, caption);
            mSavedImages++;
        }
    }
    else
    {
        LL_WARNS("AvatarLora") << "Snapshot failed for shot " << mCurrentShot << LL_ENDL;
    }

    mCurrentShot++;
    // LLProgressBar works in percent, not in a 0..1 fraction.
    mProgressBar->setValue(100.f * (F32)mCurrentShot / (F32)mShots.size());

    LLStringUtil::format_map_t args;
    args["INDEX"] = llformat("%d", mCurrentShot);
    args["TOTAL"] = llformat("%d", (S32)mShots.size());
    args["SHOT"] = shotLabel(shot);
    mStatusText->setText(getString("status_capturing", args));
}

void LLFloaterAvatarLora::applyCaptureCamera(const LoraShot& shot)
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();

    // Aspect first: setView clamps the vertical FOV against the aspect ratio,
    // and simpleSnapshot does not set it for us the way cubeSnapshot does.
    camera->setAspect((F32)mImageWidth / (F32)mImageHeight);
    camera->setViewNoBroadcast(shot.mFovRad);
    camera->setNear(CAPTURE_NEAR_CLIP);

    LLVector3 camera_pos = gAgent.getPosAgentFromGlobal(shot.mCameraGlobal);
    LLVector3 target_pos = gAgent.getPosAgentFromGlobal(shot.mTargetGlobal);
    camera->setOriginAndLookAt(camera_pos, LLVector3::z_axis, target_pos);
}

void LLFloaterAvatarLora::restoreCaptureCamera()
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    camera->setOriginAndLookAt(mCaptureCamOrigin, mCaptureCamUp, mCaptureCamLookAt);
    camera->setViewNoBroadcast(mCaptureCamFov);
    camera->setAspect(mCaptureCamAspect);
    camera->setNear(mCaptureCamNear);

    // Keep gAgentCamera in sync, or it puts its own camera back next frame.
    LLVector3d camera_global = gAgent.getPosGlobalFromAgent(mCaptureCamOrigin);
    LLVector3d focus_global = gAgent.getPosGlobalFromAgent(mCaptureCamLookAt);
    gAgentCamera.setCameraPosAndFocusGlobal(camera_global, focus_global, LLUUID::null);
}

void LLFloaterAvatarLora::setCaptureRenderState(bool enable)
{
    if (enable)
    {
        if (mRenderStateApplied)
            return;

        mSavedDepthOfField = gSavedSettings.getBOOL("RenderDepthOfField");
        mSavedVolumeLOD = gSavedSettings.getF32("RenderVolumeLODFactor");
        mSavedAvatarLOD = gSavedSettings.getF32("RenderAvatarLODFactor");
        mSavedMaxNonImpostors = gSavedSettings.getU32("RenderAvatarMaxNonImpostors");
        mSavedNameTagMode = gSavedSettings.getS32("AvatarNameTagMode");

        // Depth of field would blur parts of the subject, and name tags are
        // rendered in world - both end up baked into the training data.
        gSavedSettings.setBOOL("RenderDepthOfField", false);
        gSavedSettings.setS32("AvatarNameTagMode", 0);

        gSavedSettings.setF32("RenderVolumeLODFactor", llmax(mSavedVolumeLOD, CAPTURE_VOLUME_LOD));
        gSavedSettings.setF32("RenderAvatarLODFactor", llmax(mSavedAvatarLOD, CAPTURE_AVATAR_LOD));
        gSavedSettings.setU32("RenderAvatarMaxNonImpostors", llmax(mSavedMaxNonImpostors, CAPTURE_MAX_NON_IMPOSTORS));

        mRenderStateApplied = true;
    }
    else
    {
        if (!mRenderStateApplied)
            return;

        gSavedSettings.setBOOL("RenderDepthOfField", mSavedDepthOfField);
        gSavedSettings.setS32("AvatarNameTagMode", mSavedNameTagMode);
        gSavedSettings.setF32("RenderVolumeLODFactor", mSavedVolumeLOD);
        gSavedSettings.setF32("RenderAvatarLODFactor", mSavedAvatarLOD);
        gSavedSettings.setU32("RenderAvatarMaxNonImpostors", mSavedMaxNonImpostors);

        mRenderStateApplied = false;
    }
}

void LLFloaterAvatarLora::freezeWorld(bool enable)
{
    static bool clouds_were_paused = false;

    if (enable)
    {
        clouds_were_paused = LLEnvironment::instance().isCloudScrollPaused();
        LLEnvironment::instance().pauseCloudScroll();

        for (LLCharacter* character : LLCharacter::sInstances)
        {
            mAvatarPauseHandles.push_back(character->requestPause());
        }

        gSavedSettings.setBOOL("FreezeTime", true);
        LLViewerPartSim::getInstance()->enable(false);
    }
    else
    {
        // A pause handle per character is taken above, so an empty list means
        // the world was never frozen - don't resume things we never paused.
        if (mAvatarPauseHandles.empty())
            return;

        if (!clouds_were_paused)
        {
            LLEnvironment::instance().resumeCloudScroll();
        }

        mAvatarPauseHandles.clear();

        gSavedSettings.setBOOL("FreezeTime", false);
        LLViewerPartSim::getInstance()->enable(true);
    }
}

// static
std::string LLFloaterAvatarLora::sanitizeForPath(const std::string& text)
{
    static const std::string forbidden = "\\/:*?\"<>|";

    std::string result;
    result.reserve(text.size());

    for (char c : text)
    {
        result += (forbidden.find(c) == std::string::npos) ? c : '_';
    }

    LLStringUtil::trim(result);
    return result;
}

bool LLFloaterAvatarLora::prepareOutputDirs()
{
    const std::string delimiter = gDirUtilp->getDirDelimiter();

    if (mOutputDir.empty())
    {
        mOutputDir = gDirUtilp->getLindenUserDir() + delimiter + "avatar_lora";
    }

    if (!gDirUtilp->fileExists(mOutputDir))
    {
        LLFile::mkdir(mOutputDir);
    }

    if (!gDirUtilp->fileExists(mOutputDir))
    {
        return false;
    }

    mImageDir = mOutputDir;
    mImageRelDir.clear();

    // Kohya expects the repeat count and the concept_name in the folder name, as in
    // "img/20_ohwx_avatar person". The manifest stays in the root folder.
    if (mFolderStructureCombo->getCurrentIndex() == 1)
    {
        std::string image_root = mOutputDir + delimiter + "img";
        if (!gDirUtilp->fileExists(image_root))
        {
            LLFile::mkdir(image_root);
        }

        std::string concept_name = sanitizeForPath(mTriggerEditor->getValue().asString());
        if (concept_name.empty())
        {
            concept_name = "avatar";
        }

        std::string class_token = sanitizeForPath(mClassEditor->getValue().asString());
        if (!class_token.empty())
        {
            concept_name += " " + class_token;
        }

        S32 repeats = llmax((S32)1, (S32)mRepeatsSpinner->get());
        mImageRelDir = llformat("img/%d_%s", repeats, concept_name.c_str());
        mImageDir = image_root + delimiter + llformat("%d_%s", repeats, concept_name.c_str());

        if (!gDirUtilp->fileExists(mImageDir))
        {
            LLFile::mkdir(mImageDir);
        }
    }

    return gDirUtilp->fileExists(mImageDir);
}

//---------------------------------------------------------------------------
// Captions and manifest
//
// Caption text is training data, not UI: it stays English regardless of the
// viewer language, so none of it comes from the XUI strings.
//---------------------------------------------------------------------------

// static
std::string LLFloaterAvatarLora::viewDirectionPhrase(F32 azimuth_deg)
{
    F32 azimuth = fmodf(azimuth_deg + 360.f, 360.f);
    if (azimuth > 180.f)
    {
        azimuth -= 360.f;
    }

    // Positive azimuth puts the camera on the avatar's left.
    const char* side = (azimuth >= 0.f) ? "left" : "right";
    F32 magnitude = fabsf(azimuth);

    if (magnitude < 22.5f)
    {
        return "front view";
    }
    if (magnitude < 67.5f)
    {
        return llformat("three-quarter front view from the %s", side);
    }
    if (magnitude < 112.5f)
    {
        return llformat("%s side view", side);
    }
    if (magnitude < 157.5f)
    {
        return llformat("three-quarter back view from the %s", side);
    }

    return "back view";
}

// static
std::string LLFloaterAvatarLora::elevationPhrase(F32 elevation_deg)
{
    if (elevation_deg > 25.f)
    {
        return "high angle, shot from above";
    }
    if (elevation_deg > 5.f)
    {
        return "shot from slightly above";
    }
    if (elevation_deg < -25.f)
    {
        return "low angle, shot from below";
    }
    if (elevation_deg < -5.f)
    {
        return "shot from slightly below";
    }

    return LLStringUtil::null;
}

std::string LLFloaterAvatarLora::buildCaption(const LoraShot& shot) const
{
    const char* side_word = (shot.mSide == 0) ? "left" : "right";

    std::string subject;
    switch (shot.mGroup)
    {
    case SHOT_FULLBODY:  subject = "full body shot"; break;
    case SHOT_UPPERBODY: subject = "upper body shot"; break;
    case SHOT_MIDBODY:   subject = "torso and hips"; break;
    case SHOT_LEGS:      subject = "legs"; break;
    case SHOT_HEAD:      subject = "headshot, close-up of the face"; break;
    case SHOT_HANDS:     subject = llformat("close-up of the %s hand and forearm", side_word); break;
    case SHOT_FEET:      subject = llformat("close-up of the %s foot and lower leg", side_word); break;
    default:             subject = "photo"; break;
    }

    std::vector<std::string> parts;

    std::string trigger = mTriggerEditor->getValue().asString();
    LLStringUtil::trim(trigger);
    if (!trigger.empty())
    {
        parts.push_back(trigger);
    }

    std::string class_token = mClassEditor->getValue().asString();
    LLStringUtil::trim(class_token);
    if (!class_token.empty())
    {
        parts.push_back(class_token);
    }

    parts.push_back(subject);
    parts.push_back(viewDirectionPhrase(shot.mAzimuthDeg));

    std::string elevation = elevationPhrase(shot.mElevationDeg);
    if (!elevation.empty())
    {
        parts.push_back(elevation);
    }

    std::string extra = mExtraTagsEditor->getValue().asString();
    LLStringUtil::trim(extra);
    if (!extra.empty())
    {
        parts.push_back(extra);
    }

    std::string caption;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            caption += ", ";
        }
        caption += parts[i];
    }

    return caption;
}

bool LLFloaterAvatarLora::writeCaption(const std::string& image_file, const std::string& caption) const
{
    std::string base = image_file;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos)
    {
        base.erase(dot);
    }

    std::string full_path = mImageDir + gDirUtilp->getDirDelimiter() + base + ".txt";

    llofstream stream(full_path);
    if (!stream.is_open())
    {
        LL_WARNS("AvatarLora") << "Failed to write caption " << full_path << LL_ENDL;
        return false;
    }

    stream << caption << "\n";
    return true;
}

void LLFloaterAvatarLora::recordManifestEntry(const LoraShot& shot, const std::string& file_name, const std::string& caption)
{
    LLSD entry;
    entry["file"] = mImageRelDir.empty() ? file_name : (mImageRelDir + "/" + file_name);
    entry["group"] = sGroups[shot.mGroup].mCtrlSuffix;
    if (shot.mSide >= 0)
    {
        entry["side"] = (shot.mSide == 0) ? "left" : "right";
    }
    entry["azimuth_deg"] = (F64)shot.mAzimuthDeg;
    entry["elevation_deg"] = (F64)shot.mElevationDeg;
    entry["distance_m"] = (F64)shot.mDistance;
    entry["fov_deg"] = (F64)(shot.mFovRad * RAD_TO_DEG);
    entry["caption"] = caption;

    mManifestShots.append(entry);
}

void LLFloaterAvatarLora::writeManifest()
{
    LLSD manifest;
    manifest["created"] = LLDate::now().asString();
    manifest["viewer"] = LLVersionInfo::instance().getChannelAndVersionFS();

    manifest["avatar"]["name"] = mCaptureAvatarName;
    manifest["avatar"]["id"] = mSelectedAvatarId;
    manifest["avatar"]["height_m"] = (F64)mCaptureMetrics.mHeight;

    manifest["image"]["width"] = mImageWidth;
    manifest["image"]["height"] = mImageHeight;

    manifest["framing"]["fov_body_deg"] = (F64)mFovBodySpinner->get();
    manifest["framing"]["fov_detail_deg"] = (F64)mFovDetailSpinner->get();
    manifest["framing"]["margin_percent"] = (F64)mMarginSpinner->get();

    manifest["caption"]["trigger"] = mTriggerEditor->getValue().asString();
    manifest["caption"]["class"] = mClassEditor->getValue().asString();
    manifest["caption"]["extra"] = mExtraTagsEditor->getValue().asString();

    manifest["image_count"] = (S32)mManifestShots.size();
    manifest["shots"] = mManifestShots;

    std::string full_path = mOutputDir + gDirUtilp->getDirDelimiter() + "capture_manifest.json";

    llofstream stream(full_path);
    if (!stream.is_open())
    {
        LL_WARNS("AvatarLora") << "Failed to write manifest " << full_path << LL_ENDL;
        return;
    }

    stream << boost::json::serialize(LlsdToJson(manifest)) << "\n";
}

std::string LLFloaterAvatarLora::shotFileName(const LoraShot& shot, S32 index_in_group) const
{
    std::string side;
    if (shot.mSide == 0)
    {
        side = "_l";
    }
    else if (shot.mSide == 1)
    {
        side = "_r";
    }

    F32 azimuth = fmodf(shot.mAzimuthDeg + 360.f, 360.f);

    return llformat("lora_%s%s_%03d_az%03.0f_el%+03.0f.png",
                    sGroups[shot.mGroup].mCtrlSuffix, side.c_str(),
                    index_in_group, azimuth, shot.mElevationDeg);
}

bool LLFloaterAvatarLora::saveShotImage(const std::string& file_name)
{
    LLPointer<LLImagePNG> png_image = new LLImagePNG;
    if (!png_image->encode(mRawImage, 0))
    {
        LL_WARNS("AvatarLora") << "Failed to encode PNG for shot " << mCurrentShot << LL_ENDL;
        return false;
    }

    std::string full_path = mImageDir + gDirUtilp->getDirDelimiter() + file_name;
    if (!png_image->save(full_path))
    {
        LL_WARNS("AvatarLora") << "Failed to save " << full_path << LL_ENDL;
        return false;
    }

    return true;
}

//---------------------------------------------------------------------------
// UI
//---------------------------------------------------------------------------

S32 LLFloaterAvatarLora::getCurrentWidth() const
{
    S32 index = mResolutionCombo->getCurrentIndex();
    if (index < 0 || index >= sNumPresets)
        return sResolutionPresets[0].mWidth;

    if (index == sNumPresets - 1)
        return (S32)mCustomWidthSpinner->get();

    return sResolutionPresets[index].mWidth;
}

S32 LLFloaterAvatarLora::getCurrentHeight() const
{
    S32 index = mResolutionCombo->getCurrentIndex();
    if (index < 0 || index >= sNumPresets)
        return sResolutionPresets[0].mHeight;

    if (index == sNumPresets - 1)
        return (S32)mCustomHeightSpinner->get();

    return sResolutionPresets[index].mHeight;
}

void LLFloaterAvatarLora::onResolutionPreset()
{
    bool is_custom = (mResolutionCombo->getCurrentIndex() == sNumPresets - 1);
    mCustomWidthSpinner->setVisible(is_custom);
    mCustomHeightSpinner->setVisible(is_custom);

    updateUIState();
}

void LLFloaterAvatarLora::onBrowseBtn()
{
    std::string proposed_name = mOutputDirEditor->getValue().asString();
    if (proposed_name.empty())
    {
        proposed_name = gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter() + "avatar_lora";
    }

    (new LLDirPickerThread(boost::bind(&LLFloaterAvatarLora::onBrowseDirSelected, this, _1, _2), proposed_name))->getFile();
}

void LLFloaterAvatarLora::onBrowseDirSelected(const std::vector<std::string>& filenames, std::string proposed_name)
{
    if (filenames.empty())
        return;

    mOutputDirEditor->setValue(filenames[0]);
    updateUIState();
}

S32 LLFloaterAvatarLora::countShots() const
{
    S32 levels = llmax((S32)1, (S32)mLevelsSpinner->get());
    S32 total = 0;

    for (S32 i = 0; i < SHOT_GROUP_COUNT; ++i)
    {
        if (!mGroupEnable[i]->get())
            continue;

        S32 count = llmax((S32)0, (S32)mGroupCount[i]->get());
        if (sGroups[i].mPerSide)
        {
            count *= 2;
        }
        if (sGroups[i].mUseLevels)
        {
            count *= levels;
        }

        total += count;
    }

    return total;
}

void LLFloaterAvatarLora::updateUIState()
{
    S32 total = countShots();
    S32 width = getCurrentWidth();
    S32 height = getCurrentHeight();

    LLStringUtil::format_map_t args;
    args["COUNT"] = llformat("%d", total);
    mTotalImagesText->setText(getString("total_images", args));

    // Rough PNG estimate: renders against a plain background compress well, so
    // roughly half of the raw RGB size.
    S64 estimated_bytes = (S64)width * (S64)height * 3 * (S64)total / 2;
    std::string size_str;
    if (estimated_bytes > 1073741824)
    {
        size_str = llformat("~%.1f GB", estimated_bytes / 1073741824.0);
    }
    else if (estimated_bytes > 1048576)
    {
        size_str = llformat("~%.0f MB", estimated_bytes / 1048576.0);
    }
    else
    {
        size_str = llformat("~%.0f KB", estimated_bytes / 1024.0);
    }
    mEstimatedSizeText->setText(size_str);

    bool kohya_layout = (mFolderStructureCombo->getCurrentIndex() == 1);
    mRepeatsSpinner->setEnabled(kohya_layout);

    bool has_avatar = (getSelectedAvatar() != nullptr);
    bool has_output_dir = !mOutputDirEditor->getValue().asString().empty();

    bool can_preview = (total > 0) && has_avatar && !mCapturing;
    mPreviewPrevBtn->setEnabled(can_preview);
    mPreviewNextBtn->setEnabled(can_preview);

    mStartBtn->setEnabled(!mCapturing && has_avatar && has_output_dir && total > 0);
    mStopBtn->setEnabled(mCapturing);
}
