#include "jpx.h"

#ifdef FYNEPDF_PDFA_WITH_OPENJPEG

#include <algorithm>
#include <cmath>
#include <cstring>

#include <openjpeg.h>

namespace fynepdf::pdfa::jpx {
namespace {

// The most a picture may take decoded (OpenJPEG keeps 32 bits a sample).
constexpr unsigned long long kMaximumSamples = 128ULL * 1024 * 1024;

struct Memory {
  const unsigned char *data;
  size_t size, at;
};

OPJ_SIZE_T Read(void *buffer, OPJ_SIZE_T count, void *user) {
  Memory *memory = static_cast<Memory *>(user);
  if (memory->at >= memory->size)
    return static_cast<OPJ_SIZE_T>(-1);
  count = std::min<OPJ_SIZE_T>(count, memory->size - memory->at);
  std::memcpy(buffer, memory->data + memory->at, count);
  memory->at += count;
  return count;
}

OPJ_OFF_T Skip(OPJ_OFF_T count, void *user) {
  Memory *memory = static_cast<Memory *>(user);
  if (count < 0)
    count = -std::min<OPJ_OFF_T>(-count, static_cast<OPJ_OFF_T>(memory->at));
  else
    count = std::min<OPJ_OFF_T>(count, static_cast<OPJ_OFF_T>(memory->size - memory->at));
  memory->at += count;
  return count;
}

OPJ_BOOL Seek(OPJ_OFF_T to, void *user) {
  Memory *memory = static_cast<Memory *>(user);
  if (to < 0 || static_cast<size_t>(to) > memory->size)
    return OPJ_FALSE;
  memory->at = static_cast<size_t>(to);
  return OPJ_TRUE;
}

void Quiet(const char *, void *) {}

// A component's sample at full-resolution pixel (x, y), scaled to 0..1.
double Sample(const opj_image_comp_t &comp, int x, int y) {
  const int cx = std::min<int>(x / std::max<int>(1, comp.dx), static_cast<int>(comp.w) - 1);
  const int cy = std::min<int>(y / std::max<int>(1, comp.dy), static_cast<int>(comp.h) - 1);
  long value = comp.data[static_cast<size_t>(cy) * comp.w + cx];
  if (comp.sgnd)
    value += 1L << (comp.prec - 1);
  const double top = static_cast<double>((1UL << comp.prec) - 1);
  return std::clamp(value / top, 0.0, 1.0);
}

// Decodes |bytes| and hands the image to |use| (|most| samples at most); false when it can't.
template <typename Use> bool WithImage(const std::string &bytes, unsigned long long most, Use use) {
  static const unsigned char jp2[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20};
  const bool box = bytes.size() >= 8 && std::memcmp(bytes.data(), jp2, 8) == 0;
  opj_codec_t *codec = opj_create_decompress(box ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
  if (!codec)
    return false;
  opj_set_error_handler(codec, Quiet, nullptr);
  opj_set_warning_handler(codec, Quiet, nullptr);
  opj_set_info_handler(codec, Quiet, nullptr);
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  opj_image_t *image = nullptr;
  Memory memory{reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), 0};
  opj_stream_t *stream = opj_stream_default_create(OPJ_TRUE);
  bool ok = stream && opj_setup_decoder(codec, &parameters);
  if (ok) {
    opj_stream_set_user_data(stream, &memory, nullptr);
    opj_stream_set_user_data_length(stream, bytes.size());
    opj_stream_set_read_function(stream, Read);
    opj_stream_set_skip_function(stream, Skip);
    opj_stream_set_seek_function(stream, Seek);
    ok = opj_read_header(stream, codec, &image) && image && image->numcomps > 0 && image->comps[0].w > 0 && image->comps[0].h > 0 &&
         static_cast<unsigned long long>(image->comps[0].w) * image->comps[0].h * image->numcomps <= most && opj_decode(codec, stream, image) &&
         opj_end_decompress(codec, stream);
  }
  for (OPJ_UINT32 k = 0; ok && k < image->numcomps; ++k)
    ok = image->comps[k].data && image->comps[k].prec >= 1 && image->comps[k].prec <= 16;
  if (ok)
    ok = use(*image);
  if (image)
    opj_image_destroy(image);
  if (stream)
    opj_stream_destroy(stream);
  opj_destroy_codec(codec);
  return ok;
}

} // namespace

bool Decode(const std::string &bytes, Picture &picture, unsigned long long most) {
  return WithImage(bytes, most, [&](const opj_image_t &image) {
    const opj_image_comp_t *comps = image.comps;
    const int n = static_cast<int>(image.numcomps);
    const int width = static_cast<int>(comps[0].w * std::max<OPJ_UINT32>(1, comps[0].dx));
    const int height = static_cast<int>(comps[0].h * std::max<OPJ_UINT32>(1, comps[0].dy));
    const bool ycc = image.color_space == OPJ_CLRSPC_SYCC || image.color_space == OPJ_CLRSPC_EYCC;
    const bool cmyk = image.color_space == OPJ_CLRSPC_CMYK && n >= 4;
    int alpha_at = -1;
    for (int k = 0; k < n; ++k)
      if (comps[k].alpha)
        alpha_at = k;
    if (alpha_at < 0 && !cmyk && (n == 2 || n == 4))
      alpha_at = n - 1;
    const int colours = cmyk ? 4 : n - (alpha_at >= 0 ? 1 : 0) >= 3 ? 3 : 1;
    if (static_cast<unsigned long long>(width) * height * (colours + 1) > most)
      return false;
    picture.width = width;
    picture.height = height;
    picture.components = colours;
    picture.samples.assign(static_cast<size_t>(width) * height * colours, '\0');
    picture.alpha.clear();
    if (alpha_at >= 0)
      picture.alpha.assign(static_cast<size_t>(width) * height, '\0');
    const auto byte = [](double value) { return static_cast<char>(std::lround(std::clamp(value, 0.0, 1.0) * 255)); };
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t at = static_cast<size_t>(y) * width + x;
        char *out = &picture.samples[at * colours];
        if (colours == 3 && ycc) {
          const double luma = Sample(comps[0], x, y), cb = Sample(comps[1], x, y) - 0.5, cr = Sample(comps[2], x, y) - 0.5;
          out[0] = byte(luma + 1.402 * cr);
          out[1] = byte(luma - 0.344136 * cb - 0.714136 * cr);
          out[2] = byte(luma + 1.772 * cb);
        } else {
          for (int k = 0, from = 0; k < colours; ++k, ++from) {
            if (from == alpha_at)
              ++from;
            out[k] = byte(Sample(comps[std::min(from, n - 1)], x, y));
          }
        }
        if (alpha_at >= 0)
          picture.alpha[at] = byte(Sample(comps[alpha_at], x, y));
      }
    }
    return true;
  });
}

bool DecodeGray(const std::string &bytes, std::string &gray, std::string &alpha, int &width, int &height) {
  static const unsigned char jp2[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20};
  const bool box = bytes.size() >= 8 && std::memcmp(bytes.data(), jp2, 8) == 0;
  opj_codec_t *codec = opj_create_decompress(box ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
  if (!codec)
    return false;
  opj_set_error_handler(codec, Quiet, nullptr);
  opj_set_warning_handler(codec, Quiet, nullptr);
  opj_set_info_handler(codec, Quiet, nullptr);
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  opj_image_t *image = nullptr;
  Memory memory{reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), 0};
  opj_stream_t *stream = opj_stream_default_create(OPJ_TRUE);
  bool ok = stream && opj_setup_decoder(codec, &parameters);
  if (ok) {
    opj_stream_set_user_data(stream, &memory, nullptr);
    opj_stream_set_user_data_length(stream, bytes.size());
    opj_stream_set_read_function(stream, Read);
    opj_stream_set_skip_function(stream, Skip);
    opj_stream_set_seek_function(stream, Seek);
    ok = opj_read_header(stream, codec, &image) && image && image->numcomps > 0 && image->comps[0].w > 0 && image->comps[0].h > 0 &&
         static_cast<unsigned long long>(image->comps[0].w) * image->comps[0].h * image->numcomps <= kMaximumSamples &&
         opj_decode(codec, stream, image) && opj_end_decompress(codec, stream);
  }
  if (ok) {
    for (OPJ_UINT32 k = 0; k < image->numcomps; ++k)
      ok = ok && image->comps[k].data && image->comps[k].prec >= 1 && image->comps[k].prec <= 16;
  }
  if (ok) {
    const opj_image_comp_t *comps = image->comps;
    const int n = static_cast<int>(image->numcomps);
    width = static_cast<int>(comps[0].w * std::max<OPJ_UINT32>(1, comps[0].dx));
    height = static_cast<int>(comps[0].h * std::max<OPJ_UINT32>(1, comps[0].dy));
    const bool ycc = image->color_space == OPJ_CLRSPC_SYCC || image->color_space == OPJ_CLRSPC_EYCC;
    const bool cmyk = image->color_space == OPJ_CLRSPC_CMYK && n >= 4;
    // The alpha channel: the one OpenJPEG marks, or the last of 2 (grey) or 4 (RGB) otherwise.
    int alpha_at = -1;
    for (int k = 0; k < n; ++k)
      if (comps[k].alpha)
        alpha_at = k;
    if (alpha_at < 0 && !cmyk && (n == 2 || n == 4))
      alpha_at = n - 1;
    gray.assign(static_cast<size_t>(width) * height, '\0');
    alpha.clear();
    if (alpha_at >= 0)
      alpha.assign(static_cast<size_t>(width) * height, '\0');
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        double g;
        if (cmyk)
          g = 1 - std::min(1.0, 0.3 * Sample(comps[0], x, y) + 0.59 * Sample(comps[1], x, y) + 0.11 * Sample(comps[2], x, y) + Sample(comps[3], x, y));
        else if (n >= 3 && !ycc)
          g = 0.299 * Sample(comps[0], x, y) + 0.587 * Sample(comps[1], x, y) + 0.114 * Sample(comps[2], x, y);
        else
          g = Sample(comps[0], x, y);  // grey, or YCC's brightness
        const size_t at = static_cast<size_t>(y) * width + x;
        gray[at] = static_cast<char>(std::lround(std::clamp(g, 0.0, 1.0) * 255));
        if (alpha_at >= 0)
          alpha[at] = static_cast<char>(std::lround(Sample(comps[alpha_at], x, y) * 255));
      }
    }
  }
  if (image)
    opj_image_destroy(image);
  if (stream)
    opj_stream_destroy(stream);
  opj_destroy_codec(codec);
  return ok;
}

} // namespace fynepdf::pdfa::jpx

#else

namespace fynepdf::pdfa::jpx {
bool DecodeGray(const std::string &, std::string &, std::string &, int &, int &) { return false; }
bool Decode(const std::string &, Picture &, unsigned long long) { return false; }
} // namespace fynepdf::pdfa::jpx

#endif
