/**
 * @file llfloateravatarlora.h
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

#ifndef LL_FLOATER_AVATAR_LORA_H
#define LL_FLOATER_AVATAR_LORA_H

#include "llcharacter.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llimage.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"

#include <string>
#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLProgressBar;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLVOAvatar;

class LLFloaterAvatarLora : public LLFloater
{
    friend class LLFloaterReg;

public:
    LOG_CLASS(LLFloaterAvatarLora);

    LLFloaterAvatarLora(const LLSD& key);
    ~LLFloaterAvatarLora();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

    static void onIdle(void* user_data);

    // Shot groups, in capture order. Everything up to SHOT_GROUP_COUNT has its
    // own checkbox and count in the shot set; SHOT_FACE is a separate series
    // with its own button and a fixed plan.
    enum EShotGroup
    {
        SHOT_FULLBODY = 0,
        SHOT_UPPERBODY,
        SHOT_MIDBODY,
        SHOT_LEGS,
        SHOT_HEAD,
        SHOT_HANDS,
        SHOT_FEET,
        SHOT_GROUP_COUNT,
        SHOT_FACE = SHOT_GROUP_COUNT,
        SHOT_TYPE_COUNT
    };

    enum ECaptureMode
    {
        CAPTURE_BODY = 0,
        CAPTURE_FACE
    };

    // One planned photo. The whole list is built up front from a single
    // skeleton measurement so framing cannot drift while the series runs.
    struct LoraShot
    {
        EShotGroup  mGroup;
        S32         mSide;          // -1 = not sided, 0 = left, 1 = right
        F32         mAzimuthDeg;    // 0 = in front of the avatar's face
        F32         mElevationDeg;  // positive = camera above the target
        LLVector3d  mTargetGlobal;  // point the camera looks at
        LLVector3d  mCameraGlobal;  // resolved camera position, fixed at build time
        F32         mDistance;      // meters from target to camera
        F32         mFovRad;        // vertical field of view
    };

private:
    // Skeleton measurement the shot plan is derived from. All positions are in
    // agent coordinates, taken at one instant.
    struct AvatarMetrics
    {
        bool        mValid;
        F32         mHeight;        // sole to top of head, as currently posed
        F32         mScale;         // mHeight relative to a 1.8 m reference avatar
        F32         mSoleZ;
        F32         mHeadTopZ;
        LLVector3   mForward;       // horizontal facing direction, normalized
        LLVector3   mAxis;          // body axis (pelvis x/y)
        LLVector3   mHeadCenter;    // eye level - the face
        LLVector3   mSkullCenter;   // middle of the skull - what the head orbit turns around
        LLVector3   mChest;
        LLVector3   mPelvis;
        LLVector3   mHipMid;
        LLVector3   mWrist[2];      // 0 = left, 1 = right
        LLVector3   mElbow[2];
        LLVector3   mAnkle[2];
        LLVector3   mHandCenter[2]; // middle of the hand, not the wrist
        LLVector3   mFootCenter[2]; // middle of the foot, not the ankle

        AvatarMetrics() : mValid(false), mHeight(0.f), mScale(1.f), mSoleZ(0.f), mHeadTopZ(0.f) {}
    };

    // Static description of a shot group. Control names in the XUI are built
    // from mCtrlSuffix, so group table and XUI stay in sync by construction.
    struct GroupDesc
    {
        EShotGroup  mGroup;
        const char* mCtrlSuffix;    // e.g. "fullbody" -> "shot_fullbody" / "count_fullbody"
        S32         mDefaultCount;  // azimuth steps (per side for mPerSide groups)
        bool        mPerSide;       // count applies to left and right separately
        bool        mUseLevels;     // count is multiplied by the number of height levels
    };
    static const GroupDesc sGroups[SHOT_GROUP_COUNT];

    struct ResolutionPreset
    {
        const char* mLabel;
        S32         mWidth;
        S32         mHeight;
    };
    static const ResolutionPreset sResolutionPresets[];
    static const S32 sNumPresets;

    // Target avatar
    void        refreshAvatarList();
    void        onAvatarSelected();
    void        updateAvatarInfo();
    LLUUID      getSelectedAvatarId() const;
    LLVOAvatar* getSelectedAvatar() const;
    static LLVOAvatar* getAvatarByUuid(const LLUUID& avatar_id);
    static bool isEligibleAvatar(LLVOAvatar* avatar);

    // Settings persistence
    void        loadSettings();
    void        saveSettings();
    std::string enableSettingName(S32 group_index) const;
    std::string countSettingName(S32 group_index) const;

    // Measurement and shot plan
    bool        measureAvatar(LLVOAvatar* avatar, AvatarMetrics& metrics) const;
    bool        buildShotList();
    bool        buildFaceShotList();
    static const char* groupSuffix(EShotGroup group);
    void        addOrbit(EShotGroup group, S32 steps, const LLVector3& target,
                         F32 frame_height, F32 fov_rad, const std::vector<F32>& elevations,
                         const AvatarMetrics& metrics);
    void        addDetailShots(EShotGroup group, S32 steps_per_side, const LLVector3 targets[2],
                               F32 frame_height, F32 fov_rad, F32 elevation_deg,
                               const AvatarMetrics& metrics);
    F32         distanceForFrameHeight(F32 frame_height, F32 fov_rad) const;
    F32         currentAspect() const;
    LLVector3   cameraOffsetFor(const LLVector3& forward, F32 azimuth_deg, F32 elevation_deg, F32 distance) const;

    // Framing preview
    void        onPreviewStep(S32 delta);
    void        previewShot(S32 index);
    void        stopPreview();
    void        updatePreviewLabel();
    std::string shotLabel(const LoraShot& shot) const;

    // Capture run
    void        onStartBtn(ECaptureMode mode);
    void        onStopBtn();
    void        startCapture();
    void        stopCapture(bool restore_camera);
    void        finishCapture();
    void        captureNextShot();
    bool        saveShotImage(const std::string& file_name);
    void        applyCaptureCamera(const LoraShot& shot);
    void        restoreCaptureCamera();
    void        setCaptureRenderState(bool enable);
    void        freezeWorld(bool enable);
    bool        prepareOutputDirs();
    std::string shotFileName(const LoraShot& shot, S32 index_in_group) const;

    // Series variant: the first extra tag, folded into every file name so a
    // second series (other outfit, other expression) cannot overwrite the first.
    std::string buildVariantToken() const;
    static std::string sanitizeForFileToken(const std::string& text);

    // Dataset output: captions and manifest
    std::string buildCaption(const LoraShot& shot) const;
    static std::string viewDirectionPhrase(F32 azimuth_deg);
    static std::string elevationPhrase(F32 elevation_deg);
    static std::string sanitizeForPath(const std::string& text);
    bool        writeCaption(const std::string& image_file, const std::string& caption) const;
    void        recordManifestEntry(const LoraShot& shot, const std::string& file_name, const std::string& caption);
    void        writeManifest();

    // UI
    void        onResolutionPreset();
    void        onBrowseBtn();
    void        onBrowseDirSelected(const std::vector<std::string>& filenames, std::string proposed_name);
    void        updateUIState();
    S32         countShots() const;
    S32         getCurrentWidth() const;
    S32         getCurrentHeight() const;

    // Controls
    LLScrollListCtrl* mAvatarList;
    LLButton*         mRefreshBtn;
    LLTextBox*        mAvatarInfoText;

    LLCheckBoxCtrl*   mGroupEnable[SHOT_GROUP_COUNT];
    LLSpinCtrl*       mGroupCount[SHOT_GROUP_COUNT];
    LLSpinCtrl*       mLevelsSpinner;

    LLSpinCtrl*       mFovBodySpinner;
    LLSpinCtrl*       mFovDetailSpinner;
    LLSpinCtrl*       mMarginSpinner;
    LLSpinCtrl*       mElevationHighSpinner;
    LLSpinCtrl*       mElevationLowSpinner;
    LLSpinCtrl*       mDelaySpinner;

    LLComboBox*       mResolutionCombo;
    LLSpinCtrl*       mCustomWidthSpinner;
    LLSpinCtrl*       mCustomHeightSpinner;
    LLLineEditor*     mOutputDirEditor;
    LLButton*         mBrowseBtn;
    LLComboBox*       mFolderStructureCombo;
    LLSpinCtrl*       mRepeatsSpinner;
    LLLineEditor*     mTriggerEditor;
    LLLineEditor*     mClassEditor;
    LLLineEditor*     mExtraTagsEditor;
    LLCheckBoxCtrl*   mWriteCaptionsCheck;
    LLCheckBoxCtrl*   mTagsInFileNameCheck;

    LLTextBox*        mTotalImagesText;
    LLTextBox*        mEstimatedSizeText;
    LLButton*         mPreviewPrevBtn;
    LLButton*         mPreviewNextBtn;
    LLTextBox*        mPreviewLabel;
    LLButton*         mStartBtn;
    LLButton*         mStartFaceBtn;
    LLButton*         mStopBtn;
    LLProgressBar*    mProgressBar;
    LLTextBox*        mStatusText;

    // State
    LLUUID                mSelectedAvatarId;
    std::vector<LoraShot> mShots;
    S32                   mPreviewIndex;

    // Camera state saved when the framing preview takes over
    bool                  mPreviewActive;
    LLVector3d            mSavedCameraGlobal;
    LLVector3d            mSavedFocusGlobal;
    F32                   mSavedDefaultFov;
    bool                  mSavedCameraConstraints;

    // Capture run state
    bool                  mCapturing;
    bool                  mCountdownActive;
    LLFrameTimer          mCountdownTimer;
    S32                   mCurrentShot;
    S32                   mGroupCounter[SHOT_TYPE_COUNT];
    S32                   mImageWidth;
    S32                   mImageHeight;
    S32                   mSavedImages;
    std::string           mOutputDir;      // manifest lives here
    std::string           mImageDir;       // images and captions land here
    std::string           mImageRelDir;    // mImageDir relative to mOutputDir
    std::string           mVariantToken;   // frozen at run start, empty if unused
    LLSD                  mManifestShots;
    AvatarMetrics         mCaptureMetrics;
    std::string           mCaptureAvatarName;
    LLPointer<LLImageRaw> mRawImage;
    std::vector<LLAnimPauseRequest> mAvatarPauseHandles;

    // Camera state saved while the capture drives LLViewerCamera directly
    LLVector3             mCaptureCamOrigin;
    LLVector3             mCaptureCamLookAt;
    LLVector3             mCaptureCamUp;
    F32                   mCaptureCamFov;
    F32                   mCaptureCamAspect;
    F32                   mCaptureCamNear;
    LLVector3d            mCaptureFocusGlobal;

    // Render settings saved while the capture overrides them
    bool                  mRenderStateApplied;
    bool                  mSavedDepthOfField;
    F32                   mSavedVolumeLOD;
    F32                   mSavedAvatarLOD;
    U32                   mSavedMaxNonImpostors;
    S32                   mSavedNameTagMode;
};

#endif // LL_FLOATER_AVATAR_LORA_H
