#include <ccv.h>
#include <ccv_internal.h>
#include <nnc/ccv_nnc.h>
#include <nnc/ccv_nnc_easy.h>
#include <nnc/ccv_nnc_internal.h>
#if defined(HAVE_SSE2)
#include <xmmintrin.h>
#elif defined(HAVE_NEON)
#include <arm_neon.h>
#endif
#ifdef USE_OPENMP
#include <omp.h>
#endif
#ifdef USE_DISPATCH
#include <dispatch/dispatch.h>
#endif
#include "ccv_nnc_cmd_conv_opt.h"

#define set_n_m_dim(i, x, wd, ad) \
	do { \
		n[x] = ccv_max((i) * hint.stride.dim[x + 1] - hint.border.begin[x + 1], 0) - ((i) * hint.stride.dim[x + 1] - hint.border.begin[x + 1]); \
		m[x] = wd[x + 1] - n[x] - ((i) * hint.stride.dim[x + 1] - hint.border.begin[x + 1] + wd[x + 1] - ccv_min(ad[x + 1], (i) * hint.stride.dim[x + 1] - hint.border.begin[x + 1] + wd[x + 1])); \
	} while (0)

#ifdef HAVE_SSE2
inline static void _ccv_nnc_x4w_sse2(const float* const w, const int* const dim, float* x4w)
{
	int jump_dim = dim[3] / 4;
	parallel_for(k, jump_dim) {
		int i, j;
		float* x4wz = x4w + k * dim[2] * dim[1] * dim[0] * 4;
		const float* wz[] = {
			w + (k * 4) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 1) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 2) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 3) * dim[2] * dim[1] * dim[0],
		};
		for (i = 0; i < dim[2] * dim[1]; i++)
		{
			for (j = 0; j < dim[0]; j++)
			{
				x4wz[j * 4] = wz[0][j];
				x4wz[j * 4 + 1] = wz[1][j];
				x4wz[j * 4 + 2] = wz[2][j];
				x4wz[j * 4 + 3] = wz[3][j];
			}
			x4wz += dim[0] * 4;
			wz[0] += dim[0];
			wz[1] += dim[0];
			wz[2] += dim[0];
			wz[3] += dim[0];
		}
	} parallel_endfor
}

static int _ccv_nnc_conv_forw_sse2(const ccv_nnc_tensor_view_t* a, const ccv_nnc_tensor_t* w, const ccv_nnc_tensor_t* bias, const ccv_nnc_hint_t hint, ccv_nnc_tensor_view_t* b)
{
	const int* ainc = CCV_IS_TENSOR_VIEW(a) ? a->inc : a->info.dim;
	const int* binc = CCV_IS_TENSOR_VIEW(b) ? b->inc : b->info.dim;
	assert(w->info.dim[3] % 4 == 0);
	float* x4w = 0;
	ccmemalign((void **)&x4w, 16, sizeof(float) * w->info.dim[3] * w->info.dim[2] * w->info.dim[1] * w->info.dim[0]);
	if (!x4w)
		return CCV_NNC_EXEC_OOM;
	_ccv_nnc_x4w_sse2(w->data.f32, w->info.dim, x4w);
	int jump_dim = w->info.dim[3] / 4;
	// Do naive tail partition unroll
#define main_for(tail_block) \
	parallel_for(k, jump_dim) { \
		int c; \
		const float* ap = a->data.f32; \
		float* bp = b->data.f32 + k * 4; \
		/* kernel weight for one dim. */ \
		const float* const x4wp = x4w + k * 4 * w->info.dim[0] * w->info.dim[1] * w->info.dim[2]; \
		const float biasval[4] __attribute__ ((__aligned__(16))) = { \
			bias->data.f32[k * 4], \
			bias->data.f32[k * 4 + 1], \
			bias->data.f32[k * 4 + 2], \
			bias->data.f32[k * 4 + 3] \
		}; \
		/* This block will be cause in each for-loop, therefore, you can use it to generate some temporary variables. */ \
		int i[CCV_NNC_MAX_DIM]; \
		int n[CCV_NNC_MAX_DIM]; \
		int m[CCV_NNC_MAX_DIM]; \
		int j[CCV_NNC_MAX_DIM]; \
		for (i[1] = 0; i[1] < b->info.dim[2]; i[1]++) \
		{ \
			set_n_m_dim(i[1], 1, w->info.dim, a->info.dim); \
			const float* wpu = x4wp + n[1] * w->info.dim[1] * w->info.dim[0] * 4; \
			for (i[0] = 0; i[0] < b->info.dim[1]; i[0]++) \
			{ \
				set_n_m_dim(i[0], 0, w->info.dim, a->info.dim); \
				__m128 v40 = _mm_load_ps(biasval); \
				__m128 v41 = _mm_setzero_ps(); \
				__m128 v42 = _mm_setzero_ps(); \
				__m128 v43 = _mm_setzero_ps(); \
				const float* wpz = wpu + n[0] * w->info.dim[0] * 4; \
				const float* apz = ap + ccv_max(i[0] * hint.stride.dim[1] - hint.border.begin[1], 0) * ainc[0]; \
				for (j[1] = 0; j[1] < m[1]; j[1]++) \
				{ \
					for (j[0] = 0; j[0] < m[0]; j[0]++) \
					{ \
						for (c = 0; c < a->info.dim[0] - 3; c += 4) \
						{ \
							__m128 apz4 = _mm_loadu_ps(apz + j[0] * ainc[0] + c); \
							const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
							__m128 w40 = _mm_loadu_ps(wpzu); \
							__m128 w41 = _mm_loadu_ps(wpzu + 4); \
							__m128 w42 = _mm_loadu_ps(wpzu + 8); \
							__m128 w43 = _mm_loadu_ps(wpzu + 12); \
							__m128 apz40 = _mm_shuffle_ps(apz4, apz4, 0x00); \
							__m128 apz41 = _mm_shuffle_ps(apz4, apz4, 0x55); \
							__m128 apz42 = _mm_shuffle_ps(apz4, apz4, 0xAA); \
							__m128 apz43 = _mm_shuffle_ps(apz4, apz4, 0xFF); \
							v40 =_mm_add_ps(_mm_mul_ps(w40, apz40), v40); \
							v41 =_mm_add_ps(_mm_mul_ps(w41, apz41), v41); \
							v42 =_mm_add_ps(_mm_mul_ps(w42, apz42), v42); \
							v43 =_mm_add_ps(_mm_mul_ps(w43, apz43), v43); \
						} \
						tail_block /* insert executions for tail partition */ \
					} \
					wpz += w->info.dim[1] * w->info.dim[0] * 4; \
					apz += ainc[1] * ainc[0]; \
				} \
				__m128 v4 = _mm_add_ps(_mm_add_ps(v40, v41), _mm_add_ps(v42, v43)); \
				_mm_stream_ps(bp + i[0] * binc[0], v4); \
			} \
			bp += binc[1] * binc[0]; \
			ap += ainc[1] * ainc[0] * (ccv_max((i[1] + 1) * hint.stride.dim[2] - hint.border.begin[2], 0) - ccv_max(i[1] * hint.stride.dim[2] - hint.border.begin[2], 0)); \
		} \
	} parallel_endfor
	if (w->info.dim[0] % 4 == 0)
	{
		main_for();
	} else if (w->info.dim[0] % 4 == 3) { // unroll the last for-loops
#define tail_block \
		__m128 apz40 = _mm_load1_ps(apz + j[0] * ainc[0] + c); \
		__m128 apz41 = _mm_load1_ps(apz + j[0] * ainc[0] + c + 1); \
		__m128 apz42 = _mm_load1_ps(apz + j[0] * ainc[0] + c + 2); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		__m128 w40 = _mm_loadu_ps(wpzu); \
		__m128 w41 = _mm_loadu_ps(wpzu + 4); \
		__m128 w42 = _mm_loadu_ps(wpzu + 8); \
		v40 = _mm_add_ps(_mm_mul_ps(w40, apz40), v40); \
		v41 = _mm_add_ps(_mm_mul_ps(w41, apz41), v41); \
		v42 = _mm_add_ps(_mm_mul_ps(w42, apz42), v42);
		main_for(tail_block);
#undef tail_block
	} else if (w->info.dim[0] % 4 == 2) { // unroll the last for-loops
#define tail_block \
		__m128 apz40 = _mm_load1_ps(apz + j[0] * ainc[0] + c); \
		__m128 apz41 = _mm_load1_ps(apz + j[0] * ainc[0] + c + 1); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		__m128 w40 = _mm_loadu_ps(wpzu); \
		__m128 w41 = _mm_loadu_ps(wpzu + 4); \
		v40 = _mm_add_ps(_mm_mul_ps(w40, apz40), v40); \
		v41 = _mm_add_ps(_mm_mul_ps(w41, apz41), v41);
		main_for(tail_block);
#undef tail_block
	} else {
#define tail_block \
		__m128 apz4 = _mm_load1_ps(apz + j[0] * ainc[0] + c); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		__m128 w4 = _mm_loadu_ps(wpzu); \
		v40 = _mm_add_ps(_mm_mul_ps(w4, apz4), v40);
		main_for(tail_block);
#undef tail_block
	}
#undef main_for
	ccfree(x4w);
	return CCV_NNC_EXEC_SUCCESS;
}
#endif

#ifdef HAVE_NEON
inline static void _ccv_nnc_x4w_neon(const float* const w, const int* const dim, float* x4w)
{
	int jump_dim = dim[3] / 4;
	parallel_for(k, jump_dim) {
		int i, j;
		float* x4wz = x4w + k * dim[2] * dim[1] * dim[0] * 4;
		const float* wz[] = {
			w + (k * 4) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 1) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 2) * dim[2] * dim[1] * dim[0],
			w + (k * 4 + 3) * dim[2] * dim[1] * dim[0],
		};
		for (i = 0; i < dim[2] * dim[1]; i++)
		{
			for (j = 0; j < dim[0]; j++)
			{
				x4wz[j * 4] = wz[0][j];
				x4wz[j * 4 + 1] = wz[1][j];
				x4wz[j * 4 + 2] = wz[2][j];
				x4wz[j * 4 + 3] = wz[3][j];
			}
			x4wz += dim[0] * 4;
			wz[0] += dim[0];
			wz[1] += dim[0];
			wz[2] += dim[0];
			wz[3] += dim[0];
		}
	} parallel_endfor
}

static int _ccv_nnc_conv_forw_neon(const ccv_nnc_tensor_view_t* a, const ccv_nnc_tensor_t* w, const ccv_nnc_tensor_t* bias, const ccv_nnc_hint_t hint, ccv_nnc_tensor_view_t* b)
{
	const int* ainc = CCV_IS_TENSOR_VIEW(a) ? a->inc : a->info.dim;
	const int* binc = CCV_IS_TENSOR_VIEW(b) ? b->inc : b->info.dim;
	assert(w->info.dim[3] % 4 == 0);
	float* x4w = 0;
	ccmemalign((void **)&x4w, 16, sizeof(float) * w->info.dim[3] * w->info.dim[2] * w->info.dim[1] * w->info.dim[0]);
	if (!x4w)
		return CCV_NNC_EXEC_OOM;
	_ccv_nnc_x4w_neon(w->data.f32, w->info.dim, x4w);
	int jump_dim = w->info.dim[3] / 4;
#define main_for(tail_block) \
	parallel_for(k, jump_dim) { \
		int c; \
		const float* ap = a->data.f32; \
		float* bp = b->data.f32 + k * 4; \
		/* kernel weight for one dim. */ \
		const float* const x4wp = x4w + k * 4 * w->info.dim[0] * w->info.dim[1] * w->info.dim[2]; \
		const float biasval[4] __attribute__ ((__aligned__(16))) = { \
			bias->data.f32[k * 4], \
			bias->data.f32[k * 4 + 1], \
			bias->data.f32[k * 4 + 2], \
			bias->data.f32[k * 4 + 3] \
		}; \
		/* This block will be cause in each for-loop, therefore, you can use it to generate some temporary variables. */ \
		int i[CCV_NNC_MAX_DIM]; \
		int n[CCV_NNC_MAX_DIM]; \
		int m[CCV_NNC_MAX_DIM]; \
		int j[CCV_NNC_MAX_DIM]; \
		for (i[1] = 0; i[1] < b->info.dim[2]; i[1]++) \
		{ \
			set_n_m_dim(i[1], 1, w->info.dim, a->info.dim); \
			const float* wpu = x4wp + n[1] * w->info.dim[1] * w->info.dim[0] * 4; \
			for (i[0] = 0; i[0] < b->info.dim[1]; i[0]++) \
			{ \
				set_n_m_dim(i[0], 0, w->info.dim, a->info.dim); \
				float32x4_t v40 = vld1q_f32(biasval); \
				float32x4_t v41 = vmovq_n_f32(0); \
				float32x4_t v42 = vmovq_n_f32(0); \
				float32x4_t v43 = vmovq_n_f32(0); \
				const float* wpz = wpu + n[0] * w->info.dim[0] * 4; \
				const float* apz = ap + ccv_max(i[0] * hint.stride.dim[1] - hint.border.begin[1], 0) * ainc[0]; \
				for (j[1] = 0; j[1] < m[1]; j[1]++) \
				{ \
					for (j[0] = 0; j[0] < m[0]; j[0]++) \
					{ \
						for (c = 0; c < a->info.dim[0] - 3; c += 4) \
						{ \
							float32x2x2_t apz4 = vld2_f32(apz + j[0] * ainc[0] + c); \
							const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
							float32x4_t apz40 = vdupq_lane_f32(apz4.val[0], 0); \
							float32x4_t apz41 = vdupq_lane_f32(apz4.val[1], 0); \
							float32x4_t apz42 = vdupq_lane_f32(apz4.val[0], 1); \
							float32x4_t apz43 = vdupq_lane_f32(apz4.val[1], 1); \
							float32x4_t w40 = vld1q_f32(wpzu); \
							float32x4_t w41 = vld1q_f32(wpzu + 4); \
							float32x4_t w42 = vld1q_f32(wpzu + 8); \
							float32x4_t w43 = vld1q_f32(wpzu + 12); \
							v40 = vmlaq_f32(v40, w40, apz40); \
							v41 = vmlaq_f32(v41, w41, apz41); \
							v42 = vmlaq_f32(v42, w42, apz42); \
							v43 = vmlaq_f32(v43, w43, apz43); \
						} \
						tail_block /* insert executions for tail partition */ \
					} \
					wpz += w->info.dim[1] * w->info.dim[0] * 4; \
					apz += ainc[1] * ainc[0]; \
				} \
				v40 = vaddq_f32(v40, v41); \
				v42 = vaddq_f32(v42, v43); \
				vst1q_f32(bp + i[0] * binc[0], vaddq_f32(v40, v42)); \
			} \
			bp += binc[1] * binc[0]; \
			ap += ainc[1] * ainc[0] * (ccv_max((i[1] + 1) * hint.stride.dim[2] - hint.border.begin[2], 0) - ccv_max(i[1] * hint.stride.dim[2] - hint.border.begin[2], 0)); \
		} \
	} parallel_endfor
	if (w->info.dim[0] % 4 == 0)
	{
		main_for();
	} else if (w->info.dim[0] % 4 == 3) { // unroll the last for-loops
#define tail_block \
		float32x2_t apz4 = vld1_f32(apz + j[0] * ainc[0] + c); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		float32x4_t apz40 = vdupq_lane_f32(apz4, 0); \
		float32x4_t apz41 = vdupq_lane_f32(apz4, 1); \
		float32x4_t apz42 = vld1q_dup_f32(apz + j[0] * ainc[0] + c + 2); \
		float32x4_t w40 = vld1q_f32(wpzu); \
		float32x4_t w41 = vld1q_f32(wpzu + 4); \
		float32x4_t w42 = vld1q_f32(wpzu + 8); \
		v40 = vmlaq_f32(v40, w40, apz40); \
		v41 = vmlaq_f32(v41, w41, apz41); \
		v42 = vmlaq_f32(v42, w42, apz42);
		main_for(tail_block);
#undef tail_block
	} else if (w->info.dim[0] % 4 == 2) { // unroll the last for-loops
#define tail_block \
		float32x2_t apz4 = vld1_f32(apz + j[0] * ainc[0] + c); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		float32x4_t apz40 = vdupq_lane_f32(apz4, 0); \
		float32x4_t apz41 = vdupq_lane_f32(apz4, 1); \
		float32x4_t w40 = vld1q_f32(wpzu); \
		float32x4_t w41 = vld1q_f32(wpzu + 4); \
		v40 = vmlaq_f32(v40, w40, apz40); \
		v41 = vmlaq_f32(v41, w41, apz41);
		main_for(tail_block);
#undef tail_block
	} else { // unroll the last for-loops
#define tail_block \
		float32x4_t apz4 = vld1q_dup_f32(apz + j[0] * ainc[0] + c); \
		const float* const wpzu = wpz + (j[0] * w->info.dim[0] + c) * 4; \
		float32x4_t w4 = vld1q_f32(wpzu); \
		v40 = vmlaq_f32(v40, w4, apz4);
		main_for(tail_block);
#undef tail_block
	}
#undef main_for
	ccfree(x4w);
	return CCV_NNC_EXEC_SUCCESS;
}
#endif

int ccv_nnc_conv_forw_opt(const ccv_nnc_tensor_view_t* a, const ccv_nnc_tensor_t* w, const ccv_nnc_tensor_t* bias, const ccv_nnc_hint_t hint, ccv_nnc_tensor_view_t* b)
{
#if defined(HAVE_SSE2)
	if (w->info.dim[3] % 4 == 0)
		return _ccv_nnc_conv_forw_sse2(a, w, bias, hint, b);
#elif defined(HAVE_NEON)
	if (w->info.dim[3] % 4 == 0)
		return _ccv_nnc_conv_forw_neon(a, w, bias, hint, b);
#endif
	return CCV_NNC_EXEC_INVALID;
}