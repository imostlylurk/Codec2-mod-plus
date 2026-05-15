#include "codec2_mod.h"
#include "codec2_internal.h"
#include "analysis.h"
#include "synthesis.h"
#include "nlp.h"
#include "lpc.h"
#include "interp.h"
#include "quantise.h"

/*
 * 1600 bps codec2 — 40ms frames, 4 x 10ms subframes, 64 bits/frame
 *
 * Bit layout:
 *   SF1: voiced[0]         (1 bit)
 *   SF2: voiced[1]         (1 bit)
 *        Wo_index[1]       (7 bits)
 *        e_index[1]        (5 bits)
 *   SF3: voiced[2]         (1 bit)
 *   SF4: voiced[3]         (1 bit)
 *        Wo_index[3]       (7 bits)
 *        e_index[3]        (5 bits)
 *        LSP indexes       (36 bits: {4,4,4,4,4,4,3,3,3,3})
 *   Total: 64 bits
 */

void codec2_init(codec2_t *c2)
{
	c2->next_rn = 1; // random number generator - seed

	for (int i = 0; i < M_PITCH; i++)
		c2->Sn[i] = 1.0f;

	memset(c2->Sn_, 0, sizeof(c2->Sn_));

	/* static FFT mem allocations */
	size_t mem;

	/* FFT forward */
	mem = sizeof(c2->fft_fwd_mem);
	c2->fft_fwd_cfg = kiss_fft_alloc(FFT_ENC, 0, c2->fft_fwd_mem, &mem);

	/* FFT real forward */
	mem = sizeof(c2->fftr_fwd_mem);
	c2->fftr_fwd_cfg = kiss_fftr_alloc(FFT_ENC, 0, c2->fftr_fwd_mem, &mem);

	/* FFT real inverse */
	mem = sizeof(c2->fftr_inv_mem);
	c2->fftr_inv_cfg = kiss_fftr_alloc(FFT_DEC, 1, c2->fftr_inv_mem, &mem);

	/* NLP FFT - reused (same type, direction, and size) */
	c2->nlp.fftr_cfg = c2->fftr_fwd_cfg;

	analysis_init(c2);
	synthesis_init(c2);

	c2->prev_f0_enc = 1.0f / P_MAX_S;
	c2->bg_est = 0.0;
	c2->ex_phase = 0.0;

	for (int l = 1; l <= MAX_AMP; l++)
		c2->prev_model_dec.A[l] = 0.0;

	c2->prev_model_dec.Wo = TWO_PI / P_MAX;
	c2->prev_model_dec.L = M_PI / c2->prev_model_dec.Wo;
	c2->prev_model_dec.voiced = 0;
	memset(c2->prev_model_dec.phi, 0, sizeof(c2->prev_model_dec.phi));
	c2->ex_phase = 0.0f;

	for (int i = 0; i < LPC_ORD; i++)
	{
		c2->prev_lsps_dec[i] = i * M_PI / (LPC_ORD + 1);
	}
	c2->prev_e_dec = 1;

	nlp_init(&c2->nlp);
}

void codec2_encode(codec2_t *c2, uint8_t *bits, const int16_t *speech)
{
	model_t model[4] = {0};
	float ak[LPC_ORD + 1];
	float lsps[LPC_ORD];
	float e_sf2, e_sf4;
	int Wo_index[2], e_index[2];
	int lspd_indexes[LPC_ORD];
	unsigned int nbit = 0;

	memset(bits, 0, BYTES_PER_FRAME); // 64 bits

	/* Analyse subframe 1 - just voicing */
	analyse_one_frame(c2, &model[0], &speech[N_SAMP * 0]);

	/* Analyse subframe 2 - voicing + Wo + energy */
	analyse_one_frame(c2, &model[1], &speech[N_SAMP * 1]);

	/* LPC analysis on SF2 analysis window for energy */
	speech_to_uq_lsps(c2, lsps, ak, &e_sf2, c2->Sn, c2->w);

	/* Analyse subframe 3 - just voicing */
	analyse_one_frame(c2, &model[2], &speech[N_SAMP * 2]);

	/* Analyse subframe 4 - voicing + Wo + energy + LSPs */
	analyse_one_frame(c2, &model[3], &speech[N_SAMP * 3]);

	/* LPC analysis on SF4 analysis window for energy + LSPs */
	speech_to_uq_lsps(c2, lsps, ak, &e_sf4, c2->Sn, c2->w);

	/* Pack voicing for all 4 subframes */
	for (int sf = 0; sf < 4; sf++)
	{
		pack(bits, &nbit, model[sf].voiced, 1);
	}

	/* Subframe 2 (index 1): Wo + energy */
	Wo_index[0] = encode_Wo(model[1].Wo, WO_BITS);
	pack(bits, &nbit, Wo_index[0], WO_BITS);
	e_index[0] = encode_energy(e_sf2, E_BITS);
	pack(bits, &nbit, e_index[0], E_BITS);

	/* Subframe 4 (index 3): Wo + energy */
	Wo_index[1] = encode_Wo(model[3].Wo, WO_BITS);
	pack(bits, &nbit, Wo_index[1], WO_BITS);
	e_index[1] = encode_energy(e_sf4, E_BITS);
	pack(bits, &nbit, e_index[1], E_BITS);

	/* LSP scalar quantization with variable bits per band (36 bits total) */
	encode_lspds_scalar(lspd_indexes, lsps, LSPD_BITS);
	for (int i = 0; i < LSPD_SCALAR_INDEXES; i++)
	{
		pack(bits, &nbit, lspd_indexes[i], LSPD_BITS[i]);
	}
}

void codec2_decode(codec2_t *c2, int16_t *speech, const uint8_t *bits)
{
	model_t model[4] = {0};
	int lspd_indexes[LPC_ORD];
	float lsps[4][LPC_ORD];
	int Wo_index[2], e_index[2];
	float e[4];
	float ak[4][LPC_ORD + 1];
	complex_t Aw[FFT_ENC];
	float A2[FFT_ENC / 2];

	/* unpack bits from channel ------------------------------------*/
	unsigned int nbit = 0;

	/* 4 voicing bits */
	for (int sf = 0; sf < 4; sf++)
	{
		model[sf].voiced = unpack(bits, &nbit, 1);
	}

	/* Subframe 2 (index 1): Wo + energy */
	Wo_index[0] = unpack(bits, &nbit, WO_BITS);
	model[1].Wo = decode_Wo(Wo_index[0], WO_BITS);
	model[1].L = M_PI / model[1].Wo;

	e_index[0] = unpack(bits, &nbit, E_BITS);
	e[1] = decode_energy(e_index[0], E_BITS);

	/* Subframe 4 (index 3): Wo + energy */
	Wo_index[1] = unpack(bits, &nbit, WO_BITS);
	model[3].Wo = decode_Wo(Wo_index[1], WO_BITS);
	model[3].L = M_PI / model[3].Wo;

	e_index[1] = unpack(bits, &nbit, E_BITS);
	e[3] = decode_energy(e_index[1], E_BITS);

	/* LSP indexes (variable bits per band) */
	for (int i = 0; i < LSPD_SCALAR_INDEXES; i++)
	{
		lspd_indexes[i] = unpack(bits, &nbit, LSPD_BITS[i]);
	}
	decode_lspds_scalar(&lsps[3][0], lspd_indexes, LSPD_BITS);

	/* interpolate ------------------------------------------------*/
	/*
	 * Wo and energy transmitted for SF2 and SF4.
	 * SF1: interp from (prev_dec -> SF2)
	 * SF3: interp from (SF2 -> SF4)
	 */
	interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
	interp_Wo(&model[2], &model[1], &model[3]);

	e[0] = interp_energy(c2->prev_e_dec, e[1]);
	e[2] = interp_energy(e[1], e[3]);

	/* LSPs transmitted for SF4 only. Interpolate SF0-2 from prev to SF4. */
	for (int i = 0; i < LPC_ORD; i++)
	{
		float prev = c2->prev_lsps_dec[i];
		float next = lsps[3][i];
		lsps[0][i] = prev + 0.25f * (next - prev);
		lsps[1][i] = prev + 0.50f * (next - prev);
		lsps[2][i] = prev + 0.75f * (next - prev);
	}

	/* synthesize 4 subframes */
	for (int sf = 0; sf < 4; sf++)
	{
		lsp_to_lpc(&lsps[sf][0], &ak[sf][0]);
		aks_to_mag2(c2, &ak[sf][0], &model[sf], e[sf], Aw, A2);
		apply_lpc_correction(&model[sf]);
		synthesise_one_frame(c2, &speech[N_SAMP * sf], &model[sf], Aw, 1.0f);
	}

	/* update memories for next frame ----------------------------*/
	c2->prev_model_dec = model[3];
	c2->prev_e_dec = e[3];
	for (int i = 0; i < LPC_ORD; i++)
		c2->prev_lsps_dec[i] = lsps[3][i];
}
