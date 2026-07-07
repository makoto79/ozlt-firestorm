/**
 * @file llmcptoolsenvironment.cpp
 * @brief Environment MCP tools (get_environment/set_environment)
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

#include "llmcptools.h"

#include "llmcptoolregistry.h"

#include "llenvironment.h"
#include "llsettingssky.h"
#include "llsdutil.h"
#include "llvirtualtrackball.h"

#include <stdexcept>

namespace
{
    using ToolResultCallback = LLMCPToolRegistry::ToolResultCallback;

    LLSD colorToLLSD(const LLColor3& c)
    {
        LLSD result;
        result["r"] = c.mV[0];
        result["g"] = c.mV[1];
        result["b"] = c.mV[2];
        return result;
    }

    LLColor3 llsdToColor3(const LLSD& sd, const LLColor3& current)
    {
        LLColor3 c = current;
        if (sd.has("r")) c.mV[0] = (F32)sd["r"].asReal();
        if (sd.has("g")) c.mV[1] = (F32)sd["g"].asReal();
        if (sd.has("b")) c.mV[2] = (F32)sd["b"].asReal();
        return c;
    }

    // Same azimuth/elevation <-> quaternion convention as
    // LLFloaterEnvironmentAdjust (llfloaterenvironmentadjust.cpp,
    // onSunAzimElevChanged/onSunRotationChanged) - replicated here since
    // it isn't exposed as a reusable free function.
    LLQuaternion azimuthElevationToQuaternion(F32 azimuth_deg, F32 elevation_deg)
    {
        F32 azimuth = azimuth_deg * DEG_TO_RAD;
        F32 elevation = elevation_deg * DEG_TO_RAD;
        if (is_approx_zero(elevation))
        {
            elevation = F_APPROXIMATELY_ZERO;
        }

        LLQuaternion quat;
        quat.setAngleAxis(-elevation, 0, 1, 0);
        LLQuaternion az_quat;
        az_quat.setAngleAxis(F_TWO_PI - azimuth, 0, 0, 1);
        quat *= az_quat;
        return quat;
    }

    LLSD azimuthElevationToLLSD(const LLQuaternion& quat)
    {
        F32 azimuth, elevation;
        LLVirtualTrackball::getAzimuthAndElevationDeg(quat, azimuth, elevation);
        LLSD result;
        result["azimuth"] = azimuth;
        result["elevation"] = elevation;
        return result;
    }

    LLSD buildEnvironmentResult(const LLSettingsSky::ptr_t& sky)
    {
        LLSD result;
        result["sun"] = azimuthElevationToLLSD(sky->getSunRotation());
        result["moon"] = azimuthElevationToLLSD(sky->getMoonRotation());

        LLSD clouds;
        clouds["color"] = colorToLLSD(sky->getCloudColor());
        clouds["scale"] = sky->getCloudScale();
        clouds["shadow"] = sky->getCloudShadow();
        clouds["variance"] = sky->getCloudVariance();
        LLVector2 scroll = sky->getCloudScrollRate();
        clouds["scroll_x"] = scroll.mV[0];
        clouds["scroll_y"] = scroll.mV[1];
        result["clouds"] = clouds;

        LLSD haze;
        haze["density"] = sky->getHazeDensity();
        haze["horizon"] = sky->getHazeHorizon();
        haze["blue_density"] = colorToLLSD(sky->getBlueDensity());
        haze["blue_horizon"] = colorToLLSD(sky->getBlueHorizon());
        haze["distance_multiplier"] = sky->getDistanceMultiplier();
        result["haze"] = haze;

        return result;
    }

    void toolGetEnvironment(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        // ENV_CURRENT resolves to whatever is effectively showing right
        // now (region/parcel/local, and if it's a full day cycle, the
        // currently-interpolated sky) - see LLFloaterFixedEnvironment's
        // own use of the same call for the same purpose.
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getEnvironmentFixedSky(LLEnvironment::ENV_CURRENT, true);
        if (!sky)
        {
            LLSD result;
            result["error"] = "No current sky environment available.";
            callback(result);
            return;
        }

        callback(buildEnvironmentResult(sky));
    }

    // Clones the currently effective sky into an editable LLSettingsSky,
    // and immediately pushes it as the local environment override so that
    // in-place edits followed by update() are visible right away. Mirrors
    // LLFloaterEnvironmentAdjust::captureCurrentEnvironment(), simplified
    // (always clones+re-applies; that floater skips the clone in one edge
    // case as a minor optimization we don't need here).
    LLSettingsSky::ptr_t getEditableSky()
    {
        LLEnvironment& environment = LLEnvironment::instance();
        LLSettingsSky::ptr_t current = environment.getEnvironmentFixedSky(LLEnvironment::ENV_CURRENT, true);
        if (!current)
        {
            return LLSettingsSky::ptr_t();
        }

        LLSettingsSky::ptr_t sky = current->buildClone();
        environment.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
        environment.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
        return sky;
    }

    void toolSetEnvironment(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLSettingsSky::ptr_t sky = getEditableSky();
        if (!sky)
        {
            throw std::runtime_error("No current sky environment available to edit.");
        }

        if (arguments.has("sun"))
        {
            const LLSD& sun = arguments["sun"];
            F32 azimuth, elevation;
            LLVirtualTrackball::getAzimuthAndElevationDeg(sky->getSunRotation(), azimuth, elevation);
            if (sun.has("azimuth")) azimuth = (F32)sun["azimuth"].asReal();
            if (sun.has("elevation")) elevation = (F32)sun["elevation"].asReal();
            sky->setSunRotation(azimuthElevationToQuaternion(azimuth, elevation));
        }

        if (arguments.has("moon"))
        {
            const LLSD& moon = arguments["moon"];
            F32 azimuth, elevation;
            LLVirtualTrackball::getAzimuthAndElevationDeg(sky->getMoonRotation(), azimuth, elevation);
            if (moon.has("azimuth")) azimuth = (F32)moon["azimuth"].asReal();
            if (moon.has("elevation")) elevation = (F32)moon["elevation"].asReal();
            sky->setMoonRotation(azimuthElevationToQuaternion(azimuth, elevation));
        }

        if (arguments.has("clouds"))
        {
            const LLSD& clouds = arguments["clouds"];
            if (clouds.has("color"))
            {
                sky->setCloudColor(llsdToColor3(clouds["color"], sky->getCloudColor()));
            }
            if (clouds.has("scale")) sky->setCloudScale((F32)clouds["scale"].asReal());
            if (clouds.has("shadow")) sky->setCloudShadow((F32)clouds["shadow"].asReal());
            if (clouds.has("variance")) sky->setCloudVariance((F32)clouds["variance"].asReal());
            if (clouds.has("scroll_x") || clouds.has("scroll_y"))
            {
                LLVector2 scroll = sky->getCloudScrollRate();
                if (clouds.has("scroll_x")) scroll.mV[0] = (F32)clouds["scroll_x"].asReal();
                if (clouds.has("scroll_y")) scroll.mV[1] = (F32)clouds["scroll_y"].asReal();
                sky->setCloudScrollRate(scroll);
            }
        }

        if (arguments.has("haze"))
        {
            const LLSD& haze = arguments["haze"];
            if (haze.has("density")) sky->setHazeDensity((F32)haze["density"].asReal());
            if (haze.has("horizon")) sky->setHazeHorizon((F32)haze["horizon"].asReal());
            if (haze.has("blue_density"))
            {
                sky->setBlueDensity(llsdToColor3(haze["blue_density"], sky->getBlueDensity()));
            }
            if (haze.has("blue_horizon"))
            {
                sky->setBlueHorizon(llsdToColor3(haze["blue_horizon"], sky->getBlueHorizon()));
            }
            if (haze.has("distance_multiplier")) sky->setDistanceMultiplier((F32)haze["distance_multiplier"].asReal());
        }

        sky->update();

        callback(buildEnvironmentResult(sky));
    }
}

void registerMCPEnvironmentTools()
{
    LLMCPToolRegistry& registry = LLMCPToolRegistry::instance();

    LLSD empty_schema;
    empty_schema["type"] = "object";
    empty_schema["properties"] = LLSD::emptyMap();

    registry.registerTool(
        "get_environment",
        "Returns the current sky: sun/moon azimuth+elevation (degrees), cloud settings, haze/fog settings.",
        empty_schema,
        toolGetEnvironment);

    LLSD azel_prop;
    azel_prop["type"] = "object";
    LLSD azel_props;
    azel_props["azimuth"] = LLSDMap("type", "number")("description", "Compass direction in degrees (0-360).");
    azel_props["elevation"] = LLSDMap("type", "number")("description", "Height angle in degrees (-90 to 90; higher = higher in the sky).");
    azel_prop["properties"] = azel_props;

    LLSD rgb_prop;
    rgb_prop["type"] = "object";
    LLSD rgb_props;
    rgb_props["r"] = LLSDMap("type", "number");
    rgb_props["g"] = LLSDMap("type", "number");
    rgb_props["b"] = LLSDMap("type", "number");
    rgb_prop["properties"] = rgb_props;

    LLSD clouds_prop;
    clouds_prop["type"] = "object";
    LLSD clouds_props;
    clouds_props["color"] = rgb_prop;
    clouds_props["scale"] = LLSDMap("type", "number");
    clouds_props["shadow"] = LLSDMap("type", "number")("description", "Cloud coverage/density.");
    clouds_props["variance"] = LLSDMap("type", "number");
    clouds_props["scroll_x"] = LLSDMap("type", "number");
    clouds_props["scroll_y"] = LLSDMap("type", "number");
    clouds_prop["properties"] = clouds_props;

    LLSD haze_prop;
    haze_prop["type"] = "object";
    LLSD haze_props;
    haze_props["density"] = LLSDMap("type", "number");
    haze_props["horizon"] = LLSDMap("type", "number");
    haze_props["blue_density"] = rgb_prop;
    haze_props["blue_horizon"] = rgb_prop;
    haze_props["distance_multiplier"] = LLSDMap("type", "number");
    haze_prop["properties"] = haze_props;

    LLSD set_schema;
    set_schema["type"] = "object";
    LLSD set_props;
    set_props["sun"] = azel_prop;
    set_props["moon"] = azel_prop;
    set_props["clouds"] = clouds_prop;
    set_props["haze"] = haze_prop;
    set_schema["properties"] = set_props;

    registry.registerTool(
        "set_environment",
        "Patches the current sky - only the fields you include are changed (read current values via "
        "get_environment first, e.g. to nudge the sun lower: elevation - 15). Applies to the local "
        "environment override and takes effect immediately.",
        set_schema,
        toolSetEnvironment);
}
