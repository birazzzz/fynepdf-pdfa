#include "jpeg.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>

namespace fynepdf::pdfa::jpeg {
namespace {

// libjpeg's default error handler exits the process; this one jumps back instead.
struct Error {
  jpeg_error_mgr mgr;
  std::jmp_buf jump;
};

void OnError(j_common_ptr info) { std::longjmp(reinterpret_cast<Error *>(info->err)->jump, 1); }
void OnMessage(j_common_ptr) {}

// The compressed bytes, collected from libjpeg's memory destination.
struct Output {
  unsigned char *data = nullptr;
  unsigned long size = 0;
  ~Output() { std::free(data); }
};

// MozJPEG (a libjpeg some projects link) defaults to progressive scans and trellis quantisation,
// which a brightness-only copy can't take; libjpeg-turbo's baseline defaults are asked for. Set
// before the defaults are.
void Baseline(j_compress_ptr info) {
#ifdef JPEG_C_PARAM_SUPPORTED
  jpeg_c_set_int_param(info, JINT_COMPRESS_PROFILE, JCP_FASTEST);
#else
  (void)info;
#endif
}

} // namespace

bool GrayFromYCbCr(const std::string &bytes, std::string &out) {
  jpeg_decompress_struct src{};
  jpeg_compress_struct dst{};
  Error error{};
  Output output;
  src.err = jpeg_std_error(&error.mgr);
  dst.err = &error.mgr;
  error.mgr.error_exit = OnError;
  error.mgr.output_message = OnMessage;
  volatile bool created_dst = false;  // read after longjmp
  if (setjmp(error.jump)) {
    if (created_dst)
      jpeg_destroy_compress(&dst);
    jpeg_destroy_decompress(&src);
    return false;
  }
  jpeg_create_decompress(&src);
  jpeg_mem_src(&src, reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<unsigned long>(bytes.size()));
  if (jpeg_read_header(&src, TRUE) != JPEG_HEADER_OK || src.jpeg_color_space != JCS_YCbCr || src.num_components != 3 ||
      src.comp_info[0].h_samp_factor != src.max_h_samp_factor || src.comp_info[0].v_samp_factor != src.max_v_samp_factor) {
    jpeg_destroy_decompress(&src);
    return false;
  }
  jvirt_barray_ptr *coefficients = jpeg_read_coefficients(&src);
  jpeg_create_compress(&dst);
  created_dst = true;
  Baseline(&dst);
  jpeg_copy_critical_parameters(&src, &dst);
  // As jpegtran's transupp.c: grey, keeping the brightness channel's quantisation table.
  const int table = dst.comp_info[0].quant_tbl_no;
  jpeg_set_colorspace(&dst, JCS_GRAYSCALE);
  dst.comp_info[0].quant_tbl_no = table;
  dst.optimize_coding = TRUE;
  jpeg_mem_dest(&dst, &output.data, &output.size);
  jpeg_write_coefficients(&dst, coefficients);
  jpeg_finish_compress(&dst);
  jpeg_destroy_compress(&dst);
  jpeg_finish_decompress(&src);
  jpeg_destroy_decompress(&src);
  out.assign(reinterpret_cast<const char *>(output.data), output.size);
  return output.size > 0;
}

bool Encode(const unsigned char *pixels, int width, int height, int components, int quality, std::string &out) {
  jpeg_compress_struct dst{};
  Error error{};
  Output output;
  dst.err = jpeg_std_error(&error.mgr);
  error.mgr.error_exit = OnError;
  error.mgr.output_message = OnMessage;
  if (setjmp(error.jump)) {
    jpeg_destroy_compress(&dst);
    return false;
  }
  jpeg_create_compress(&dst);
  Baseline(&dst);
  jpeg_mem_dest(&dst, &output.data, &output.size);
  dst.image_width = static_cast<JDIMENSION>(width);
  dst.image_height = static_cast<JDIMENSION>(height);
  dst.input_components = components;
  dst.in_color_space = components == 3 ? JCS_RGB : JCS_GRAYSCALE;
  jpeg_set_defaults(&dst);
  jpeg_set_quality(&dst, quality, TRUE);
  dst.optimize_coding = TRUE;
  jpeg_start_compress(&dst, TRUE);
  while (dst.next_scanline < dst.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(pixels + static_cast<size_t>(dst.next_scanline) * width * components);
    jpeg_write_scanlines(&dst, &row, 1);
  }
  jpeg_finish_compress(&dst);
  jpeg_destroy_compress(&dst);
  out.assign(reinterpret_cast<const char *>(output.data), output.size);
  return output.size > 0;
}

} // namespace fynepdf::pdfa::jpeg
