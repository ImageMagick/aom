/*
 * Copyright (c) 2021, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

// This tool creates a film grain table, for use in stills and videos,
// representing the noise that one would get by shooting with a digital camera
// at a given light level. Much of the noise in digital images is photon shot
// noise, which is due to the characteristics of photon arrival and grows in
// standard deviation as the square root of the expected number of photons
// captured.
// https://www.photonstophotos.net/Emil%20Martinec/noise.html#shotnoise
//
// The proxy used by this tool for the amount of light captured is the ISO value
// such that the focal plane exposure at the time of capture would have been
// mapped by a 35mm camera to the output lightness observed in the image. That
// is, if one were to shoot on a 35mm camera (36×24mm sensor) at the nominal
// exposure for that ISO setting, the resulting image should contain noise of
// the same order of magnitude as generated by this tool.
//
// Example usage:
//
//     ./photon_noise_table --width=3840 --height=2160 --iso=25600 -o noise.tbl
//     # Then, for example:
//     aomenc --film-grain-table=noise.tbl ...
//     # Or:
//     avifenc -c aom -a film-grain-table=noise.tbl ...
//
// The (mostly) square-root relationship between light intensity and noise
// amplitude holds in linear light, but AV1 streams are most often encoded
// non-linearly, and the film grain is applied to those non-linear values.
// Therefore, this tool must account for the non-linearity, and this is
// controlled by the optional `--transfer-function` (or `-t`) parameter, which
// specifies the tone response curve that will be used when encoding the actual
// image. The default for this tool is sRGB, which is approximately similar to
// an encoding gamma of 1/2.2 (i.e. a decoding gamma of 2.2) though not quite
// identical.
//
// As alluded to above, the tool assumes that the image is taken from the
// entirety of a 36×24mm (“35mm format”) sensor. If that assumption does not
// hold, then a “35mm-equivalent ISO value” that can be passed to the tool can
// be obtained by multiplying the true ISO value by the ratio of 36×24mm to the
// area that was actually used. For formats that approximately share the same
// aspect ratio, this is often expressed as the square of the “equivalence
// ratio” which is the ratio of their diagonals. For example, APS-C (often
// ~24×16mm) is said to have an equivalence ratio of 1.5 relative to the 35mm
// format, and therefore ISO 1000 on APS-C and ISO 1000×1.5² = 2250 on 35mm
// produce an image of the same lightness from the same amount of light spread
// onto their respective surface areas (resulting in different focal plane
// exposures), and those images will thus have similar amounts of noise if the
// cameras are of similar technology. https://doi.org/10.1117/1.OE.57.11.110801
//
// The tool needs to know the resolution of the images to which its grain tables
// will be applied so that it can know how the light on the sensor was shared
// between its pixels. As a general rule, while a higher pixel count will lead
// to more noise per pixel, when the final image is viewed at the same physical
// size, that noise will tend to “average out” to the same amount over a given
// area, since there will be more pixels in it which, in aggregate, will have
// received essentially as much light. Put differently, the amount of noise
// depends on the scale at which it is measured, and the decision for this tool
// was to make that scale relative to the image instead of its constituent
// samples. For more on this, see:
//
// https://www.photonstophotos.net/Emil%20Martinec/noise-p3.html#pixelsize
// https://www.dpreview.com/articles/5365920428/the-effect-of-pixel-and-sensor-sizes-on-noise/2
// https://www.dpreview.com/videos/7940373140/dpreview-tv-why-lower-resolution-sensors-are-not-better-in-low-light

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aom_dsp/grain_table.h"
#include "common/args.h"
#include "common/tools_common.h"

static const char *exec_name;

static const struct arg_enum_list transfer_functions[] = {
  { "bt470m", AOM_CICP_TC_BT_470_M }, { "bt470bg", AOM_CICP_TC_BT_470_B_G },
  { "srgb", AOM_CICP_TC_SRGB },       { "smpte2084", AOM_CICP_TC_SMPTE_2084 },
  { "hlg", AOM_CICP_TC_HLG },         ARG_ENUM_LIST_END
};

static arg_def_t help_arg =
    ARG_DEF("h", "help", 0, "Show the available options");
static arg_def_t width_arg =
    ARG_DEF("w", "width", 1, "Width of the image in pixels (required)");
static arg_def_t height_arg =
    ARG_DEF("l", "height", 1, "Height of the image in pixels (required)");
static arg_def_t iso_arg = ARG_DEF(
    "i", "iso", 1, "ISO setting indicative of the light level (required)");
static arg_def_t output_arg =
    ARG_DEF("o", "output", 1,
            "Output file to which to write the film grain table (required)");
static arg_def_t transfer_function_arg =
    ARG_DEF_ENUM("t", "transfer-function", 1,
                 "Transfer function used by the encoded image (default = sRGB)",
                 transfer_functions);

void usage_exit(void) {
  fprintf(stderr,
          "Usage: %s [--transfer-function=<tf>] --width=<width> "
          "--height=<height> --iso=<iso> --output=<output.tbl>\n",
          exec_name);
  exit(EXIT_FAILURE);
}

typedef struct {
  float (*to_linear)(float);
  float (*from_linear)(float);
  // In linear output light. This would typically be 0.18 for SDR (this matches
  // the definition of Standard Output Sensitivity from ISO 12232:2019), but in
  // HDR, we certainly do not want to consider 18% of the maximum output a
  // “mid-tone”, as it would be e.g. 1800 cd/m² for SMPTE ST 2084 (PQ).
  float mid_tone;
} transfer_function_t;

static const transfer_function_t *find_transfer_function(
    aom_transfer_characteristics_t tc);

typedef struct {
  int width;
  int height;
  int iso_setting;

  const transfer_function_t *transfer_function;

  const char *output_filename;
} photon_noise_args_t;

static void parse_args(int argc, char **argv,
                       photon_noise_args_t *photon_noise_args) {
  static const arg_def_t *args[] = { &help_arg,   &width_arg,
                                     &height_arg, &iso_arg,
                                     &output_arg, &transfer_function_arg,
                                     NULL };
  struct arg arg;
  int width_set = 0, height_set = 0, iso_set = 0, output_set = 0, i;

  photon_noise_args->transfer_function =
      find_transfer_function(AOM_CICP_TC_SRGB);

  for (i = 1; i < argc; i += arg.argv_step) {
    arg.argv_step = 1;
    if (arg_match(&arg, &help_arg, argv + i)) {
      arg_show_usage(stdout, args);
      exit(EXIT_SUCCESS);
    } else if (arg_match(&arg, &width_arg, argv + i)) {
      photon_noise_args->width = arg_parse_int(&arg);
      width_set = 1;
    } else if (arg_match(&arg, &height_arg, argv + i)) {
      photon_noise_args->height = arg_parse_int(&arg);
      height_set = 1;
    } else if (arg_match(&arg, &iso_arg, argv + i)) {
      photon_noise_args->iso_setting = arg_parse_int(&arg);
      iso_set = 1;
    } else if (arg_match(&arg, &output_arg, argv + i)) {
      photon_noise_args->output_filename = arg.val;
      output_set = 1;
    } else if (arg_match(&arg, &transfer_function_arg, argv + i)) {
      const aom_transfer_characteristics_t tc = arg_parse_enum(&arg);
      photon_noise_args->transfer_function = find_transfer_function(tc);
    } else {
      fatal("unrecognized argument \"%s\", see --help for available options",
            argv[i]);
    }
  }

  if (!width_set) {
    fprintf(stderr, "Missing required parameter --width\n");
    exit(EXIT_FAILURE);
  }

  if (!height_set) {
    fprintf(stderr, "Missing required parameter --height\n");
    exit(EXIT_FAILURE);
  }

  if (!iso_set) {
    fprintf(stderr, "Missing required parameter --iso\n");
    exit(EXIT_FAILURE);
  }

  if (!output_set) {
    fprintf(stderr, "Missing required parameter --output\n");
    exit(EXIT_FAILURE);
  }
}

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

static float gamma22_to_linear(float g) { return powf(g, 2.2f); }
static float gamma22_from_linear(float l) { return powf(l, 1 / 2.2f); }
static float gamma28_to_linear(float g) { return powf(g, 2.8f); }
static float gamma28_from_linear(float l) { return powf(l, 1 / 2.8f); }

static float srgb_to_linear(float srgb) {
  return srgb <= 0.04045f ? srgb / 12.92f
                          : powf((srgb + 0.055f) / 1.055f, 2.4f);
}
static float srgb_from_linear(float linear) {
  return linear <= 0.0031308f ? 12.92f * linear
                              : 1.055f * powf(linear, 1 / 2.4f) - 0.055f;
}

static const float kPqM1 = 2610.f / 16384;
static const float kPqM2 = 128 * 2523.f / 4096;
static const float kPqC1 = 3424.f / 4096;
static const float kPqC2 = 32 * 2413.f / 4096;
static const float kPqC3 = 32 * 2392.f / 4096;
static float pq_to_linear(float pq) {
  const float pq_pow_inv_m2 = powf(pq, 1.f / kPqM2);
  return powf(maxf(0, pq_pow_inv_m2 - kPqC1) / (kPqC2 - kPqC3 * pq_pow_inv_m2),
              1.f / kPqM1);
}
static float pq_from_linear(float linear) {
  const float linear_pow_m1 = powf(linear, kPqM1);
  return powf((kPqC1 + kPqC2 * linear_pow_m1) / (1 + kPqC3 * linear_pow_m1),
              kPqM2);
}

// Note: it is perhaps debatable whether “linear” for HLG should be scene light
// or display light. Here, it is implemented in terms of display light assuming
// a nominal peak display luminance of 1000 cd/m², hence the system γ of 1.2. To
// make it scene light instead, the OOTF (powf(x, 1.2f)) and its inverse should
// be removed from the functions below, and the .mid_tone should be replaced
// with powf(26.f / 1000, 1 / 1.2f).
static const float kHlgA = 0.17883277f;
static const float kHlgB = 0.28466892f;
static const float kHlgC = 0.55991073f;
static float hlg_to_linear(float hlg) {
  // EOTF = OOTF ∘ OETF⁻¹
  const float linear =
      hlg <= 0.5f ? hlg * hlg / 3 : ((flout) exp((hlg - kHlgC) / kHlgA) + kHlgB) / 12;
  return powf(linear, 1.2f);
}
static float hlg_from_linear(float linear) {
  // EOTF⁻¹ = OETF ∘ OOTF⁻¹
  linear = powf(linear, 1.f / 1.2f);
  return linear <= 1.f / 12 ? sqrtf(3 * linear)
                            : kHlgA * logf(12 * linear - kHlgB) + kHlgC;
}

static const transfer_function_t *find_transfer_function(
    aom_transfer_characteristics_t tc) {
  static const transfer_function_t
      kGamma22TransferFunction = { .to_linear = &gamma22_to_linear,
                                   .from_linear = &gamma22_from_linear,
                                   .mid_tone = 0.18f },
      kGamma28TransferFunction = { .to_linear = &gamma28_to_linear,
                                   .from_linear = &gamma28_from_linear,
                                   .mid_tone = 0.18f },
      kSRgbTransferFunction = { .to_linear = &srgb_to_linear,
                                .from_linear = &srgb_from_linear,
                                .mid_tone = 0.18f },
      kPqTransferFunction = { .to_linear = &pq_to_linear,
                              .from_linear = &pq_from_linear,
                              // https://www.itu.int/pub/R-REP-BT.2408-4-2021
                              // page 6 (PDF page 8)
                              .mid_tone = 26.f / 10000 },
      kHlgTransferFunction = { .to_linear = &hlg_to_linear,
                               .from_linear = &hlg_from_linear,
                               .mid_tone = 26.f / 1000 };

  switch (tc) {
    case AOM_CICP_TC_BT_470_M: return &kGamma22TransferFunction;
    case AOM_CICP_TC_BT_470_B_G: return &kGamma28TransferFunction;
    case AOM_CICP_TC_SRGB: return &kSRgbTransferFunction;
    case AOM_CICP_TC_SMPTE_2084: return &kPqTransferFunction;
    case AOM_CICP_TC_HLG: return &kHlgTransferFunction;

    default: fatal("unimplemented transfer function %d", tc);
  }
}

static void generate_photon_noise(const photon_noise_args_t *photon_noise_args,
                                  aom_film_grain_t *film_grain) {
  // Assumes a daylight-like spectrum.
  // https://www.strollswithmydog.com/effective-quantum-efficiency-of-sensor/#:~:text=11%2C260%20photons/um%5E2/lx-s
  static const float kPhotonsPerLxSPerUm2 = 11260;

  // Order of magnitude for cameras in the 2010-2020 decade, taking the CFA into
  // account.
  static const float kEffectiveQuantumEfficiency = 0.20f;

  // Also reasonable values for current cameras. The read noise is typically
  // higher than this at low ISO settings but it matters less there.
  static const float kPhotoResponseNonUniformity = 0.005f;
  static const float kInputReferredReadNoise = 1.5f;

  // Focal plane exposure for a mid-tone (typically a 18% reflectance card), in
  // lx·s.
  const float mid_tone_exposure = 10.f / photon_noise_args->iso_setting;

  // In microns. Assumes a 35mm sensor (36mm × 24mm).
  const float pixel_area_um2 = (36000 * 24000.f) / (photon_noise_args->width *
                                                    photon_noise_args->height);

  const float mid_tone_electrons_per_pixel = kEffectiveQuantumEfficiency *
                                             kPhotonsPerLxSPerUm2 *
                                             mid_tone_exposure * pixel_area_um2;
  const float max_electrons_per_pixel =
      mid_tone_electrons_per_pixel /
      photon_noise_args->transfer_function->mid_tone;

  int i;

  film_grain->num_y_points = 14;
  for (i = 0; i < film_grain->num_y_points; ++i) {
    float x = i / (film_grain->num_y_points - 1.f);
    const float linear = photon_noise_args->transfer_function->to_linear(x);
    const float electrons_per_pixel = max_electrons_per_pixel * linear;
    // Quadrature sum of the relevant sources of noise, in electrons rms. Photon
    // shot noise is sqrt(electrons) so we can skip the square root and the
    // squaring.
    // https://en.wikipedia.org/wiki/Addition_in_quadrature
    // https://doi.org/10.1117/3.725073
    const float noise_in_electrons =
        sqrtf(kInputReferredReadNoise * kInputReferredReadNoise +
              electrons_per_pixel +
              (kPhotoResponseNonUniformity * kPhotoResponseNonUniformity *
               electrons_per_pixel * electrons_per_pixel));
    const float linear_noise = noise_in_electrons / max_electrons_per_pixel;
    const float linear_range_start = maxf(0.f, linear - 2 * linear_noise);
    const float linear_range_end = minf(1.f, linear + 2 * linear_noise);
    const float tf_slope =
        (photon_noise_args->transfer_function->from_linear(linear_range_end) -
         photon_noise_args->transfer_function->from_linear(
             linear_range_start)) /
        (linear_range_end - linear_range_start);
    float encoded_noise = linear_noise * tf_slope;

    x = roundf(255 * x);
    encoded_noise = minf(255.f, roundf(255 * 7.88f * encoded_noise));

    film_grain->scaling_points_y[i][0] = (int)x;
    film_grain->scaling_points_y[i][1] = (int)encoded_noise;
  }

  film_grain->apply_grain = 1;
  film_grain->update_parameters = 1;
  film_grain->num_cb_points = 0;
  film_grain->num_cr_points = 0;
  film_grain->scaling_shift = 8;
  film_grain->ar_coeff_lag = 0;
  film_grain->ar_coeffs_cb[0] = 0;
  film_grain->ar_coeffs_cr[0] = 0;
  film_grain->ar_coeff_shift = 6;
  film_grain->cb_mult = 0;
  film_grain->cb_luma_mult = 0;
  film_grain->cb_offset = 0;
  film_grain->cr_mult = 0;
  film_grain->cr_luma_mult = 0;
  film_grain->cr_offset = 0;
  film_grain->overlap_flag = 1;
  film_grain->random_seed = 7391;
  film_grain->chroma_scaling_from_luma = 0;
}

int main(int argc, char **argv) {
  photon_noise_args_t photon_noise_args;
  aom_film_grain_table_t film_grain_table;
  aom_film_grain_t film_grain;
  struct aom_internal_error_info error_info;
  memset(&photon_noise_args, 0, sizeof(photon_noise_args));
  memset(&film_grain_table, 0, sizeof(film_grain_table));
  memset(&film_grain, 0, sizeof(film_grain));
  memset(&error_info, 0, sizeof(error_info));

  exec_name = argv[0];
  parse_args(argc, argv, &photon_noise_args);

  generate_photon_noise(&photon_noise_args, &film_grain);
  aom_film_grain_table_append(&film_grain_table, 0, 9223372036854775807ull,
                              &film_grain);
  if (aom_film_grain_table_write(&film_grain_table,
                                 photon_noise_args.output_filename,
                                 &error_info) != AOM_CODEC_OK) {
    aom_film_grain_table_free(&film_grain_table);
    fprintf(stderr, "Failed to write film grain table");
    if (error_info.has_detail) {
      fprintf(stderr, ": %s", error_info.detail);
    }
    fprintf(stderr, "\n");
    return EXIT_FAILURE;
  }
  aom_film_grain_table_free(&film_grain_table);

  return EXIT_SUCCESS;
}
