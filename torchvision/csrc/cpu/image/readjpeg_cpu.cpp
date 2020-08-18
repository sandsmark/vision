#include "readjpeg_cpu.h"

#include <ATen/ATen.h>
#include <setjmp.h>
#include <string>

#if !JPEG_FOUND

torch::Tensor decodeJPEG(const torch::Tensor& data) {
  AT_ERROR("decodeJPEG: torchvision not compiled with libjpeg support");
}

#else
#include <jpeglib.h>

const static JOCTET EOI_BUFFER[1] = {JPEG_EOI};
char jpegLastErrorMsg[JMSG_LENGTH_MAX];

struct torch_jpeg_error_mgr {
  struct jpeg_error_mgr pub; /* "public" fields */
  jmp_buf setjmp_buffer; /* for return to caller */
};

typedef struct torch_jpeg_error_mgr* torch_jpeg_error_ptr;

void torch_jpeg_error_exit(j_common_ptr cinfo) {
  /* cinfo->err really points to a torch_jpeg_error_mgr struct, so coerce
   * pointer */
  torch_jpeg_error_ptr myerr = (torch_jpeg_error_ptr)cinfo->err;

  /* Always display the message. */
  /* We could postpone this until after returning, if we chose. */
  // (*cinfo->err->output_message)(cinfo);
  /* Create the message */
  (*(cinfo->err->format_message))(cinfo, jpegLastErrorMsg);

  /* Return control to the setjmp point */
  longjmp(myerr->setjmp_buffer, 1);
}

struct torch_jpeg_mgr {
  struct jpeg_source_mgr pub;
  const JOCTET* data;
  size_t len;
};

static void torch_jpeg_init_source(j_decompress_ptr cinfo) {}

static boolean torch_jpeg_fill_input_buffer(j_decompress_ptr cinfo) {
  torch_jpeg_mgr* src = (torch_jpeg_mgr*)cinfo->src;
  // No more data.  Probably an incomplete image;  Raise exception.
  torch_jpeg_error_ptr myerr = (torch_jpeg_error_ptr)cinfo->err;
  strcpy(jpegLastErrorMsg, "Image is incomplete or truncated");
  longjmp(myerr->setjmp_buffer, 1);
  src->pub.next_input_byte = EOI_BUFFER;
  src->pub.bytes_in_buffer = 1;
  return TRUE;
}

static void torch_jpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
  torch_jpeg_mgr* src = (torch_jpeg_mgr*)cinfo->src;
  if (src->pub.bytes_in_buffer < num_bytes) {
    // Skipping over all of remaining data;  output EOI.
    src->pub.next_input_byte = EOI_BUFFER;
    src->pub.bytes_in_buffer = 1;
  } else {
    // Skipping over only some of the remaining data.
    src->pub.next_input_byte += num_bytes;
    src->pub.bytes_in_buffer -= num_bytes;
  }
}

static void torch_jpeg_term_source(j_decompress_ptr cinfo) {}

static void torch_jpeg_set_source_mgr(
    j_decompress_ptr cinfo,
    const unsigned char* data,
    size_t len) {
  torch_jpeg_mgr* src;
  if (cinfo->src == 0) { // if this is first time;  allocate memory
    cinfo->src = (struct jpeg_source_mgr*)(*cinfo->mem->alloc_small)(
        (j_common_ptr)cinfo, JPOOL_PERMANENT, sizeof(torch_jpeg_mgr));
  }
  src = (torch_jpeg_mgr*)cinfo->src;
  src->pub.init_source = torch_jpeg_init_source;
  src->pub.fill_input_buffer = torch_jpeg_fill_input_buffer;
  src->pub.skip_input_data = torch_jpeg_skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; // default
  src->pub.term_source = torch_jpeg_term_source;
  // fill the buffers
  src->data = (const JOCTET*)data;
  src->len = len;
  src->pub.bytes_in_buffer = len;
  src->pub.next_input_byte = src->data;
}

}

// This needs to happen in a separate function to avoid undefined behavior with
// setjmp and longjmp
static void actualDecodeJPEG(const torch::Tensor& input,
        jpeg_decompress_struct *cinfo,
        torch_jpeg_error_mgr *jerr,
        torch::Tensor *output
        )
{
  uint8_t *inputData = input.data_ptr<uint8_t>();
  // Setup decompression structure
  cinfo->err = jpeg_std_error(&jerr->pub);
  jerr->pub.error_exit = torch_jpeg_error_exit;
  /* Establish the setjmp return context for my_error_exit to use. */
  if (setjmp(jerr->setjmp_buffer)) {
    /* If we get here, the JPEG code has signaled an error.
     * We need to clean up the JPEG object.
     */
    jpeg_destroy_decompress(cinfo);
    AT_ERROR(jpegLastErrorMsg);
  }

  jpeg_create_decompress(cinfo);
  torch_jpeg_set_source_mgr(cinfo, inputData, input.numel());

  // read info from header.
  jpeg_read_header(cinfo, TRUE);
  jpeg_start_decompress(cinfo);

  const int height = cinfo->output_height;
  const int width = cinfo->output_width;
  const int components = cinfo->output_components;

  const int stride = width * components;
  uint8_t *outputData = output->data_ptr<uint8_t>();
  while (cinfo->output_scanline < cinfo->output_height) {
    /* jpeg_read_scanlines expects an array of pointers to scanlines.
     * Here the array is only one element long, but you could ask for
     * more than one scanline at a time if that's more convenient.
     */
    jpeg_read_scanlines(cinfo, &outputData, 1);
    outputData += stride;
  }

  jpeg_finish_decompress(cinfo);
  jpeg_destroy_decompress(cinfo);
}

torch::Tensor decodeJPEG(const torch::Tensor& data)
{
  // setjmp and longjmp leads to undefined behavior and are generally just a
  // PITA. So to make sure we don't invoke any undefined behavior declare
  // everything outside the actual decoding function using setjmp and longjmp.
  // For now most compilers (to the best of my knowledge) aren't affected by
  // this, but it might lead to memory corruption in the future.
  jpeg_decompress_struct cinfo;
  torch_jpeg_error_mgr jerr;
  auto output = torch::empty(
      {int64_t(height), int64_t(width), int64_t(components)}, torch::kU8);
  actualDecodeJPEG(data, &cinfo, &jerr, &output);
  return output;
}

#endif // JPEG_FOUND
