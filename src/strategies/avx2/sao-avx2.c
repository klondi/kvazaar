/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include "strategies/avx2/sao-avx2.h"

#if COMPILE_INTEL_AVX2
#include <immintrin.h>

#include "cu.h"
#include "encoder.h"
#include "encoderstate.h"
#include "kvazaar.h"
#include "sao.h"
#include "strategyselector.h"


// These optimizations are based heavily on sao-generic.c.
// Might be useful to check that if (when) this file
// is difficult to understand.

static INLINE __m128i load_6_pixels(const kvz_pixel* data)
{
  return _mm_insert_epi16(_mm_cvtsi32_si128(*(int32_t*)&(data[0])), *(int16_t*)&(data[4]), 2);
}

static INLINE __m256i load_5_offsets(const int* offsets)
{
 return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((__m128i*) offsets)), _mm_insert_epi32(_mm_setzero_si128(), offsets[4], 0), 1);
}


static __m128i sao_calc_eo_cat_avx2_256(__m128i* a, __m128i* b, __m128i* c)
{
 __m256i v_eo_idx = _mm256_set1_epi16(2);

 __m256i v_a = _mm256_cvtepu8_epi16(*a);
 __m256i v_c = _mm256_cvtepu8_epi16(*c);
 __m256i v_b = _mm256_cvtepu8_epi16(*b);

 __m256i temp_a = _mm256_sign_epi16(_mm256_set1_epi16(1), _mm256_sub_epi16(v_c, v_a));
 __m256i temp_b = _mm256_sign_epi16(_mm256_set1_epi16(1), _mm256_sub_epi16(v_c, v_b));
 v_eo_idx = _mm256_add_epi16(v_eo_idx, temp_a);
 v_eo_idx = _mm256_add_epi16(v_eo_idx, temp_b);

 v_eo_idx = _mm256_packus_epi16(v_eo_idx, v_eo_idx);
 v_eo_idx = _mm256_permute4x64_epi64(v_eo_idx, 0b11011000);

 __m128i v_cat_lookup = _mm_setr_epi8(1, 2, 0, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
 __m128i v_cat = _mm_shuffle_epi8(v_cat_lookup, _mm256_extracti128_si256(v_eo_idx, 1));

 return v_cat;
}

static __m128i sao_calc_eo_cat_avx2(__m128i* a, __m128i* b, __m128i* c)
{
  __m128i v_eo_idx = _mm_set1_epi16(2);

  __m128i v_a = _mm_cvtepu8_epi16(*a);
  __m128i v_c = _mm_cvtepu8_epi16(*c);
  __m128i v_b = _mm_cvtepu8_epi16(*b);

  __m128i temp_a = _mm_sign_epi16(_mm_set1_epi16(1), _mm_sub_epi16(v_c, v_a));
  __m128i temp_b = _mm_sign_epi16(_mm_set1_epi16(1), _mm_sub_epi16(v_c, v_b));
  v_eo_idx = _mm_add_epi16(v_eo_idx, temp_a);
  v_eo_idx = _mm_add_epi16(v_eo_idx, temp_b);

  v_eo_idx = _mm_packus_epi16(v_eo_idx, v_eo_idx);
  
  __m128i v_cat_lookup = _mm_setr_epi8(1,2,0,3,4,0,0,0,0,0,0,0,0,0,0,0);
  __m128i v_cat = _mm_shuffle_epi8(v_cat_lookup, v_eo_idx);

  return v_cat;
}

static int sao_edge_ddistortion_avx2(const kvz_pixel *orig_data,
                                     const kvz_pixel *rec_data,
                                     int block_width,
                                     int block_height,
                                     int eo_class,
                                     int offsets[NUM_SAO_EDGE_CATEGORIES])


{
 int y, x;
  int sum = 0;
  vector2d_t a_ofs = g_sao_edge_offsets[eo_class][0];
  vector2d_t b_ofs = g_sao_edge_offsets[eo_class][1];

  __m256i v_accum = { 0 };
  __m256i v_offset = _mm256_loadu_si256((__m256i*) offsets);
  __m256i temp_v_offset;
  __m128i vector_c_data;
  __m128i vector_a, vector_b;


  for (y = 1; y < block_height - 1; ++y) {

   for (x = 1; x < block_width - 16; x += 16) {

    const kvz_pixel *c_data = &rec_data[y * block_width + x];

    vector_c_data = _mm_loadu_si128((__m128i*)c_data);
    vector_a = _mm_loadu_si128((__m128i*)(&c_data[a_ofs.y * block_width + a_ofs.x]));

    vector_b = _mm_loadu_si128((__m128i*)(&c_data[b_ofs.y * block_width + b_ofs.x]));
    __m128i temp_cat = sao_calc_eo_cat_avx2_256(&vector_a, &vector_b, &vector_c_data);

    //Split temp_cat vector to higher and upper parts
    __m256i vector_cat_lower = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(_mm_extract_epi64(temp_cat, 0)));
    __m256i vector_cat_upper = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(_mm_extract_epi64(temp_cat, 1)));

    // Re-arrenge offsets to right order
    temp_v_offset = _mm256_permutevar8x32_epi32(v_offset, vector_cat_lower);
    __m256i v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(orig_data[y * block_width + x])));

    v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(vector_c_data));

    __m256i v_diff_minus_offset = _mm256_sub_epi32(v_diff, temp_v_offset);

    __m256i v_temp_sum = _mm256_sub_epi32(_mm256_mullo_epi32(v_diff_minus_offset, v_diff_minus_offset), _mm256_mullo_epi32(v_diff, v_diff));
    v_accum = _mm256_add_epi32(v_accum, v_temp_sum);

    temp_v_offset = _mm256_permutevar8x32_epi32(v_offset, vector_cat_upper);
    v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(orig_data[y * block_width + x + 8])));
    int64_t*c_pointer = (int64_t*)&vector_c_data;
    v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(c_pointer[1])));

    v_diff_minus_offset = _mm256_sub_epi32(v_diff, temp_v_offset);

    v_temp_sum = _mm256_sub_epi32(_mm256_mullo_epi32(v_diff_minus_offset, v_diff_minus_offset), _mm256_mullo_epi32(v_diff, v_diff));
    v_accum = _mm256_add_epi32(v_accum, v_temp_sum);
   }
   /*

   Make 14 pixel load here

   static INLINE __m128i load_14_pixels(const kvz_pixel* data) {
    __m128i temp;
    temp = _mm_loadl_epi64((__m128i*)data);
    _mm_insert_epi32((int32_t*)&)
     *
   }*/


   // After x> (block_width-16) handle 8 pixels and after that the last 6 pixels
   const kvz_pixel *c_data = &rec_data[y * block_width + x];

   vector_c_data = _mm_loadl_epi64((__m128i*)c_data);
   vector_a = _mm_loadl_epi64((__m128i*)(&c_data[a_ofs.y * block_width + a_ofs.x]));
   vector_b = _mm_loadl_epi64((__m128i*)(&c_data[b_ofs.y * block_width + b_ofs.x]));

   __m256i v_cat = _mm256_cvtepu8_epi32(sao_calc_eo_cat_avx2_256(&vector_a, &vector_b, &vector_c_data));

   temp_v_offset = _mm256_permutevar8x32_epi32(v_offset, v_cat);


   __m256i v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(orig_data[y * block_width + x])));

   v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(vector_c_data));

   __m256i v_diff_minus_offset = _mm256_sub_epi32(v_diff, temp_v_offset);

   __m256i v_temp_sum = _mm256_sub_epi32(_mm256_mullo_epi32(v_diff_minus_offset, v_diff_minus_offset), _mm256_mullo_epi32(v_diff, v_diff));
   v_accum = _mm256_add_epi32(v_accum, v_temp_sum);

   //Handle last 6 pixels separately to prevent reading over boundary
   x += 8;
   c_data = &rec_data[y * block_width + x];
   vector_c_data = load_6_pixels(c_data);
   const kvz_pixel *a_ptr = &c_data[a_ofs.y * block_width + a_ofs.x];
   const kvz_pixel *b_ptr = &c_data[b_ofs.y * block_width + b_ofs.x];
   vector_a = load_6_pixels(a_ptr);
   vector_b = load_6_pixels(b_ptr);

   v_cat = _mm256_cvtepu8_epi32(sao_calc_eo_cat_avx2_256(&vector_a, &vector_b, &vector_c_data));

   temp_v_offset = _mm256_permutevar8x32_epi32(v_offset, v_cat);

   const kvz_pixel *orig_ptr = &(orig_data[y * block_width + x]);
   v_diff = _mm256_cvtepu8_epi32(load_6_pixels(orig_ptr));

   v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(vector_c_data));


   v_diff_minus_offset = _mm256_sub_epi32(v_diff, temp_v_offset);
   v_temp_sum = _mm256_sub_epi32(_mm256_mullo_epi32(v_diff_minus_offset, v_diff_minus_offset), _mm256_mullo_epi32(v_diff, v_diff));
   v_accum = _mm256_add_epi32(v_accum, v_temp_sum);
  }

  //Full horizontal sum
  v_accum = _mm256_hadd_epi32(v_accum, v_accum);
  v_accum = _mm256_hadd_epi32(v_accum, v_accum);
  int32_t* pointer = (int32_t*)&v_accum;
  sum += pointer[0] + pointer[4];

  return sum;
}

/*
// Mapping of edge_idx values to eo-classes.
static int sao_calc_eo_cat(kvz_pixel a, kvz_pixel b, kvz_pixel c)
{
// Mapping relationships between a, b and c to eo_idx.
static const int sao_eo_idx_to_eo_category[] = { 1, 2, 0, 3, 4 };

int eo_idx = 2 + SIGN3((int)c - (int)a) + SIGN3((int)c - (int)b);

return sao_eo_idx_to_eo_category[eo_idx];
}
*/

static INLINE void accum_count_eo_cat_avx2(__m256i*  __restrict v_diff_accum,
                                           __m256i* __restrict v_count,
                                           __m256i* __restrict v_cat,
                                           __m256i* __restrict v_diff,
                                           int eo_cat)
{
        __m256i v_mask = _mm256_cmpeq_epi32(*v_cat, _mm256_set1_epi32(eo_cat));
        *v_diff_accum = _mm256_add_epi32(*v_diff_accum, _mm256_and_si256(*v_diff, v_mask));
        *v_count = _mm256_sub_epi32(*v_count, v_mask);
}


#define ACCUM_COUNT_EO_CAT_AVX2(EO_CAT, V_CAT) \
  \
  accum_count_eo_cat_avx2(&(v_diff_accum[ EO_CAT ]), &(v_count[ EO_CAT ]), &V_CAT , &v_diff, EO_CAT);


/*
static void calc_sao_edge_dir_generic(const kvz_pixel *orig_data,
                                      const kvz_pixel *rec_data,
                                      int eo_class,
                                      int block_width,
                                      int block_height,
                                      int cat_sum_cnt[2][NUM_SAO_EDGE_CATEGORIES])
{
  int y, x;
  vector2d_t a_ofs = g_sao_edge_offsets[eo_class][0];
  vector2d_t b_ofs = g_sao_edge_offsets[eo_class][1];
  // Arrays orig_data and rec_data are quarter size for chroma.

  // Don't sample the edge pixels because this function doesn't have access to
  // their neighbours.
  for (y = 1; y < block_height - 1; ++y) {
    for (x = 1; x < block_width - 1; ++x) {
      const kvz_pixel *c_data = &rec_data[y * block_width + x];
      kvz_pixel a = c_data[a_ofs.y * block_width + a_ofs.x];
      kvz_pixel c = c_data[0];
      kvz_pixel b = c_data[b_ofs.y * block_width + b_ofs.x];

      int eo_cat = sao_calc_eo_cat(a, b, c);

      cat_sum_cnt[0][eo_cat] += orig_data[y * block_width + x] - c;
      cat_sum_cnt[1][eo_cat] += 1;
    }
  }
}*/

static void calc_sao_edge_dir_avx2(const kvz_pixel *orig_data,
 const kvz_pixel *rec_data,
 int eo_class,
 int block_width,
 int block_height,
 int cat_sum_cnt[2][NUM_SAO_EDGE_CATEGORIES])
{
 int y, x;
 vector2d_t a_ofs = g_sao_edge_offsets[eo_class][0];
 vector2d_t b_ofs = g_sao_edge_offsets[eo_class][1];

 __m256i v_diff_accum[NUM_SAO_EDGE_CATEGORIES] = { { 0 } };
 __m256i v_count[NUM_SAO_EDGE_CATEGORIES] = { { 0 } };
 // Arrays orig_data and rec_data are quarter size for chroma.

 // Don't sample the edge pixels because this function doesn't have access to
 // their neighbours.

 __m128i temp_cat;
 for (y = 1; y < block_height - 1; ++y) {
  for (x = 1; x < block_width - 16; x+=16) {

   const kvz_pixel *c_data = &rec_data[y * block_width + x];

   __m128i v_c_data = _mm_loadu_si128((__m128i*)c_data);
   __m128i v_a = _mm_loadu_si128((__m128i*)(&c_data[a_ofs.y * block_width + a_ofs.x]));
   __m128i v_c = v_c_data;
   __m128i v_b = _mm_loadu_si128((__m128i*)(&c_data[b_ofs.y * block_width + b_ofs.x]));

   temp_cat = sao_calc_eo_cat_avx2_256(&v_a, &v_b, &v_c);
   __m256i cat_lower = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(_mm_extract_epi64(temp_cat, 0)));
   __m256i cat_upper = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(_mm_extract_epi64(temp_cat, 1)));

   __m256i v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i* __restrict)&(orig_data[y * block_width + x])));
   v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(v_c));

   //Accumulate differences and occurrences for each category
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT0, cat_lower);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT1, cat_lower);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT2, cat_lower);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT3, cat_lower);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT4, cat_lower);


   v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i* __restrict)&(orig_data[y * block_width + x+8])));
   int64_t*c_pointer = (int64_t*)&v_c;
   v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(_mm_cvtsi64_si128(c_pointer[1])));

   //Accumulate differences and occurrences for each category
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT0, cat_upper);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT1, cat_upper);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT2, cat_upper);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT3, cat_upper);
   ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT4, cat_upper);
  }

  const kvz_pixel *c_data = &rec_data[y * block_width + x];

  __m128i v_c_data = _mm_loadl_epi64((__m128i* __restrict)c_data);
  __m128i v_a = _mm_loadl_epi64((__m128i* __restrict)(&c_data[a_ofs.y * block_width + a_ofs.x]));
  __m128i v_c = v_c_data;
  __m128i v_b = _mm_loadl_epi64((__m128i* __restrict)(&c_data[b_ofs.y * block_width + b_ofs.x]));

  __m256i v_cat = _mm256_cvtepu8_epi32(sao_calc_eo_cat_avx2(&v_a, &v_b, &v_c));

  __m256i v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i* __restrict)&(orig_data[y * block_width + x])));
  v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(v_c));

  //Accumulate differences and occurrences for each category
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT0, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT1, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT2, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT3, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT4, v_cat);

  x += 8;

  //Handle last 6 pixels separately to prevent reading over boundary
  c_data = &rec_data[y * block_width + x];
  v_c_data = load_6_pixels(c_data);
  const kvz_pixel* a_ptr = &c_data[a_ofs.y * block_width + a_ofs.x];
  const kvz_pixel* b_ptr = &c_data[b_ofs.y * block_width + b_ofs.x];
  v_a = load_6_pixels(a_ptr);
  v_c = v_c_data;
  v_b = load_6_pixels(b_ptr);

  v_cat = _mm256_cvtepu8_epi32(sao_calc_eo_cat_avx2(&v_a, &v_b, &v_c));

  //Set the last two elements to a non-existing category to cause
  //the accumulate-count macro to discard those values.
  __m256i v_mask = _mm256_setr_epi32(0, 0, 0, 0, 0, 0, -1, -1);
  v_cat = _mm256_or_si256(v_cat, v_mask);

  const kvz_pixel* orig_ptr = &(orig_data[y * block_width + x]);
  v_diff = _mm256_cvtepu8_epi32(load_6_pixels(orig_ptr));
  v_diff = _mm256_sub_epi32(v_diff, _mm256_cvtepu8_epi32(v_c));

  //Accumulate differences and occurrences for each category
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT0, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT1, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT2, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT3, v_cat);
  ACCUM_COUNT_EO_CAT_AVX2(SAO_EO_CAT4, v_cat);
 }

 for (int eo_cat = 0; eo_cat < NUM_SAO_EDGE_CATEGORIES; ++eo_cat) {
  int accum = 0;
  int count = 0;

  //Full horizontal sum of accumulated values

  v_diff_accum[eo_cat] = _mm256_hadd_epi32(v_diff_accum[eo_cat], v_diff_accum[eo_cat]);
  v_diff_accum[eo_cat] = _mm256_hadd_epi32(v_diff_accum[eo_cat], v_diff_accum[eo_cat]);
  accum += _mm256_extract_epi32(v_diff_accum[eo_cat], 0) + _mm256_extract_epi32(v_diff_accum[eo_cat], 4);


  //Full horizontal sum of accumulated values
  v_count[eo_cat] = _mm256_hadd_epi32(v_count[eo_cat], v_count[eo_cat]);
  v_count[eo_cat] = _mm256_hadd_epi32(v_count[eo_cat], v_count[eo_cat]);
  count += _mm256_extract_epi32(v_count[eo_cat], 0) + _mm256_extract_epi32(v_count[eo_cat], 4);


  cat_sum_cnt[0][eo_cat] += accum;
  cat_sum_cnt[1][eo_cat] += count;

 }

}

static void sao_reconstruct_color_avx2(const encoder_control_t * const encoder,
                                       const kvz_pixel *rec_data, kvz_pixel *new_rec_data,
                                       const sao_info_t *sao,
                                       int stride, int new_stride,
                                       int block_width, int block_height,
                                       color_t color_i)
{
  // Arrays orig_data and rec_data are quarter size for chroma.
  int offset_v = color_i == COLOR_V ? 5 : 0;

  if (sao->type == SAO_TYPE_BAND) {
    int offsets[1 << KVZ_BIT_DEPTH];
    kvz_calc_sao_offset_array(encoder, sao, offsets, color_i);
    for (int y = 0; y < block_height; ++y) {
      for (int x = 0; x < block_width; ++x) {
        new_rec_data[y * new_stride + x] = offsets[rec_data[y * stride + x]];
      }
    }
  } else {
    // Don't sample the edge pixels because this function doesn't have access to
    // their neighbours.
    for (int y = 0; y < block_height; ++y) {
      for (int x = 0; x < block_width; x+=8) {
        vector2d_t a_ofs = g_sao_edge_offsets[sao->eo_class][0];
        vector2d_t b_ofs = g_sao_edge_offsets[sao->eo_class][1];
        const kvz_pixel *c_data = &rec_data[y * stride + x];
        kvz_pixel *new_data = &new_rec_data[y * new_stride + x];
        const kvz_pixel* a_ptr = &c_data[a_ofs.y * stride + a_ofs.x];
        const kvz_pixel* c_ptr = &c_data[0];
        const kvz_pixel* b_ptr = &c_data[b_ofs.y * stride + b_ofs.x];

        __m128i v_a = _mm_loadl_epi64((__m128i*)a_ptr);
        __m128i v_b = _mm_loadl_epi64((__m128i*)b_ptr);
        __m128i v_c = _mm_loadl_epi64((__m128i*)c_ptr);

        __m256i v_cat = _mm256_cvtepu8_epi32(sao_calc_eo_cat_avx2(&v_a, &v_b, &v_c) );

        __m256i v_offset_v = load_5_offsets(sao->offsets + offset_v);
        __m256i v_new_data = _mm256_permutevar8x32_epi32(v_offset_v, v_cat);
        v_new_data = _mm256_add_epi32(v_new_data, _mm256_cvtepu8_epi32(v_c));
        __m128i v_new_data_128 = _mm_packus_epi32(_mm256_castsi256_si128(v_new_data), _mm256_extracti128_si256(v_new_data, 1));
        v_new_data_128 = _mm_packus_epi16(v_new_data_128, v_new_data_128);
        
        if ((block_width - x) >= 8) {
          _mm_storel_epi64((__m128i*)new_data, v_new_data_128);
        } else {
          
          kvz_pixel arr[8];
          _mm_storel_epi64((__m128i*)arr, v_new_data_128);
          for (int i = 0; i < block_width - x; ++i) new_data[i] = arr[i];
        }
      
      }
    }
  }
}


static int sao_band_ddistortion_avx2(const encoder_state_t * const state,
                                     const kvz_pixel *orig_data,
                                     const kvz_pixel *rec_data,
                                     int block_width,
                                     int block_height,
                                     int band_pos,
                                     int sao_bands[4])
{
  int y, x;
  int shift = state->encoder_control->bitdepth-5;
  int sum = 0;


  __m256i v_accum = { 0 };

  for (y = 0; y < block_height; ++y) {
    for (x = 0; x < block_width; x+=8) {
      
      __m256i v_band = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(rec_data[y * block_width + x])));
      v_band = _mm256_srli_epi32(v_band, shift);
      v_band = _mm256_sub_epi32(v_band, _mm256_set1_epi32(band_pos));

      __m256i v_offset = { 0 };
      __m256i v_mask = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(~3), v_band), _mm256_setzero_si256());
      v_offset = _mm256_permutevar8x32_epi32(_mm256_castsi128_si256(_mm_loadu_si128((__m128i*)sao_bands)), v_band);

      v_offset = _mm256_and_si256(v_offset, v_mask);
      

      __m256i v_diff = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(orig_data[y * block_width + x])));
      __m256i v_rec = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(rec_data[y * block_width + x])));
      v_diff = _mm256_sub_epi32(v_diff, v_rec);
      __m256i v_diff_minus_offset = _mm256_sub_epi32(v_diff, v_offset);
      __m256i v_temp_sum = _mm256_sub_epi32(_mm256_mullo_epi32(v_diff_minus_offset, v_diff_minus_offset), _mm256_mullo_epi32(v_diff, v_diff));
      v_accum = _mm256_add_epi32(v_accum, v_temp_sum);
    }
  }

  //Full horizontal sum
  v_accum = _mm256_hadd_epi32(v_accum, v_accum);
  v_accum = _mm256_hadd_epi32(v_accum, v_accum);
  sum += _mm256_extract_epi32(v_accum, 0) + _mm256_extract_epi32(v_accum, 4);

  return sum;
}

#endif //COMPILE_INTEL_AVX2

int kvz_strategy_register_sao_avx2(void* opaque, uint8_t bitdepth)
{
  bool success = true;
#if COMPILE_INTEL_AVX2
  if (bitdepth == 8) {
    success &= kvz_strategyselector_register(opaque, "sao_edge_ddistortion", "avx2", 40, &sao_edge_ddistortion_avx2);
    success &= kvz_strategyselector_register(opaque, "calc_sao_edge_dir", "avx2", 40, &calc_sao_edge_dir_avx2);
    success &= kvz_strategyselector_register(opaque, "sao_reconstruct_color", "avx2", 40, &sao_reconstruct_color_avx2);
    success &= kvz_strategyselector_register(opaque, "sao_band_ddistortion", "avx2", 40, &sao_band_ddistortion_avx2);
  }
#endif //COMPILE_INTEL_AVX2
  return success;
}
