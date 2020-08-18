#include "readpng_cpu.h"

// Comment
#include <ATen/ATen.h>
#include <setjmp.h>
#include <string>

#if !PNG_FOUND
torch::Tensor decodePNG(const torch::Tensor& data) {
  AT_ERROR("decodePNG: torchvision not compiled with libPNG support");
}
#else
#include <png.h>


namespace {
  struct Reader {
      png_const_bytep ptr;
  };

  static void readCallback(png_structp png_ptr, png_bytep output, png_size_t bytes)
  {
    Reader *reader = static_cast<Reader*>(png_get_io_ptr(png_ptr));
    std::copy(reader->ptr, reader->ptr + bytes, output);
    reader->ptr += bytes;
  }
}

// See the comments in the jpeg decoder for why we need to do this. Spoiler:
// undefined behavior with setjmp and longjmp.
static void actualDecodePNG(const torch::Tensor& data,
        png_structp *png_ptr,
        png_infop *info_ptr,
        torch::Tensor *output,
        Reader *Reader
        )
{
  unsigned char *datap = data.accessor<unsigned char, 1>().data();

  if (setjmp(png_jmpbuf(png_ptr)) != 0) {
    TORCH_CHECK(false, "Internal error.");
  }
  bool is_png = !png_sig_cmp(datap, 0, 8);
  TORCH_CHECK(is_png, "Content is not png!")
  reader->ptr = png_const_bytep(datap) + 8;

  png_set_sig_bytes(png_ptr, 8);
  png_set_read_fn(png_ptr, reader, readCallback);
  png_read_info(png_ptr, info_ptr);

  png_uint_32 width, height;
  int bit_depth, color_type;
  png_uint_32 retval = png_get_IHDR(
      png_ptr,
      info_ptr,
      &width,
      &height,
      &bit_depth,
      &color_type,
      nullptr,
      nullptr,
      nullptr);

  if (retval != 1) {
    TORCH_CHECK(retval == 1, "Could read image metadata from content.")
  }
  if (color_type != PNG_COLOR_TYPE_RGB) {
    TORCH_CHECK(
        color_type == PNG_COLOR_TYPE_RGB, "Non RGB images are not supported.")
  }

  uint8_t *ptr = tensor.accessor<uint8_t, 3>().data();
  size_t bytes = png_get_rowbytes(png_ptr, info_ptr);
  for (decltype(height) i = 0; i < height; ++i) {
    png_read_row(png_ptr, ptr, nullptr);
    ptr += bytes;
  }
}


torch::Tensor decodePNG(const torch::Tensor& data) {
  auto png_ptr =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  TORCH_CHECK(png_ptr, "libpng read structure allocation failed!")
  auto info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    // Seems redundant with the if statement. done here to avoid leaking memory.
    TORCH_CHECK(info_ptr, "libpng info structure allocation failed!")
  }

  auto tensor =
      torch::empty({int64_t(height), int64_t(width), int64_t(3)}, torch::kU8);

  Reader reader;
  actualDecodePNG(data, png_ptr, info_ptr, &tensor, &reader);

  png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

  return tensor;
}
#endif // PNG_FOUND
