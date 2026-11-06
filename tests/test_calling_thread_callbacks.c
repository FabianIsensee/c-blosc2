/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Unit tests for codecs and filters registered with BLOSC2_CALLING_THREAD_ONLY:
  their callbacks must run on the thread that called C-Blosc2, whatever
  nthreads, blosc2_set_nthreads() or BLOSC_NTHREADS say.

  Copyright (c) 2026  Blosc Development Team <blosc@blosc.org>
  https://blosc.org
  License: BSD 3-Clause (see LICENSE.txt)

  See LICENSE.txt for details about copyright and rights to use.
**********************************************************************/

#include "test_common.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
#endif

#define NTHREADS 4
#define NTHREADS_STR "4"  // for BLOSC_NTHREADS
#define CHUNK_NBYTES (64 * KB)
#define BLOCK_NBYTES (4 * KB)  // 16 blocks per chunk: enough for the worker threads
#define SCHUNK_NCHUNKS 3
#define SCHUNK_CHUNK_NBYTES (2 * MB)
#define SCHUNK_BLOCK_NBYTES (64 * KB)  // big enough for the parallel slice read of a partial chunk
#define SCHUNK_NBYTES (SCHUNK_NCHUNKS * SCHUNK_CHUNK_NBYTES)

#define CODEC_ID 200             // registered with BLOSC2_CALLING_THREAD_ONLY
#define CODEC_UNFLAGGED_ID 201   // the same callbacks, registered without flags
#define FILTER_ID 210            // registered with BLOSC2_CALLING_THREAD_ONLY
#define FILTER_UNFLAGGED_ID 211  // the same callbacks, registered without flags

int tests_run = 0;

#if defined(_WIN32)
static DWORD calling_thread;
static void set_calling_thread(void) { calling_thread = GetCurrentThreadId(); }
static bool on_calling_thread(void) { return GetCurrentThreadId() == calling_thread; }
static void unset_blosc_nthreads(void) { _putenv_s("BLOSC_NTHREADS", ""); }  // "" removes it
#else
static pthread_t calling_thread;
static void set_calling_thread(void) { calling_thread = pthread_self(); }
static bool on_calling_thread(void) { return pthread_equal(pthread_self(), calling_thread) != 0; }
static void unset_blosc_nthreads(void) { unsetenv("BLOSC_NTHREADS"); }
#endif

/* Callback calls, counted on the calling thread only: on any other thread the
   callbacks fail, which makes the C-Blosc2 call fail */
static int ncalls = 0;

/* Test data: bytes below 16 (see the codec), filled once in main() */
static uint8_t *src;

/* A codec that stores two 4-bit values per byte */
static int nibble_encoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                          uint8_t meta, blosc2_cparams *cparams, const void *chunk) {
  UNUSED(meta);
  UNUSED(cparams);
  UNUSED(chunk);
  if (!on_calling_thread()) {
    return BLOSC2_ERROR_FAILURE;
  }
  ncalls++;
  int32_t nbytes = (input_len + 1) / 2;
  if (nbytes > output_len) {
    return 0;
  }
  for (int32_t i = 0; i < nbytes; i++) {
    uint8_t high = (2 * i + 1 < input_len) ? input[2 * i + 1] : 0;
    output[i] = (uint8_t)((input[2 * i] & 0x0f) | (high << 4));
  }
  return nbytes;
}

static int nibble_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                          uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
  UNUSED(meta);
  UNUSED(dparams);
  UNUSED(chunk);
  if (!on_calling_thread()) {
    return BLOSC2_ERROR_FAILURE;
  }
  ncalls++;
  if (input_len < (output_len + 1) / 2) {
    return BLOSC2_ERROR_READ_BUFFER;
  }
  for (int32_t i = 0; i < output_len; i++) {
    output[i] = (i % 2 == 0) ? (input[i / 2] & 0x0f) : (input[i / 2] >> 4);
  }
  return output_len;
}

/* A filter that reverses the bytes of a block */
static int reverse(const uint8_t *in, uint8_t *out, int32_t size) {
  if (!on_calling_thread()) {
    return BLOSC2_ERROR_FAILURE;
  }
  ncalls++;
  for (int32_t i = 0; i < size; i++) {
    out[i] = in[size - 1 - i];
  }
  return BLOSC2_ERROR_SUCCESS;
}

static int reverse_forward(const uint8_t *in, uint8_t *out, int32_t size, uint8_t meta,
                           blosc2_cparams *cparams, uint8_t id) {
  UNUSED(meta);
  UNUSED(cparams);
  UNUSED(id);
  return reverse(in, out, size);
}

static int reverse_backward(const uint8_t *in, uint8_t *out, int32_t size, uint8_t meta,
                            blosc2_dparams *dparams, uint8_t id) {
  UNUSED(meta);
  UNUSED(dparams);
  UNUSED(id);
  return reverse(in, out, size);
}

static blosc2_codec nibble = {.compcode = CODEC_ID, .compname = "nibble", .complib = CODEC_ID, .version = 1,
                              .encoder = nibble_encoder, .decoder = nibble_decoder};
static blosc2_codec nibble_unflagged = {.compcode = CODEC_UNFLAGGED_ID, .compname = "nibble_unflagged",
                                        .complib = CODEC_UNFLAGGED_ID, .version = 1,
                                        .encoder = nibble_encoder, .decoder = nibble_decoder};
static blosc2_filter reverse_filter = {.id = FILTER_ID, .name = "reverse", .version = 1,
                                       .forward = reverse_forward, .backward = reverse_backward};
static blosc2_filter reverse_unflagged = {.id = FILTER_UNFLAGGED_ID, .name = "reverse_unflagged", .version = 1,
                                          .forward = reverse_forward, .backward = reverse_backward};

/* cparams for `codec` and, if not 0, the user filter `filter` */
static blosc2_cparams make_cparams(uint8_t codec, uint8_t filter, int16_t nthreads, int32_t blocksize) {
  blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
  cparams.compcode = codec;
  cparams.typesize = 1;
  cparams.clevel = 5;
  cparams.nthreads = nthreads;
  cparams.blocksize = blocksize;
  cparams.splitmode = BLOSC_NEVER_SPLIT;
  for (int i = 0; i < BLOSC2_MAX_FILTERS; i++) {
    cparams.filters[i] = BLOSC_NOFILTER;
  }
  cparams.filters[BLOSC2_MAX_FILTERS - 1] = filter;
  return cparams;
}

/* Compress and decompress a chunk with contexts asking for `nthreads` threads
   and getitem from it; returns the first error, 0 if all worked and a callback
   ran. */
static int ctx_roundtrip(uint8_t codec, uint8_t filter, int16_t nthreads) {
  uint8_t chunk[CHUNK_NBYTES + BLOSC2_MAX_OVERHEAD];
  uint8_t dest[CHUNK_NBYTES];
  blosc2_context *cctx = blosc2_create_cctx(make_cparams(codec, filter, nthreads, BLOCK_NBYTES));
  blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
  dparams.nthreads = nthreads;
  blosc2_context *dctx = blosc2_create_dctx(dparams);
  ncalls = 0;

  int cbytes = blosc2_compress_ctx(cctx, src, CHUNK_NBYTES, chunk, sizeof(chunk));
  int rc = cbytes;
  if (rc > 0) {
    rc = blosc2_decompress_ctx(dctx, chunk, cbytes, dest, CHUNK_NBYTES);
  }
  if (rc >= 0) {
    rc = memcmp(src, dest, CHUNK_NBYTES) == 0 ? 0 : BLOSC2_ERROR_DATA;
  }
  if (rc == 0) {
    // getitem across two blocks
    rc = blosc2_getitem_ctx(dctx, chunk, cbytes, BLOCK_NBYTES / 2, BLOCK_NBYTES, dest, CHUNK_NBYTES);
    if (rc >= 0) {
      rc = memcmp(src + BLOCK_NBYTES / 2, dest, BLOCK_NBYTES) == 0 ? 0 : BLOSC2_ERROR_DATA;
    }
  }
  blosc2_free_ctx(cctx);
  blosc2_free_ctx(dctx);
  return rc == 0 && ncalls == 0 ? BLOSC2_ERROR_FAILURE : rc;
}

/* ctx_roundtrip() for a VL-block chunk of CHUNK_NBYTES / BLOCK_NBYTES blocks */
static int vl_roundtrip(uint8_t codec, uint8_t filter, int16_t nthreads) {
  enum { NBLOCKS = CHUNK_NBYTES / BLOCK_NBYTES };
  const void *srcs[NBLOCKS];
  int32_t srcsizes[NBLOCKS];
  void *dests[NBLOCKS] = {0};
  int32_t destsizes[NBLOCKS];
  uint8_t chunk[CHUNK_NBYTES + BLOSC2_MAX_OVERHEAD + 4 * NBLOCKS];
  for (int i = 0; i < NBLOCKS; i++) {
    srcs[i] = src + i * BLOCK_NBYTES;
    srcsizes[i] = BLOCK_NBYTES;
  }
  blosc2_context *cctx = blosc2_create_cctx(make_cparams(codec, filter, nthreads, 0));
  blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
  dparams.nthreads = nthreads;
  blosc2_context *dctx = blosc2_create_dctx(dparams);
  ncalls = 0;
  int rc = blosc2_vlcompress_ctx(cctx, srcs, srcsizes, NBLOCKS, chunk, sizeof(chunk));
  if (rc > 0) {
    rc = blosc2_vldecompress_ctx(dctx, chunk, rc, dests, destsizes, NBLOCKS);
  }
  for (int i = 0; rc == NBLOCKS && i < NBLOCKS; i++) {
    if (destsizes[i] != BLOCK_NBYTES || memcmp(dests[i], srcs[i], BLOCK_NBYTES) != 0) {
      rc = BLOSC2_ERROR_DATA;
    }
  }
  for (int i = 0; i < NBLOCKS; i++) {
    free(dests[i]);
  }
  blosc2_free_ctx(cctx);
  blosc2_free_ctx(dctx);
  if (rc < 0) {
    return rc;
  }
  return rc != NBLOCKS ? BLOSC2_ERROR_DATA : (ncalls == 0 ? BLOSC2_ERROR_FAILURE : 0);
}

/* Decompress a chunk of `codec` and `filter`, compressed on one thread, with
   blosc2_decompress() (the global context) and blosc2_set_nthreads(nthreads);
   returns the first error, 0 if it worked.  User codecs cannot compress there
   (blosc1_set_compressor() refuses them). */
static int global_decompress(uint8_t codec, uint8_t filter, int16_t nthreads) {
  uint8_t chunk[CHUNK_NBYTES + BLOSC2_MAX_OVERHEAD];
  uint8_t dest[CHUNK_NBYTES];
  blosc2_context *cctx = blosc2_create_cctx(make_cparams(codec, filter, 1, BLOCK_NBYTES));
  int rc = blosc2_compress_ctx(cctx, src, CHUNK_NBYTES, chunk, sizeof(chunk));
  blosc2_free_ctx(cctx);
  int16_t old_nthreads = blosc2_set_nthreads(nthreads);
  if (rc > 0) {
    rc = blosc2_decompress(chunk, rc, dest, CHUNK_NBYTES);
  }
  blosc2_set_nthreads(old_nthreads);
  if (rc >= 0) {
    rc = memcmp(src, dest, CHUNK_NBYTES) == 0 ? 0 : BLOSC2_ERROR_DATA;
  }
  return rc;
}

/* A super-chunk of SCHUNK_NCHUNKS chunks of `src` with contexts asking for `nthreads` threads */
static blosc2_schunk *new_schunk(uint8_t codec, uint8_t filter, int16_t nthreads) {
  blosc2_cparams cparams = make_cparams(codec, filter, nthreads, SCHUNK_BLOCK_NBYTES);
  blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
  dparams.nthreads = nthreads;
  blosc2_storage storage = {.cparams = &cparams, .dparams = &dparams};
  blosc2_schunk *schunk = blosc2_schunk_new(&storage);
  for (int i = 0; schunk != NULL && i < SCHUNK_NCHUNKS; i++) {
    if (blosc2_schunk_append_buffer(schunk, src + i * SCHUNK_CHUNK_NBYTES, SCHUNK_CHUNK_NBYTES) != i + 1) {
      blosc2_schunk_free(schunk);
      schunk = NULL;
    }
  }
  return schunk;
}

/* Reads of a super-chunk made by new_schunk(): whole chunks, a slice (parallel
   masked reads of partial chunks) and sparse items (a worker per thread) */
static bool schunk_reads(blosc2_schunk *schunk) {
  uint8_t *dest = malloc(SCHUNK_NBYTES);
  bool ok = true;
  for (int i = 0; ok && i < SCHUNK_NCHUNKS; i++) {
    ok = blosc2_schunk_decompress_chunk(schunk, i, dest, SCHUNK_CHUNK_NBYTES) == SCHUNK_CHUNK_NBYTES &&
         memcmp(dest, src + i * SCHUNK_CHUNK_NBYTES, SCHUNK_CHUNK_NBYTES) == 0;
  }
  // All of the second chunk but its first 100 bytes, and the first half of the third
  int64_t start = SCHUNK_CHUNK_NBYTES + 100;
  int64_t stop = 2 * SCHUNK_CHUNK_NBYTES + SCHUNK_CHUNK_NBYTES / 2;
  if (ok) {
    ok = blosc2_schunk_get_slice_buffer(schunk, start, stop, dest) == 0 &&
         memcmp(dest, src + start, (size_t)(stop - start)) == 0;
  }
  int64_t coords[64];
  uint8_t items[64];
  for (int i = 0; i < 64; i++) {
    coords[i] = ((int64_t)i * 104729) % SCHUNK_NBYTES;
  }
  if (ok) {
    ok = blosc2_schunk_get_sparse_buffer(schunk, 64, coords, items) == 0;
    for (int i = 0; ok && i < 64; i++) {
      ok = items[i] == src[coords[i]];
    }
  }
  free(dest);
  return ok;
}

static char *check_schunk(uint8_t codec, uint8_t filter, int16_t nthreads) {
  ncalls = 0;
  blosc2_schunk *schunk = new_schunk(codec, filter, nthreads);
  bool ok = schunk != NULL && schunk_reads(schunk);
  if (schunk != NULL) {
    blosc2_schunk_free(schunk);
  }
  mu_assert("ERROR: super-chunk round trip", ok);
  mu_assert("ERROR: the callbacks were not called", ncalls > 0);
  return 0;
}

/* The unflagged twins must fail with worker threads, or the checks of the
   flagged ones prove nothing */
static char *test_unflagged_run_on_workers(void) {
  mu_assert("ERROR: unflagged codec ran on the calling thread only",
            ctx_roundtrip(CODEC_UNFLAGGED_ID, 0, NTHREADS) < 0);
  mu_assert("ERROR: unflagged filter ran on the calling thread only",
            ctx_roundtrip(BLOSC_BLOSCLZ, FILTER_UNFLAGGED_ID, NTHREADS) < 0);
  mu_assert("ERROR: unflagged codec ran on the calling thread only in the global context",
            global_decompress(CODEC_UNFLAGGED_ID, 0, NTHREADS) < 0);
  mu_assert("ERROR: unflagged codec ran on the calling thread only in VL blocks",
            vl_roundtrip(CODEC_UNFLAGGED_ID, 0, NTHREADS) < 0);
  mu_assert("ERROR: unflagged codec failed with one thread", ctx_roundtrip(CODEC_UNFLAGGED_ID, 0, 1) == 0);
  mu_assert("ERROR: unflagged codec failed with one thread in VL blocks", vl_roundtrip(CODEC_UNFLAGGED_ID, 0, 1) == 0);
  return 0;
}

static char *test_contexts(void) {
  mu_assert("ERROR: flagged codec", ctx_roundtrip(CODEC_ID, 0, NTHREADS) == 0);
  mu_assert("ERROR: flagged filter", ctx_roundtrip(BLOSC_BLOSCLZ, FILTER_ID, NTHREADS) == 0);
  // Either of the two in a chunk is enough
  mu_assert("ERROR: flagged codec + unflagged filter", ctx_roundtrip(CODEC_ID, FILTER_UNFLAGGED_ID, NTHREADS) == 0);
  mu_assert("ERROR: unflagged codec + flagged filter", ctx_roundtrip(CODEC_UNFLAGGED_ID, FILTER_ID, NTHREADS) == 0);
  mu_assert("ERROR: flagged codec in VL blocks", vl_roundtrip(CODEC_ID, 0, NTHREADS) == 0);
  mu_assert("ERROR: flagged filter in VL blocks", vl_roundtrip(BLOSC_BLOSCLZ, FILTER_ID, NTHREADS) == 0);
  return 0;
}

static char *test_global_context(void) {
  mu_assert("ERROR: flagged codec in the global context", global_decompress(CODEC_ID, 0, NTHREADS) == 0);
  mu_assert("ERROR: flagged filter in the global context", global_decompress(BLOSC_BLOSCLZ, FILTER_ID, NTHREADS) == 0);
  return 0;
}

static char *test_schunks(void) {
  char *msg = check_schunk(CODEC_ID, 0, NTHREADS);
  return msg != NULL ? msg : check_schunk(BLOSC_LZ4, FILTER_ID, NTHREADS);
}

/* A super-chunk made from a frame gets the global thread count */
static char *test_schunk_from_frame(void) {
  blosc2_schunk *schunk = new_schunk(CODEC_ID, FILTER_ID, 1);
  uint8_t *frame = NULL;
  bool needs_free = false;
  int64_t frame_len = schunk != NULL ? blosc2_schunk_to_buffer(schunk, &frame, &needs_free) : -1;
  int16_t old_nthreads = blosc2_set_nthreads(NTHREADS);
  blosc2_schunk *copy = frame_len > 0 ? blosc2_schunk_from_buffer(frame, frame_len, true) : NULL;
  blosc2_dparams *dparams = NULL;
  bool ok = copy != NULL && blosc2_schunk_get_dparams(copy, &dparams) == 0 && dparams->nthreads == NTHREADS;
  ncalls = 0;
  ok = ok && schunk_reads(copy) && ncalls > 0;
  blosc2_set_nthreads(old_nthreads);
  free(dparams);
  if (copy != NULL) {
    blosc2_schunk_free(copy);
  }
  if (needs_free) {
    free(frame);
  }
  if (schunk != NULL) {
    blosc2_schunk_free(schunk);
  }
  mu_assert("ERROR: super-chunk from a frame with the global thread count", ok);
  return 0;
}

/* BLOSC_NTHREADS replaces the nthreads of every new context, an explicit 1 included */
static char *test_blosc_nthreads_env(void) {
  char *msg = NULL;
  setenv("BLOSC_NTHREADS", NTHREADS_STR, 1);
  if (ctx_roundtrip(CODEC_UNFLAGGED_ID, 0, 1) >= 0) {
    msg = "ERROR: unflagged codec ran on the calling thread only with BLOSC_NTHREADS";
  }
  else if (ctx_roundtrip(CODEC_ID, 0, 1) != 0) {
    msg = "ERROR: flagged codec with BLOSC_NTHREADS";
  }
  else if (ctx_roundtrip(BLOSC_BLOSCLZ, FILTER_ID, 1) != 0) {
    msg = "ERROR: flagged filter with BLOSC_NTHREADS";
  }
  else {
    msg = check_schunk(CODEC_ID, FILTER_ID, 1);
  }
  unset_blosc_nthreads();
  return msg;
}

static char *test_register_flags(void) {
  mu_assert("ERROR: unknown codec flags accepted", blosc2_register_codec_flags(&nibble, 2) < 0);
  mu_assert("ERROR: negative codec flags accepted", blosc2_register_codec_flags(&nibble, -1) < 0);
  mu_assert("ERROR: NULL codec accepted", blosc2_register_codec_flags(NULL, 0) < 0);
  mu_assert("ERROR: unknown filter flags accepted", blosc2_register_filter_flags(&reverse_filter, 2) < 0);
  mu_assert("ERROR: NULL filter accepted", blosc2_register_filter_flags(NULL, 0) < 0);
  // Registering again under the same name without flags keeps the flag
  mu_assert("ERROR: re-registering the codec", blosc2_register_codec(&nibble) == 0);
  mu_assert("ERROR: re-registering the filter", blosc2_register_filter(&reverse_filter) == 0);
  mu_assert("ERROR: re-registering dropped the flags", ctx_roundtrip(CODEC_ID, FILTER_ID, NTHREADS) == 0);
  // Registering again under the same name with the flag adds it
  mu_assert("ERROR: re-registering with flags",
            blosc2_register_codec_flags(&nibble_unflagged, BLOSC2_CALLING_THREAD_ONLY) == 0);
  mu_assert("ERROR: re-registering did not add the flags", ctx_roundtrip(CODEC_UNFLAGGED_ID, 0, NTHREADS) == 0);
  return 0;
}

static char *all_tests(void) {
  mu_run_test(test_unflagged_run_on_workers);
  mu_run_test(test_contexts);
  mu_run_test(test_global_context);
  mu_run_test(test_schunks);
  mu_run_test(test_schunk_from_frame);
  mu_run_test(test_blosc_nthreads_env);
  mu_run_test(test_register_flags);  // last: it flags the unflagged codec
  return 0;
}

int main(void) {
  // The tests choose the thread counts themselves, test_blosc_nthreads_env() included
  unset_blosc_nthreads();
  blosc2_init();
  set_calling_thread();
  src = malloc(SCHUNK_NBYTES);
  for (int32_t i = 0; i < SCHUNK_NBYTES; i++) {
    src[i] = (uint8_t)((i * 7 + i / 1000) % 16);
  }
  if (blosc2_register_codec_flags(&nibble, BLOSC2_CALLING_THREAD_ONLY) < 0 ||
      blosc2_register_codec(&nibble_unflagged) < 0 ||
      blosc2_register_filter_flags(&reverse_filter, BLOSC2_CALLING_THREAD_ONLY) < 0 ||
      blosc2_register_filter(&reverse_unflagged) < 0) {
    printf("Cannot register the codecs and filters\n");
    return 1;
  }

  char *result = all_tests();
  if (result != 0) {
    printf(" (%s)\n", result);
  }
  else {
    printf(" ALL TESTS PASSED");
  }
  printf("\tTests run: %d\n", tests_run);

  free(src);
  blosc2_destroy();
  return result != 0;
}
