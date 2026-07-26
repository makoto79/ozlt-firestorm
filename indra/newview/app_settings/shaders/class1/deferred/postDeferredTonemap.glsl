/**
 * @file postDeferredTonemap.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;

uniform sampler3D color_grading_lut;
uniform float color_grading_lut_intensity;
uniform int color_grading_lut_enabled;
uniform float color_grading_lut_size;
uniform int color_grading_lut_is_log;

in vec2 vary_fragcoord;

#ifdef GAMMA_CORRECT
uniform float gamma;
#endif

vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);
vec3 toneMap(vec3 color);

// Standard Cineon: black at code 95, reference white at 685, 300 codes per
// decade of exposure, black offset 0.0108. Maps scene-linear HDR (superwhites
// up to ~13.5x ref white) into the 0-1 domain that print film emulation LUTs
// expect as input.
vec3 linear_to_cineon(vec3 x)
{
    return clamp((log2(max(x, vec3(0.0)) * (1.0 - 0.0108) + 0.0108) * (300.0 / log2(10.0)) + 685.0) / 1023.0, 0.0, 1.0);
}

vec3 clampHDRRange(vec3 color);

#ifdef GAMMA_CORRECT
vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}
#endif

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(diffuseRect, vary_fragcoord);

    vec3 hdr = diff.rgb; // scene-linear, pre-tonemap; input for log-domain LUTs

#ifndef NO_POST
    diff.rgb = toneMap(diff.rgb);
#else
    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
#endif

    if (color_grading_lut_enabled != 0)
    {
        float scale = (color_grading_lut_size - 1.0) / color_grading_lut_size;
        float offset = 0.5 / color_grading_lut_size;

        // Grading LUTs are display-referred: both the lookup and the intensity
        // mix must happen in gamma-encoded space, not linear.
        vec3 display = linear_to_srgb(clamp(diff.rgb, vec3(0.0), vec3(1.0)));
        vec3 coord = (color_grading_lut_is_log != 0) ? linear_to_cineon(hdr) : display;
        vec3 lut_color = texture(color_grading_lut, coord * scale + offset).rgb;
        display = mix(display, lut_color, color_grading_lut_intensity);
        diff.rgb = srgb_to_linear(display);
    }

#ifdef GAMMA_CORRECT
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif

#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0)); // We should always be 0-1 past this point

    //debugExposure(diff.rgb);
    frag_color = diff;
}

