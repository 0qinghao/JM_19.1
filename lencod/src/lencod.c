/*!
 ***********************************************************************
 *  \mainpage
 *     This is the H.264/AVC encoder reference software. For detailed documentation
 *     see the comments in each file.
 *
 *     The JM software web site is located at:
 *     http://iphome.hhi.de/suehring/tml
 *
 *     For bug reporting and known issues see:
 *     https://ipbt.hhi.de
 *
 *  \author
 *     The main contributors are listed in contributors.h
 *
 *  \version
 *     JM 14.0 (FRExt)
 *
 *  \note
 *     tags are used for document system "doxygen"
 *     available at http://www.doxygen.org
 */
/*!
 *  \file
 *     lencod.c
 *  \brief
 *     H.264/AVC reference encoder project main()
 *  \author
 *   Main contributors (see contributors.h for copyright, address and affiliation details)
 *   - Inge Lille-Langoy               <inge.lille-langoy@telenor.com>
 *   - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *   - Stephan Wenger                  <stewe@cs.tu-berlin.de>
 *   - Jani Lainema                    <jani.lainema@nokia.com>
 *   - Byeong-Moon Jeon                <jeonbm@lge.com>
 *   - Yoon-Seong Soh                  <yunsung@lge.com>
 *   - Thomas Stockhammer              <stockhammer@ei.tum.de>
 *   - Detlev Marpe                    <marpe@hhi.de>
 *   - Guido Heising
 *   - Valeri George                   <george@hhi.de>
 *   - Karsten Suehring                <suehring@hhi.de>
 *   - Alexis Michael Tourapis         <alexismt@ieee.org>
 ***********************************************************************
 */

#include "contributors.h"

#include <time.h>
#include <math.h>
#include <sys/timeb.h>
#include "global.h"

#include "configfile.h"
#include "leaky_bucket.h"
#include "memalloc.h"
#include "intrarefresh.h"
#include "fmo.h"
#include "sei.h"
#include "parset.h"
#include "image.h"
#include "input.h"
#include "output.h"

#include "me_epzs.h"
#include "me_umhex.h"
#include "me_umhexsmp.h"

#include "ratectl.h"
#include "explicit_gop.h"
#include "context_ini.h"

#include "q_matrix.h"
#include "q_offsets.h"
#include "rdo_quant.h"

#define JM      "14 (FRExt)"
#define VERSION "14.0"
#define EXT_VERSION "(FRExt)"

InputParameters inputs,      *params = &inputs;
ImageParameters images,      *img   = &images;
StatParameters  statistics,  *stats = &statistics;
SNRParameters   snrs,        *snr   = &snrs;
Decoders decoders, *decs=&decoders;

static void information_init(void);

#ifdef _ADAPT_LAST_GROUP_
int initial_Bframes = 0;
#endif

Boolean In2ndIGOP = FALSE;
int    start_frame_no_in_this_IGOP = 0;
int    start_tr_in_this_IGOP = 0;
int    FirstFrameIn2ndIGOP=0;
int    cabac_encoding = 0;
int    frame_statistic_start;
extern ColocatedParams *Co_located;
extern ColocatedParams *Co_located_JV[MAX_PLANE];  //!< Co_located to be used during 4:4:4 independent mode encoding
extern double *mb16x16_cost_frame;
extern int FrameNumberInFile;
static char DistortionType[3][20] = {"SAD", "SSE", "Hadamard SAD"};

void Init_Motion_Search_Module (void);
void Clear_Motion_Search_Module (void);
void report_frame_statistic(void);
void SetLevelIndices(void);
void chroma_mc_setup(void);

void init_stats (void)
{
  stats->successive_Bframe = params->successive_Bframe;
  stats->bit_ctr_I = 0;
  stats->bit_ctr_P = 0;
  stats->bit_ctr_B = 0;
  snr->snr_y = 0.0;
  snr->snr_u = 0.0;
  snr->snr_v = 0.0;
  snr->snr_y1 = 0.0;
  snr->snr_u1 = 0.0;
  snr->snr_v1 = 0.0;
  snr->snr_ya = 0.0;
  snr->snr_ua = 0.0;
  snr->snr_va = 0.0;
  snr->sse_y  = 0.0;
  snr->sse_u  = 0.0;
  snr->sse_v  = 0.0;
  snr->msse_y = 0.0;
  snr->msse_u = 0.0;
  snr->msse_v = 0.0;
  snr->frame_ctr = 0;
}

/*!
 ***********************************************************************
 * \brief
 *    Initialize encoding parameters.
 ***********************************************************************
 */
void init_frame_params()
{
  int base_mul = 0;

  if (params->idr_period)
  {
    if (!params->adaptive_idr_period && ( img->frm_number - img->lastIDRnumber ) % params->idr_period == 0 )
      img->nal_reference_idc = NALU_PRIORITY_HIGHEST;
    if (params->adaptive_idr_period == 1 && ( img->frm_number - imax(img->lastIntraNumber, img->lastIDRnumber) ) % params->idr_period == 0 )
      img->nal_reference_idc = NALU_PRIORITY_HIGHEST;
    else
      img->nal_reference_idc = (params->DisposableP) ? (img->frm_number + 1)% 2 : NALU_PRIORITY_LOW;
  }
  else
    img->nal_reference_idc = (img->frm_number && params->DisposableP) ? (img->frm_number + 1)% 2 : NALU_PRIORITY_LOW;

  //much of this can go in init_frame() or init_field()?
  //poc for this frame or field
  if (params->idr_period)
  {
    if (!params->adaptive_idr_period)
      base_mul = ( img->frm_number - img->lastIDRnumber ) % params->idr_period;
    else if (params->adaptive_idr_period == 1)
      base_mul = (( img->frm_number - imax(img->lastIntraNumber, img->lastIDRnumber) ) % params->idr_period == 0) ? 0 : ( img->frm_number - img->lastIDRnumber );
  }
  else 
    base_mul = ( img->frm_number - img->lastIDRnumber );

  if ((img->frm_number - img->lastIDRnumber) <= params->intra_delay)
  {    
    base_mul = -base_mul;
  }
  else
  {
    base_mul -= ( base_mul ? params->intra_delay :  0);    
  }

  img->toppoc = base_mul * (2 * (params->jumpd + 1));

  if ((params->PicInterlace==FRAME_CODING) && (params->MbInterlace==FRAME_CODING))
    img->bottompoc = img->toppoc;     //progressive
  else
    img->bottompoc = img->toppoc + 1;   //hard coded

  img->framepoc = imin (img->toppoc, img->bottompoc);

  //the following is sent in the slice header
  img->delta_pic_order_cnt[0] = 0;

  if ((params->BRefPictures == 1) && (img->frm_number))
  {
    img->delta_pic_order_cnt[0] = +2 * params->successive_Bframe;
  }  
}

/*!
 ***********************************************************************
 * \brief
 *    Main function for encoder.
 * \param argc
 *    number of command line arguments
 * \param argv
 *    command line arguments
 * \return
 *    exit code
 ***********************************************************************
 */
int main(int argc,char **argv)
{
  int nplane;
  int primary_disp = 0;
  giRDOpt_B8OnlyFlag = 0;

  p_dec = p_in = -1;

  p_stat = p_log = p_trace = NULL;

  frame_statistic_start = 1;

  Configure (argc, argv);

  Init_QMatrix();
  Init_QOffsetMatrix();

  AllocNalPayloadBuffer();

  init_poc();
  GenerateParameterSets();
  SetLevelIndices();

  init_img();

  init_rdopt ();
#ifdef _LEAKYBUCKET_
  Bit_Buffer = malloc((params->no_frames * (params->successive_Bframe + 1) + 1) * sizeof(long));
#endif

  // Prepare hierarchical coding structures. 
  // Code could be extended in the future to allow structure adaptation.
  if (params->HierarchicalCoding)
  {
    init_gop_structure();
    if (params->successive_Bframe && params->HierarchicalCoding == 3)
      interpret_gop_structure();
    else
      create_hierarchy();
  }

  dpb.init_done = 0;
  init_dpb();
  init_out_buffer();
  init_stats();

  enc_picture = NULL;

  init_global_buffers();

  create_context_memory ();
  Init_Motion_Search_Module ();
  information_init();

  //Rate control
  if (params->RCEnable)
    rc_init_sequence();

  if (params->SearchMode == UM_HEX)
    UMHEX_DefineThreshold();

  // Init frame type counter. Only supports single slice per frame.
  memset(frame_ctr, 0, 5 * sizeof(int));

  img->last_valid_reference = 0;
  tot_time=0;                 // time for total encoding session

#ifdef _ADAPT_LAST_GROUP_
  if (params->last_frame > 0)
    params->no_frames = 1 + (params->last_frame + params->jumpd) / (params->jumpd + 1);
  initial_Bframes = params->successive_Bframe;
#endif

  PatchInputNoFrames();

  // Write sequence header (with parameter sets)
  stats->bit_ctr_parametersets = 0;
  stats->bit_slice = start_sequence();
  stats->bit_ctr_parametersets += stats->bit_ctr_parametersets_n;
  start_frame_no_in_this_IGOP = 0;


  if (params->UseRDOQuant)
    precalculate_unary_exp_golomb_level();

  for (img->number=0; img->number < params->no_frames; img->number++)
  {
    img->frm_number = img->number;    
    FrameNumberInFile = CalculateFrameNumber();
    SetImgType();
    init_frame_params();

    if (img->last_ref_idc)
    {
      img->frame_num++;                 //increment frame_num once for B-frames
      img->frame_num %= max_frame_num;
    }

    //frame_num for this frame
    if (params->idr_period && ((!params->adaptive_idr_period && ( img->frm_number - img->lastIDRnumber ) % params->idr_period == 0)
      || (params->adaptive_idr_period == 1 && ( img->frm_number - imax(img->lastIntraNumber, img->lastIDRnumber) ) % params->idr_period == 0)) )
    {
      img->frame_num = 0;
      primary_disp   = 0;   
    }

    if (params->ResendSPS == 1 && img->type == I_SLICE && img->frm_number != 0)
    {
      stats->bit_slice = rewrite_paramsets();
      stats->bit_ctr_parametersets += stats->bit_ctr_parametersets_n;
    }

#ifdef _ADAPT_LAST_GROUP_
    if (params->successive_Bframe && params->last_frame && IMG_NUMBER+1 == params->no_frames)
    {
      int bi = (int)((float)(params->jumpd+1)/(params->successive_Bframe + 1.0) + 0.499999);

      params->successive_Bframe = ((params->last_frame - (img->frm_number - 1)*(params->jumpd + 1)) / bi) - 1;

      //about to code the last ref frame, adjust delta poc
      img->delta_pic_order_cnt[0]= -2*(initial_Bframes - params->successive_Bframe);
      img->toppoc    += img->delta_pic_order_cnt[0];
      img->bottompoc += img->delta_pic_order_cnt[0];
      img->framepoc   = imin (img->toppoc, img->bottompoc);
    }
#endif

    //Rate control
    if (params->RCEnable && img->type == I_SLICE)
      rc_init_gop_params();

    // which layer does the image belong to?
    img->layer = (IMG_NUMBER % (params->NumFramesInELSubSeq + 1)) ? 0 : 1;

    // redundant frame initialization and allocation
    if (params->redundant_pic_flag)
    {
      Init_redundant_frame();
      Set_redundant_frame();
    }

    encode_one_frame(); // encode one I- or P-frame

    img->last_ref_idc = img->nal_reference_idc ? 1 : 0;

    // if key frame is encoded, encode one redundant frame
    if (params->redundant_pic_flag && key_frame)
    {
      encode_one_redundant_frame();
    }

    if (img->type == I_SLICE && params->EnableOpenGOP)
      img->last_valid_reference = img->ThisPOC;

    if (params->ReportFrameStats)
      report_frame_statistic();

    if (img->nal_reference_idc == 0)
    {
      primary_disp ++;
      //img->frame_num -= 1;
      //img->frame_num %= max_frame_num;
    }

    if (img->currentPicture->idr_flag)
    {
      img->idr_gop_number = 0;
      //start_frame_no_in_this_IGOP = img->frm_number;
      //start_tr_in_this_IGOP = img->toppoc;
      // start_tr_in_this_IGOP = (img->frm_number - 1 ) * (params->jumpd + 1) +1;
    }
    else
      img->idr_gop_number ++;

    encode_enhancement_layer();    
  }
  // terminate sequence
  terminate_sequence();

  flush_dpb();

  close(p_in);
  if (-1 != p_dec)
    close(p_dec);
  if (p_trace)
    fclose(p_trace);

  Clear_Motion_Search_Module ();

  RandomIntraUninit();
  FmoUninit();

  if (params->HierarchicalCoding)
    clear_gop_structure ();

  // free structure for rd-opt. mode decision
  clear_rdopt ();

#ifdef _LEAKYBUCKET_
  calc_buffer();
#endif

  // report everything
  report();

#ifdef _LEAKYBUCKET_
  if (Bit_Buffer)
  free(Bit_Buffer);
#endif

  free_dpb();
  if( IS_INDEPENDENT(params) )
  {
    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      free_colocated(Co_located_JV[nplane]);
    }
  }
  else
  {
    free_colocated(Co_located);
  }
  uninit_out_buffer();

  free_global_buffers();

  // free image mem
  free_img ();
  free_context_memory ();
  FreeNalPayloadBuffer();
  FreeParameterSets();

  return 0;
}
/*!
 ***********************************************************************
 * \brief
 *    Terminates and reports statistics on error.
 *
 ***********************************************************************
 */
void report_stats_on_error(void)
{
  int nplane;
  params->no_frames = img->frm_number;
  terminate_sequence();

  flush_dpb();

  close(p_in);
  if (-1 != p_dec)
    close(p_dec);

  if (p_trace)
    fclose(p_trace);

  Clear_Motion_Search_Module ();

  RandomIntraUninit();
  FmoUninit();

  if (params->HierarchicalCoding)
    clear_gop_structure ();

  // free structure for rd-opt. mode decision
  clear_rdopt ();

#ifdef _LEAKYBUCKET_
  calc_buffer();
#endif

  if (params->ReportFrameStats)
    report_frame_statistic();

  // report everything
  report();

  free_dpb();
  if( IS_INDEPENDENT(params) )
  {
    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      free_colocated(Co_located_JV[nplane]);
    }
  }
  else
  {
    free_colocated(Co_located);
  }
  uninit_out_buffer();

  free_global_buffers();

  // free image mem
  free_img ();
  free_context_memory ();
  FreeNalPayloadBuffer();
  FreeParameterSets();
  exit (-1);
}

/*!
 ***********************************************************************
 * \brief
 *    Initializes the POC structure with appropriate parameters.
 *
 ***********************************************************************
 */
void init_poc()
{
  //the following should probably go in sequence parameters
  // frame poc's increase by 2, field poc's by 1

  img->pic_order_cnt_type=params->pic_order_cnt_type;

  img->delta_pic_order_always_zero_flag = FALSE;
  img->num_ref_frames_in_pic_order_cnt_cycle= 1;

  if (params->BRefPictures == 1)
  {
    img->offset_for_non_ref_pic  =   0;
    img->offset_for_ref_frame[0] =   2;
  }
  else
  {
    img->offset_for_non_ref_pic  =  -2*(params->successive_Bframe);
    img->offset_for_ref_frame[0] =   2*(params->successive_Bframe+1);
  }

  if ((params->PicInterlace==FRAME_CODING) && (params->MbInterlace==FRAME_CODING))
    img->offset_for_top_to_bottom_field=0;
  else
    img->offset_for_top_to_bottom_field=1;

  if ((params->PicInterlace==FRAME_CODING) && (params->MbInterlace==FRAME_CODING))
  {
    img->pic_order_present_flag = FALSE;
    img->delta_pic_order_cnt_bottom = 0;
  }
  else
  {
    img->pic_order_present_flag = TRUE;
    img->delta_pic_order_cnt_bottom = 1;
  }
}


/*!
 ***********************************************************************
 * \brief
 *    Initializes the img->nz_coeff
 * \par Input:
 *    none
 * \par  Output:
 *    none
 * \ side effects
 *    sets omg->nz_coef[][][][] to -1
 ***********************************************************************
 */
void CAVLC_init(void)
{
  unsigned int i, k, l;

  for (i = 0; i < img->PicSizeInMbs; i++)
    for (k = 0; k < 4; k++)
      for (l = 0; l < (4 + (unsigned int)img->num_blk8x8_uv); l++)
        img->nz_coeff[i][k][l] = 0;
}


/*!
 ***********************************************************************
 * \brief
 *    Initializes the Image structure with appropriate parameters.
 * \par Input:
 *    Input Parameters struct inp_par *inp
 * \par  Output:
 *    Image Parameters ImageParameters *img
 ***********************************************************************
 */
void init_img()
{
  int i, j;
  int byte_abs_range;

  static int mb_width_cr[4] = {0,8, 8,16};
  static int mb_height_cr[4]= {0,8,16,16};

  // Color format
  img->yuv_format = params->yuv_format;
  img->P444_joined = (img->yuv_format == YUV444 && !IS_INDEPENDENT(params));  

  //pel bitdepth init
  img->bitdepth_luma            = params->output.bit_depth[0];
  img->bitdepth_scale[0]        = 1 << (img->bitdepth_luma - 8);
  img->bitdepth_lambda_scale    = 2 * (img->bitdepth_luma - 8);
  img->bitdepth_luma_qp_scale   = 3 *  img->bitdepth_lambda_scale;
  img->dc_pred_value_comp[0]    =  1<<(img->bitdepth_luma - 1);
  img->max_imgpel_value_comp[0] = (1<<img->bitdepth_luma) - 1;

  img->dc_pred_value            =  img->dc_pred_value_comp[0]; // set defaults
  img->max_imgpel_value         = img->max_imgpel_value_comp[0];

  img->mb_size[0][0]            = img->mb_size[0][1] = MB_BLOCK_SIZE;

  // Initialization for RC QP parameters (could be placed in ratectl.c)
  img->RCMinQP                = params->RCMinQP[P_SLICE];
  img->RCMaxQP                = params->RCMaxQP[P_SLICE];

  // Set current residue & prediction array pointers
  img->curr_res = img->m7[0];
  img->curr_prd = img->mpr[0];

  if (img->yuv_format != YUV400)
  {
    img->bitdepth_chroma          = params->output.bit_depth[1];
    img->bitdepth_scale[1]        = 1 << (img->bitdepth_chroma - 8);
    img->dc_pred_value_comp[1]    = 1<<(img->bitdepth_chroma - 1);
    img->dc_pred_value_comp[2]    = img->dc_pred_value_comp[1];
    img->max_imgpel_value_comp[1] = (1<<img->bitdepth_chroma) - 1;
    img->max_imgpel_value_comp[2] = img->max_imgpel_value_comp[1];
    img->num_blk8x8_uv            = (1<<img->yuv_format)&(~(0x1));
    img->num_cdc_coeff            = img->num_blk8x8_uv << 1;
    img->mb_size[1][0] = img->mb_size[2][0] = img->mb_cr_size_x = (img->yuv_format == YUV420 || img->yuv_format == YUV422) ? 8 : 16;
    img->mb_size[1][1] = img->mb_size[2][1] = img->mb_cr_size_y = (img->yuv_format == YUV444 || img->yuv_format == YUV422) ? 16 : 8;

    img->bitdepth_chroma_qp_scale = 6*(img->bitdepth_chroma - 8);

    img->chroma_qp_offset[0] = active_pps->cb_qp_index_offset;
    img->chroma_qp_offset[1] = active_pps->cr_qp_index_offset;
  }
  else
  {
    img->bitdepth_chroma     = 0;
    img->bitdepth_scale[1]   = 0;
    img->max_imgpel_value_comp[1] = 0;
    img->max_imgpel_value_comp[2] = img->max_imgpel_value_comp[1];
    img->num_blk8x8_uv       = 0;
    img->num_cdc_coeff       = 0;
    img->mb_size[1][0] = img->mb_size[2][0] = img->mb_cr_size_x = 0;
    img->mb_size[1][1] = img->mb_size[2][1] = img->mb_cr_size_y = 0;

    img->bitdepth_chroma_qp_scale = 0;
    img->bitdepth_chroma_qp_scale = 0;

    img->chroma_qp_offset[0] = 0;
    img->chroma_qp_offset[1] = 0;
  }  
 
  //img->pic_unit_size_on_disk = (imax(img->bitdepth_luma , img->bitdepth_chroma) > 8) ? 16 : 8;
  img->pic_unit_size_on_disk = (imax(params->source.bit_depth[0], params->source.bit_depth[1]) > 8) ? 16 : 8;
  img->out_unit_size_on_disk = (imax(params->output.bit_depth[0], params->output.bit_depth[1]) > 8) ? 16 : 8;

  img->max_bitCount =  128 + 256 * img->bitdepth_luma + 2 * img->mb_cr_size_y * img->mb_cr_size_x * img->bitdepth_chroma;
  //img->max_bitCount =  (128 + 256 * img->bitdepth_luma + 2 *img->mb_cr_size_y * img->mb_cr_size_x * img->bitdepth_chroma)*2;

  img->max_qp_delta = (25 + (img->bitdepth_luma_qp_scale>>1));
  img->min_qp_delta = img->max_qp_delta + 1;

  img->num_ref_frames = active_sps->num_ref_frames;
  img->max_num_references   = active_sps->frame_mbs_only_flag ? active_sps->num_ref_frames : 2 * active_sps->num_ref_frames;

  img->buf_cycle = params->num_ref_frames;
  img->base_dist = params->jumpd;  

  // Intra/IDR related parameters
  img->lastIntraNumber = 0;
  img->lastINTRA       = 0;
  img->lastIDRnumber   = 0;
  img->last_ref_idc    = 0;
  img->idr_refresh     = 0;
  img->idr_gop_number  = 0;
  img->rewind_frame    = 0;

  img->DeblockCall     = 0;
  img->framerate       = (float) params->FrameRate;   // The basic frame rate (of the original sequence)

  // Allocate proper memory space for different parameters (i.e. MVs, coefficients, etc)
  if (!params->IntraProfile)
  {
    get_mem_mv (&(img->pred_mv));
    get_mem_mv (&(img->all_mv));

    if (params->BiPredMotionEstimation)
    {
      get_mem_mv (&(img->bipred_mv1));
      get_mem_mv (&(img->bipred_mv2));
    }
  }

  get_mem_ACcoeff (&(img->cofAC));
  get_mem_DCcoeff (&(img->cofDC));

  if (params->AdaptiveRounding)
  {
    get_mem3Dint(&(img->fadjust4x4), 4, MB_BLOCK_SIZE, MB_BLOCK_SIZE);
    get_mem3Dint(&(img->fadjust8x8), 3, MB_BLOCK_SIZE, MB_BLOCK_SIZE);

    if (img->yuv_format != 0)
    {
      get_mem4Dint(&(img->fadjust4x4Cr), 2, 4, img->mb_cr_size_y, img->mb_cr_size_x);
      get_mem4Dint(&(img->fadjust8x8Cr), 2, 3, img->mb_cr_size_y, img->mb_cr_size_x);
    }
  }

  if(params->MbInterlace)
  {
    if (!params->IntraProfile)
    {
      get_mem_mv (&(rddata_top_frame_mb.pred_mv));
      get_mem_mv (&(rddata_top_frame_mb.all_mv));

      get_mem_mv (&(rddata_bot_frame_mb.pred_mv));
      get_mem_mv (&(rddata_bot_frame_mb.all_mv));
    }

    get_mem_ACcoeff (&(rddata_top_frame_mb.cofAC));
    get_mem_DCcoeff (&(rddata_top_frame_mb.cofDC));

    get_mem_ACcoeff (&(rddata_bot_frame_mb.cofAC));
    get_mem_DCcoeff (&(rddata_bot_frame_mb.cofDC));

    if ( params->MbInterlace != FRAME_MB_PAIR_CODING )
    {
      if (!params->IntraProfile)
      {      
        get_mem_mv (&(rddata_top_field_mb.pred_mv));
        get_mem_mv (&(rddata_top_field_mb.all_mv));

        get_mem_mv (&(rddata_bot_field_mb.pred_mv));
        get_mem_mv (&(rddata_bot_field_mb.all_mv));
      }

      get_mem_ACcoeff (&(rddata_top_field_mb.cofAC));
      get_mem_DCcoeff (&(rddata_top_field_mb.cofDC));

      get_mem_ACcoeff (&(rddata_bot_field_mb.cofAC));
      get_mem_DCcoeff (&(rddata_bot_field_mb.cofDC));
    }
  }

  if (params->UseRDOQuant && params->RDOQ_QP_Num > 1)
  {
    get_mem_ACcoeff (&(rddata_trellis_curr.cofAC));
    get_mem_DCcoeff (&(rddata_trellis_curr.cofDC));

    get_mem_ACcoeff (&(rddata_trellis_best.cofAC));
    get_mem_DCcoeff (&(rddata_trellis_best.cofDC));
    
    if (!params->IntraProfile)
    {          
      get_mem_mv (&(rddata_trellis_curr.pred_mv));
      get_mem_mv (&(rddata_trellis_curr.all_mv));
      get_mem_mv (&(rddata_trellis_best.pred_mv));
      get_mem_mv (&(rddata_trellis_best.all_mv));

      if (params->Transform8x8Mode && params->RDOQ_CP_MV)
      {
        get_mem5Dshort(&tmp_mv8, 2, img->max_num_references, 4, 4, 2);
        get_mem5Dshort(&tmp_pmv8, 2, img->max_num_references, 4, 4, 2);
        get_mem3Dint  (&motion_cost8, 2, img->max_num_references, 4);
      }
    }
  }

  byte_abs_range = (imax(img->max_imgpel_value_comp[0],img->max_imgpel_value_comp[1]) + 1) * 2;

  if ((img->quad = (int*)calloc (byte_abs_range, sizeof(int))) == NULL)
    no_mem_exit ("init_img: img->quad");

  img->quad += byte_abs_range/2;
  for (i=0; i < byte_abs_range/2; ++i)
  {
    img->quad[i] = img->quad[-i] = i * i;
  }

  img->width         = (params->output.width  + img->auto_crop_right);
  img->height        = (params->output.height + img->auto_crop_bottom);
  img->width_blk     = img->width  / BLOCK_SIZE;
  img->height_blk    = img->height / BLOCK_SIZE;
  img->width_padded  = img->width  + 2 * IMG_PAD_SIZE;
  img->height_padded = img->height + 2 * IMG_PAD_SIZE;

  if (img->yuv_format != YUV400)
  {
    img->width_cr = img->width  * mb_width_cr[img->yuv_format]  / 16;
    img->height_cr= img->height * mb_height_cr[img->yuv_format] / 16;
  }
  else
  {
    img->width_cr = 0;
    img->height_cr= 0;
  }
  img->height_cr_frame = img->height_cr;

  img->size = img->width * img->height;
  img->size_cr = img->width_cr * img->height_cr;

  img->PicWidthInMbs    = img->width/MB_BLOCK_SIZE;
  img->FrameHeightInMbs = img->height/MB_BLOCK_SIZE;
  img->FrameSizeInMbs   = img->PicWidthInMbs * img->FrameHeightInMbs;

  img->PicHeightInMapUnits = ( active_sps->frame_mbs_only_flag ? img->FrameHeightInMbs : img->FrameHeightInMbs/2 );

  if( IS_INDEPENDENT(params) )
  {
    for( i=0; i<MAX_PLANE; i++ ){
      if ((img->mb_data_JV[i] = (Macroblock *) calloc(img->FrameSizeInMbs,sizeof(Macroblock))) == NULL)
        no_mem_exit("init_img: img->mb_data_JV");
    }
    img->mb_data = NULL;
  }
  else
  {
    if ((img->mb_data = (Macroblock *) calloc(img->FrameSizeInMbs, sizeof(Macroblock))) == NULL)
      no_mem_exit("init_img: img->mb_data");
  }

  if (params->UseConstrainedIntraPred)
  {
    if ((img->intra_block = (int*)calloc(img->FrameSizeInMbs, sizeof(int))) == NULL)
      no_mem_exit("init_img: img->intra_block");
  }

  if (params->CtxAdptLagrangeMult == 1)
  {
    if ((mb16x16_cost_frame = (double*)calloc(img->FrameSizeInMbs, sizeof(double))) == NULL)
    {
      no_mem_exit("init mb16x16_cost_frame");
    }
  }
  get_mem2D((byte***)&(img->ipredmode), img->height_blk, img->width_blk);        //need two extra rows at right and bottom
  get_mem2D((byte***)&(img->ipredmode8x8), img->height_blk, img->width_blk);     // help storage for ipredmode 8x8, inserted by YV
  memset(&(img->ipredmode[0][0]), -1, img->height_blk * img->width_blk *sizeof(char));
  memset(&(img->ipredmode8x8[0][0]), -1, img->height_blk * img->width_blk *sizeof(char));

  get_mem2D((byte***)&(rddata_top_frame_mb.ipredmode), img->height_blk, img->width_blk);
  get_mem2D((byte***)&(rddata_trellis_curr.ipredmode), img->height_blk, img->width_blk);
  get_mem2D((byte***)&(rddata_trellis_best.ipredmode), img->height_blk, img->width_blk);

  if (params->MbInterlace)
  {
    get_mem2D((byte***)&(rddata_bot_frame_mb.ipredmode), img->height_blk, img->width_blk);
    get_mem2D((byte***)&(rddata_top_field_mb.ipredmode), img->height_blk, img->width_blk);
    get_mem2D((byte***)&(rddata_bot_field_mb.ipredmode), img->height_blk, img->width_blk);
  }
  // CAVLC mem
  get_mem3Dint(&(img->nz_coeff), img->FrameSizeInMbs, 4, 4+img->num_blk8x8_uv);


  get_mem2Ddb_offset(&(img->lambda_md), 10, 52 + img->bitdepth_luma_qp_scale,img->bitdepth_luma_qp_scale);
  get_mem3Ddb_offset (&(img->lambda_me), 10, 52 + img->bitdepth_luma_qp_scale, 3, img->bitdepth_luma_qp_scale);
  get_mem3Dint_offset(&(img->lambda_mf), 10, 52 + img->bitdepth_luma_qp_scale, 3, img->bitdepth_luma_qp_scale);

  if (params->CtxAdptLagrangeMult == 1)
  {
    get_mem2Ddb_offset(&(img->lambda_mf_factor), 10, 52 + img->bitdepth_luma_qp_scale, img->bitdepth_luma_qp_scale);
  }

  CAVLC_init();

  img->b_frame_to_code = 0;
  img->GopLevels = (params->successive_Bframe) ? 1 : 0;
  img->mb_y_upd=0;

  RandomIntraInit (img->PicWidthInMbs, img->FrameHeightInMbs, params->RandomIntraMBRefresh);

  InitSEIMessages();  // Tian Dong (Sept 2002)

  initInput(&params->source, &params->output);

  // Allocate I/O Frame memory
  if (AllocateFrameMemory(img, params->source.size))
    no_mem_exit("AllocateFrameMemory: buf");
  // Initialize filtering parameters. If sending parameters, the offsets are
  // multiplied by 2 since inputs are taken in "div 2" format.
  // If not sending parameters, all fields are cleared
  if (params->DFSendParameters)
  {
    for (j = 0; j < 2; j++)
    {
      for (i = 0; i < 5; i++)
      {
        params->DFAlpha[j][i] <<= 1;
        params->DFBeta [j][i] <<= 1;
      }
    }
  }
  else
  {
    for (j = 0; j < 2; j++)
    {
      for (i = 0; i < 5; i++)
      {
        params->DFDisableIdc[j][i] = 0;
        params->DFAlpha[j][i] = 0;
        params->DFBeta[j][i] = 0;
      }
    }
  }

  if( params->separate_colour_plane_flag )
  {
    img->ChromaArrayType = 0;
  }
  else
  {
    img->ChromaArrayType = params->yuv_format;
  }

  if (params->RDPictureDecision)
  {
    img->frm_iter = 3;
  }
  else
    img->frm_iter = 1;
}


/*!
 ***********************************************************************
 * \brief
 *    Free the Image structures
 * \par Input:
 *    Image Parameters ImageParameters *img
 ***********************************************************************
 */
void free_img ()
{
  // Delete Frame memory 
  DeleteFrameMemory();

  CloseSEIMessages(); 
  if (!params->IntraProfile)
  {
    free_mem_mv (img->pred_mv);
    free_mem_mv (img->all_mv);

    if (params->BiPredMotionEstimation)
    {
      free_mem_mv (img->bipred_mv1);
      free_mem_mv (img->bipred_mv2);
    }
  }

  free_mem_ACcoeff (img->cofAC);
  free_mem_DCcoeff (img->cofDC);

  if (params->AdaptiveRounding)
  {
    free_mem3Dint(img->fadjust4x4);
    free_mem3Dint(img->fadjust8x8);
    if (img->yuv_format != 0)
    {
      free_mem4Dint(img->fadjust4x4Cr);
      free_mem4Dint(img->fadjust8x8Cr);
    }
  }


  if (params->MbInterlace)
  {
    if (!params->IntraProfile)
    {    
      free_mem_mv (rddata_top_frame_mb.pred_mv);
      free_mem_mv (rddata_top_frame_mb.all_mv);

      free_mem_mv (rddata_bot_frame_mb.pred_mv);
      free_mem_mv (rddata_bot_frame_mb.all_mv);
    }

    free_mem_ACcoeff (rddata_top_frame_mb.cofAC);
    free_mem_DCcoeff (rddata_top_frame_mb.cofDC);

    free_mem_ACcoeff (rddata_bot_frame_mb.cofAC);
    free_mem_DCcoeff (rddata_bot_frame_mb.cofDC);

    if ( params->MbInterlace != FRAME_MB_PAIR_CODING )
    {
      if (!params->IntraProfile)
      {
        free_mem_mv (rddata_top_field_mb.pred_mv);
        free_mem_mv (rddata_top_field_mb.all_mv);

        free_mem_mv (rddata_bot_field_mb.pred_mv);
        free_mem_mv (rddata_bot_field_mb.all_mv);
      }

      free_mem_ACcoeff (rddata_top_field_mb.cofAC);
      free_mem_DCcoeff (rddata_top_field_mb.cofDC);

      free_mem_ACcoeff (rddata_bot_field_mb.cofAC);
      free_mem_DCcoeff (rddata_bot_field_mb.cofDC);
    }
  }

  if (params->UseRDOQuant && params->RDOQ_QP_Num > 1)
  {
    free_mem_ACcoeff (rddata_trellis_curr.cofAC);
    free_mem_DCcoeff (rddata_trellis_curr.cofDC);
    free_mem_ACcoeff (rddata_trellis_best.cofAC);
    free_mem_DCcoeff (rddata_trellis_best.cofDC);

    if (!params->IntraProfile)
    {    
      free_mem_mv (rddata_trellis_curr.pred_mv);
      free_mem_mv (rddata_trellis_curr.all_mv);

      free_mem_mv (rddata_trellis_best.pred_mv);
      free_mem_mv (rddata_trellis_best.all_mv);

      if (params->Transform8x8Mode && params->RDOQ_CP_MV)
      {
        free_mem5Dshort(tmp_mv8);
        free_mem5Dshort(tmp_pmv8);
        free_mem3Dint(motion_cost8);
      }
    }
  }

  free (img->quad - (imax(img->max_imgpel_value_comp[0],img->max_imgpel_value_comp[1]) + 1));

  if (params->MbInterlace)
  {
    free_mem2D((byte**)rddata_bot_frame_mb.ipredmode);
    free_mem2D((byte**)rddata_top_field_mb.ipredmode);
    free_mem2D((byte**)rddata_bot_field_mb.ipredmode);
  }
}


/*!
 ************************************************************************
 * \brief
 *    Allocates the picture structure along with its dependent
 *    data structures
 * \return
 *    Pointer to a Picture
 ************************************************************************
 */

Picture *malloc_picture()
{
  Picture *pic;
  if ((pic = calloc (1, sizeof (Picture))) == NULL) no_mem_exit ("malloc_picture: Picture structure");
  //! Note: slice structures are allocated as needed in code_a_picture
  return pic;
}

/*!
 ************************************************************************
 * \brief
 *    Frees a picture
 * \param
 *    pic: POinter to a Picture to be freed
 ************************************************************************
 */


void free_picture(Picture *pic)
{
  if (pic != NULL)
  {
    free_slice_list(pic);
    free (pic);
  }
}


/*!
 ************************************************************************
 * \brief
 *    Reports frame statistical data to a stats file
 ************************************************************************
 */
void report_frame_statistic()
{
  FILE *p_stat_frm = NULL;
  static int64 last_mode_use[NUM_PIC_TYPE][MAXMODE];
  static int   last_b8_mode_0[NUM_PIC_TYPE][2];
  static int   last_mode_chroma_use[4];
  static int64 last_bit_ctr_n = 0;
  int i;
  char name[30];
  int bitcounter;

#ifndef WIN32
  time_t now;
  struct tm *l_time;
  char string[1000];
#else
  char timebuf[128];
#endif


  // write to log file
  if ((p_stat_frm = fopen("stat_frame.dat", "r")) == 0)            // check if file exists
  {
    if ((p_stat_frm = fopen("stat_frame.dat", "a")) == NULL)       // append new statistic at the end
    {
      snprintf(errortext, ET_SIZE, "Error open file %s  \n", "stat_frame.dat.dat");
      error(errortext, 500);
    }
    else                                            // Create header for new log file
    {
      fprintf(p_stat_frm, " --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- \n");
      fprintf(p_stat_frm, "|            Encoder statistics. This file is generated during first encoding session, new sessions will be appended                                                                                                                                                                                                                                                                                                                                                              |\n");
      fprintf(p_stat_frm, " --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- \n");
    }
  }
  else
  {
    fclose (p_stat_frm);
    if ((p_stat_frm = fopen("stat_frame.dat", "a")) == NULL)       // File exists, just open for appending
    {
      snprintf(errortext, ET_SIZE, "Error open file %s  \n", "stat_frame.dat.dat");
      error(errortext, 500);
    }
  }

  if (frame_statistic_start)
  {
    fprintf(p_stat_frm, "|     ver     | Date  | Time  |    Sequence                  |Frm | QP |P/MbInt|   Bits   |  SNRY  |  SNRU  |  SNRV  |  I4  |  I8  | I16  | IC0  | IC1  | IC2  | IC3  | PI4  | PI8  | PI16 |  P0  |  P1  |  P2  |  P3  | P1*8*| P1*4*| P2*8*| P2*4*| P3*8*| P3*4*|  P8  | P8:4 | P4*8*| P4*4*| P8:5 | P8:6 | P8:7 | BI4  | BI8  | BI16 |  B0  |  B1  |  B2  |  B3  | B0*8*| B0*4*| B1*8*| B1*4*| B2*8*| B2*4*| B3*8*| B3*4*|  B8  | B8:0 |B80*8*|B80*4*| B8:4 | B4*8*| B4*4*| B8:5 | B8:6 | B8:7 |\n");
    fprintf(p_stat_frm, " ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ \n");
  }

  //report
  fprintf(p_stat_frm, "|%4s/%s", VERSION, EXT_VERSION);

#ifdef WIN32
  _strdate( timebuf );
  fprintf(p_stat_frm, "| %1.5s |", timebuf);

  _strtime( timebuf);
  fprintf(p_stat_frm, " % 1.5s |", timebuf);
#else
  now = time ((time_t *) NULL); // Get the system time and put it into 'now' as 'calender time'
  time (&now);
  l_time = localtime (&now);
  strftime (string, sizeof string, "%d-%b-%Y", l_time);
  fprintf(p_stat_frm, "| %1.5s |", string );

  strftime (string, sizeof string, "%H:%M:%S", l_time);
  fprintf(p_stat_frm, " %1.5s |", string);
#endif

  for (i=0;i<30;i++)
    name[i]=params->infile[i + imax(0,(int) (strlen(params->infile)- 30))]; // write last part of path, max 30 chars

  fprintf(p_stat_frm, "%30.30s|", name);

  fprintf(p_stat_frm, "%3d |", frame_no);

  fprintf(p_stat_frm, "%3d |", img->qp);

  fprintf(p_stat_frm, "  %d/%d  |", params->PicInterlace, params->MbInterlace);


  if (img->frm_number == 0 && img->frame_num == 0)
  {
    bitcounter = (int) stats->bit_ctr_I;
  }
  else
  {
    bitcounter = (int) (stats->bit_ctr_n - last_bit_ctr_n);
    last_bit_ctr_n = stats->bit_ctr_n;
  }

  //report bitrate
  fprintf(p_stat_frm, " %9d|", bitcounter);

  //report snr's
  fprintf(p_stat_frm, " %2.4f| %2.4f| %2.4f|", snr->snr_y, snr->snr_u, snr->snr_v);

  //report modes
  //I-Modes
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[I_SLICE][I4MB] - last_mode_use[I_SLICE][I4MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[I_SLICE][I8MB] - last_mode_use[I_SLICE][I8MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[I_SLICE][I16MB] - last_mode_use[I_SLICE][I16MB]);

  //chroma intra mode
  fprintf(p_stat_frm, " %5d|", stats->intra_chroma_mode[0] - last_mode_chroma_use[0]);
  fprintf(p_stat_frm, " %5d|", stats->intra_chroma_mode[1] - last_mode_chroma_use[1]);
  fprintf(p_stat_frm, " %5d|", stats->intra_chroma_mode[2] - last_mode_chroma_use[2]);
  fprintf(p_stat_frm, " %5d|", stats->intra_chroma_mode[3] - last_mode_chroma_use[3]);

  //P-Modes
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][I4MB] - last_mode_use[P_SLICE][I4MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][I8MB] - last_mode_use[P_SLICE][I8MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][I16MB] - last_mode_use[P_SLICE][I16MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][0   ] - last_mode_use[P_SLICE][0   ]);

  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][1   ] - last_mode_use[P_SLICE][1   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][2   ] - last_mode_use[P_SLICE][2   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][3   ] - last_mode_use[P_SLICE][3   ]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][0][1]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][0][1]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][0][2]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][0][2]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][0][3]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][0][3]);

  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][P8x8] - last_mode_use[P_SLICE][P8x8]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][4   ] - last_mode_use[P_SLICE][4   ]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][0][4]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][0][4]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][5   ] - last_mode_use[P_SLICE][5   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][6   ] - last_mode_use[P_SLICE][6   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[P_SLICE][7   ] - last_mode_use[P_SLICE][7   ]);

  //B-Modes
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][I4MB] - last_mode_use[B_SLICE][I4MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][I8MB] - last_mode_use[B_SLICE][I8MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][I16MB] - last_mode_use[B_SLICE][I16MB]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][0   ] - last_mode_use[B_SLICE][0   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][1   ] - last_mode_use[B_SLICE][1   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][2   ] - last_mode_use[B_SLICE][2   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][3   ] - last_mode_use[B_SLICE][3   ]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][1][0]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][1][0]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][1][1]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][1][1]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][1][2]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][1][2]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][1][3]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][1][3]);

  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][P8x8] - last_mode_use[B_SLICE][P8x8]);
  fprintf(p_stat_frm, " %d|", (stats->b8_mode_0_use [B_SLICE][0]+stats->b8_mode_0_use [B_SLICE][1]) - (last_b8_mode_0[B_SLICE][0]+last_b8_mode_0[B_SLICE][1]));
  fprintf(p_stat_frm, " %5d|", stats->b8_mode_0_use [B_SLICE][1] - last_b8_mode_0[B_SLICE][1]);
  fprintf(p_stat_frm, " %5d|", stats->b8_mode_0_use [B_SLICE][0] - last_b8_mode_0[B_SLICE][0]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][4   ] - last_mode_use[B_SLICE][4   ]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[1][1][4]);
  fprintf(p_stat_frm, " %5d|", stats->mode_use_transform[0][1][4]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][5   ] - last_mode_use[B_SLICE][5   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][6   ] - last_mode_use[B_SLICE][6   ]);
  fprintf(p_stat_frm, " %5" FORMAT_OFF_T  "|", stats->mode_use[B_SLICE][7   ] - last_mode_use[B_SLICE][7   ]);

  fprintf(p_stat_frm, "\n");

  //save the last results
  memcpy(last_mode_use[I_SLICE], stats->mode_use[I_SLICE], MAXMODE *  sizeof(int64));
  memcpy(last_mode_use[P_SLICE], stats->mode_use[P_SLICE], MAXMODE *  sizeof(int64));
  memcpy(last_mode_use[B_SLICE], stats->mode_use[B_SLICE], MAXMODE *  sizeof(int64));
  memset(stats->mode_use_transform[1], 0, 2 * MAXMODE *  sizeof(int));
  memset(stats->mode_use_transform[0], 0, 2 * MAXMODE *  sizeof(int));
  memcpy(last_b8_mode_0[B_SLICE], stats->b8_mode_0_use[B_SLICE], 2 *  sizeof(int));
  memcpy(last_mode_chroma_use, stats->intra_chroma_mode, 4 *  sizeof(int));

  frame_statistic_start = 0;
  fclose(p_stat_frm);
}


/*!
 ************************************************************************
 * \brief
 *    Reports the gathered information to appropriate outputs
 * \par Input:
 *    struct inp_par *inp,                                            \n
 *    ImageParameters *img,                                            \n
 *    struct stat_par *stats,                                          \n
 *    struct stat_par *stats                                           \n
 *
 * \par Output:
 *    None
 ************************************************************************
 */
void report()
{
  int64 bit_use[NUM_PIC_TYPE][2] ;
  int i,j;
  char name[40];
  int64 total_bits;
  float frame_rate;
  double mean_motion_info_bit_use[2] = {0.0};

#ifndef WIN32
  time_t now;
  struct tm *l_time;
  char string[1000];
#else
  char timebuf[128];
#endif
  bit_use[I_SLICE][0] = frame_ctr[I_SLICE];
  bit_use[P_SLICE][0] = imax(frame_ctr[P_SLICE] + frame_ctr[SP_SLICE],1);
  bit_use[B_SLICE][0] = frame_ctr[B_SLICE];

  //  Accumulate bit usage for inter and intra frames
  for (j=0; j < NUM_PIC_TYPE; j++)
  {
    bit_use[j][1] = 0;
  }

  for (j=0; j < NUM_PIC_TYPE; j++)
  {
    for(i=0; i < MAXMODE; i++)
      bit_use[j][1] += stats->bit_use_mode[j][i];

    bit_use[j][1] += stats->bit_use_header[j];
    bit_use[j][1] += stats->bit_use_mb_type[j];
    bit_use[j][1] += stats->tmp_bit_use_cbp[j];
    bit_use[j][1] += stats->bit_use_coeffC[j];
    bit_use[j][1] += stats->bit_use_coeff[0][j];   
    bit_use[j][1] += stats->bit_use_coeff[1][j]; 
    bit_use[j][1] += stats->bit_use_coeff[2][j]; 
    bit_use[j][1] += stats->bit_use_delta_quant[j];
    bit_use[j][1] += stats->bit_use_stuffingBits[j];
  }

  frame_rate = (img->framerate *(float)(stats->successive_Bframe + 1)) / (float) (params->jumpd+1);

  //! Currently adding NVB bits on P rate. Maybe additional stats info should be created instead and added in log file
  stats->bitrate_I = (stats->bit_ctr_I)*(frame_rate)/(float) (params->no_frames + frame_ctr[B_SLICE]);
  stats->bitrate_P = (stats->bit_ctr_P)*(frame_rate)/(float) (params->no_frames + frame_ctr[B_SLICE]);
  stats->bitrate_B = (stats->bit_ctr_B)*(frame_rate)/(float) (params->no_frames + frame_ctr[B_SLICE]);

  switch (params->Verbose)
  {
  case 0:
  case 1:
  default:
    fprintf(stdout,"------------------ Average data all frames  -----------------------------------\n\n");
    break;
  case 2:
    fprintf(stdout,"------------------------------------  Average data all frames  ---------------------------------\n\n");
    break;
  }

  if (params->Verbose != 0)
  {
    int  impix    = params->output.size_cmp[0];
    int  impix_cr = params->output.size_cmp[1];
    unsigned int max_pix_value_sqd = img->max_imgpel_value_comp[0] * img->max_imgpel_value_comp[0];
    unsigned int max_pix_value_sqd_uv = img->max_imgpel_value_comp[1] * img->max_imgpel_value_comp[1];
    float csnr_y = (float) (10 * log10 (max_pix_value_sqd *
      (double)((double) impix / (snr->msse_y == 0.0? 1.0 : snr->msse_y))));
    float csnr_u = (float) (10 * log10 (max_pix_value_sqd_uv *
      (double)((double) impix_cr / (snr->msse_u == 0.0? 1.0 : snr->msse_u))));
    float csnr_v = (float) (10 * log10 (max_pix_value_sqd_uv *
      (double)((double) impix_cr / (snr->msse_v == 0.0? 1.0 : snr->msse_v))));
    fprintf(stdout,  " Total encoding time for the seq.  : %.3f sec (%.2f fps)\n", tot_time*0.001, 1000.0*(params->no_frames + frame_ctr[B_SLICE])/tot_time);
    fprintf(stdout,  " Total ME time for sequence        : %.3f sec \n\n", me_tot_time*0.001);

    fprintf(stdout," PSNR Y(dB)                        : %5.2f\n", snr->snr_ya);
    fprintf(stdout," PSNR U(dB)                        : %5.2f\n", snr->snr_ua);
    fprintf(stdout," PSNR V(dB)                        : %5.2f\n", snr->snr_va);
    fprintf(stdout," cSNR Y(dB)                        : %5.2f (%5.2f)\n", csnr_y,snr->msse_y/impix);
    fprintf(stdout," cSNR U(dB)                        : %5.2f (%5.2f)\n", csnr_u,snr->msse_u/impix_cr);
    fprintf(stdout," cSNR V(dB)                        : %5.2f (%5.2f)\n\n", csnr_v,snr->msse_v/impix_cr);
  }
  else
    fprintf(stdout,  " Total encoding time for the seq.  : %.3f sec (%.2f fps)\n\n", tot_time*0.001, 1000.0*(params->no_frames + frame_ctr[B_SLICE])/tot_time);

  if (frame_ctr[B_SLICE] != 0)
  {
    fprintf(stdout, " Total bits                        : %" FORMAT_OFF_T  " (I %" FORMAT_OFF_T  ", P %" FORMAT_OFF_T  ", B %" FORMAT_OFF_T  " NVB %d) \n",
      total_bits=stats->bit_ctr_P + stats->bit_ctr_I + stats->bit_ctr_B + stats->bit_ctr_parametersets,
      stats->bit_ctr_I, stats->bit_ctr_P, stats->bit_ctr_B, stats->bit_ctr_parametersets);

    frame_rate = (img->framerate *(float)(stats->successive_Bframe + 1)) / (float) (params->jumpd+1);
    //    stats->bitrate= ((float) total_bits * frame_rate)/((float) (params->no_frames + frame_ctr[B_SLICE]));
    stats->bitrate= ((float) total_bits * frame_rate) / ((float)(frame_ctr[I_SLICE] + frame_ctr[P_SLICE] + frame_ctr[B_SLICE]));

    fprintf(stdout, " Bit rate (kbit/s)  @ %2.2f Hz     : %5.2f\n", frame_rate, stats->bitrate/1000);

  }
  else if (params->sp_periodicity == 0)
  {
    fprintf(stdout, " Total bits                        : %" FORMAT_OFF_T  " (I %" FORMAT_OFF_T  ", P %" FORMAT_OFF_T  ", NVB %d) \n",
      total_bits=stats->bit_ctr_P + stats->bit_ctr_I + stats->bit_ctr_parametersets, stats->bit_ctr_I, stats->bit_ctr_P, stats->bit_ctr_parametersets);


    frame_rate = img->framerate / ( (float) (params->jumpd + 1) );
    stats->bitrate= ((float) total_bits * frame_rate) / ((float) params->no_frames );

    fprintf(stdout, " Bit rate (kbit/s)  @ %2.2f Hz     : %5.2f\n", frame_rate, stats->bitrate/1000);
  }
  else
  {
    fprintf(stdout, " Total bits                        : %" FORMAT_OFF_T  " (I %" FORMAT_OFF_T  ", P %" FORMAT_OFF_T  ", NVB %d) \n",
      total_bits=stats->bit_ctr_P + stats->bit_ctr_I + stats->bit_ctr_parametersets, stats->bit_ctr_I, stats->bit_ctr_P, stats->bit_ctr_parametersets);


    frame_rate = img->framerate / ( (float) (params->jumpd + 1) );
    stats->bitrate= ((float) total_bits * frame_rate)/((float) params->no_frames );

    fprintf(stdout, " Bit rate (kbit/s)  @ %2.2f Hz     : %5.2f\n", frame_rate, stats->bitrate/1000);
  }

  fprintf(stdout, " Bits to avoid Startcode Emulation : %d \n", stats->bit_ctr_emulationprevention);
  fprintf(stdout, " Bits for parameter sets           : %d \n\n", stats->bit_ctr_parametersets);

  switch (params->Verbose)
  {
  case 0:
  case 1:
  default:
    fprintf(stdout,"-------------------------------------------------------------------------------\n");
    break;
  case 2:
    fprintf(stdout,"------------------------------------------------------------------------------------------------\n");
    break;
  }  
  fprintf(stdout,"Exit JM %s encoder ver %s ", JM, VERSION);
  fprintf(stdout,"\n");

  // status file
  if (strlen(params->StatsFile) == 0)
    strcpy (params->StatsFile,"stats.dat");

  if ((p_stat = fopen(params->StatsFile, "wt")) == 0)
  {
    snprintf(errortext, ET_SIZE, "Error open file %s", params->StatsFile);
    error(errortext, 500);
  }

  fprintf(p_stat," -------------------------------------------------------------- \n");
  fprintf(p_stat,"  This file contains statistics for the last encoded sequence   \n");
  fprintf(p_stat," -------------------------------------------------------------- \n");
  fprintf(p_stat,   " Sequence                     : %s\n", params->infile);
  fprintf(p_stat,   " No.of coded pictures         : %4d\n", params->no_frames+frame_ctr[B_SLICE]);
  fprintf(p_stat,   " Freq. for encoded bitstream  : %4.0f\n", frame_rate);

  fprintf(p_stat,   " I Slice Bitrate(kb/s)        : %6.2f\n", stats->bitrate_I/1000);
  fprintf(p_stat,   " P Slice Bitrate(kb/s)        : %6.2f\n", stats->bitrate_P/1000);
  // B pictures
  if (stats->successive_Bframe != 0)
    fprintf(p_stat,   " B Slice Bitrate(kb/s)        : %6.2f\n", stats->bitrate_B/1000);
  fprintf(p_stat,   " Total Bitrate(kb/s)          : %6.2f\n", stats->bitrate/1000);

  for (i = 0; i < 3; i++)
  {
    fprintf(p_stat," ME Metric for Refinement Level %1d : %s\n", i, DistortionType[params->MEErrorMetric[i]]);
  }
  fprintf(p_stat," Mode Decision Metric             : %s\n", DistortionType[params->ModeDecisionMetric]);

  switch ( params->ChromaMEEnable )
  {
  case 1:
    fprintf(p_stat," Motion Estimation for components : YCbCr\n");
    break;
  default:
    fprintf(p_stat," Motion Estimation for components : Y\n");
    break;
  }

  fprintf(p_stat,  " Image format                 : %dx%d\n", params->output.width, params->output.height);

  if (params->intra_upd)
    fprintf(p_stat," Error robustness             : On\n");
  else
    fprintf(p_stat," Error robustness             : Off\n");

  fprintf(p_stat,  " Search range                 : %d\n", params->search_range);

  fprintf(p_stat,   " Total number of references   : %d\n", params->num_ref_frames);
  fprintf(p_stat,   " References for P slices      : %d\n", params->P_List0_refs ? params->P_List0_refs : params->num_ref_frames);
  if (stats->successive_Bframe != 0)
  {
    fprintf(p_stat, " List0 refs for B slices      : %d\n", params->B_List0_refs ? params->B_List0_refs : params->num_ref_frames);
    fprintf(p_stat, " List1 refs for B slices      : %d\n", params->B_List1_refs ? params->B_List1_refs : params->num_ref_frames);
  }

  if (params->symbol_mode == CAVLC)
    fprintf(p_stat,   " Entropy coding method        : CAVLC\n");
  else
    fprintf(p_stat,   " Entropy coding method        : CABAC\n");

  fprintf(p_stat,   " Profile/Level IDC            : (%d,%d)\n", params->ProfileIDC, params->LevelIDC);
  if (params->MbInterlace)
    fprintf(p_stat, " MB Field Coding : On \n");

  if (params->SearchMode == EPZS)
    EPZSOutputStats(p_stat, 1);

  if (params->full_search == 2)
    fprintf(p_stat," Search range restrictions    : none\n");
  else if (params->full_search == 1)
    fprintf(p_stat," Search range restrictions    : older reference frames\n");
  else
    fprintf(p_stat," Search range restrictions    : smaller blocks and older reference frames\n");

  if (params->rdopt)
    fprintf(p_stat," RD-optimized mode decision   : used\n");
  else
    fprintf(p_stat," RD-optimized mode decision   : not used\n");

  fprintf(p_stat," ---------------------|----------------|---------------|\n");
  fprintf(p_stat,"     Item             |     Intra      |   All frames  |\n");
  fprintf(p_stat," ---------------------|----------------|---------------|\n");
  fprintf(p_stat," SNR Y(dB)            |");
  fprintf(p_stat," %5.2f          |", snr->snr_y1);
  fprintf(p_stat," %5.2f         |\n", snr->snr_ya);
  fprintf(p_stat," SNR U/V (dB)         |");
  fprintf(p_stat," %5.2f/%5.2f    |", snr->snr_u1, snr->snr_v1);
  fprintf(p_stat," %5.2f/%5.2f   |\n", snr->snr_ua, snr->snr_va);

  // QUANT.
  fprintf(p_stat," Average quant        |");
  fprintf(p_stat," %5d          |", iabs(params->qp0));
  fprintf(p_stat," %5.2f         |\n", (float)stats->quant1/dmax(1.0,(float)stats->quant0));

  fprintf(p_stat,"\n ---------------------|----------------|---------------|---------------|\n");
  fprintf(p_stat,"     SNR              |        I       |       P       |       B       |\n");
  fprintf(p_stat," ---------------------|----------------|---------------|---------------|\n");
  fprintf(p_stat," SNR Y(dB)            |      %5.3f    |     %5.3f    |     %5.3f    |\n",
    snr->snr_yt[I_SLICE], snr->snr_yt[P_SLICE], snr->snr_yt[B_SLICE]);
  fprintf(p_stat," SNR U(dB)            |      %5.3f    |     %5.3f    |     %5.3f    |\n",
    snr->snr_ut[I_SLICE], snr->snr_ut[P_SLICE], snr->snr_ut[B_SLICE]);
  fprintf(p_stat," SNR V(dB)            |      %5.3f    |     %5.3f    |     %5.3f    |\n",
    snr->snr_vt[I_SLICE], snr->snr_vt[P_SLICE], snr->snr_vt[B_SLICE]);


  // MODE
  fprintf(p_stat,"\n ---------------------|----------------|\n");
  fprintf(p_stat,"   Intra              |   Mode used    |\n");
  fprintf(p_stat," ---------------------|----------------|\n");

  fprintf(p_stat," Mode 0  intra 4x4    |  %5" FORMAT_OFF_T  "         |\n", stats->mode_use[I_SLICE][I4MB ]);
  fprintf(p_stat," Mode 1  intra 8x8    |  %5" FORMAT_OFF_T  "         |\n", stats->mode_use[I_SLICE][I8MB ]);
  fprintf(p_stat," Mode 2+ intra 16x16  |  %5" FORMAT_OFF_T  "         |\n", stats->mode_use[I_SLICE][I16MB]);
  fprintf(p_stat," Mode    intra IPCM   |  %5" FORMAT_OFF_T  "         |\n", stats->mode_use[I_SLICE][IPCM ]);

  fprintf(p_stat,"\n ---------------------|----------------|-----------------|\n");
  fprintf(p_stat,"   Inter              |   Mode used    | MotionInfo bits |\n");
  fprintf(p_stat," ---------------------|----------------|-----------------|");
  fprintf(p_stat,"\n Mode  0  (copy)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[P_SLICE][0   ], (double)stats->bit_use_mode[P_SLICE][0   ]/(double)bit_use[P_SLICE][0]);
  fprintf(p_stat,"\n Mode  1  (16x16)     |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[P_SLICE][1   ], (double)stats->bit_use_mode[P_SLICE][1   ]/(double)bit_use[P_SLICE][0]);
  fprintf(p_stat,"\n Mode  2  (16x8)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[P_SLICE][2   ], (double)stats->bit_use_mode[P_SLICE][2   ]/(double)bit_use[P_SLICE][0]);
  fprintf(p_stat,"\n Mode  3  (8x16)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[P_SLICE][3   ], (double)stats->bit_use_mode[P_SLICE][3   ]/(double)bit_use[P_SLICE][0]);
  fprintf(p_stat,"\n Mode  4  (8x8)       |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[P_SLICE][P8x8], (double)stats->bit_use_mode[P_SLICE][P8x8]/(double)bit_use[P_SLICE][0]);
  fprintf(p_stat,"\n Mode  5  intra 4x4   |  %5" FORMAT_OFF_T  "         |-----------------|", stats->mode_use[P_SLICE][I4MB]);
  fprintf(p_stat,"\n Mode  6  intra 8x8   |  %5" FORMAT_OFF_T  "         |", stats->mode_use[P_SLICE][I8MB]);
  fprintf(p_stat,"\n Mode  7+ intra 16x16 |  %5" FORMAT_OFF_T  "         |", stats->mode_use[P_SLICE][I16MB]);
  fprintf(p_stat,"\n Mode     intra IPCM  |  %5" FORMAT_OFF_T  "         |", stats->mode_use[P_SLICE][IPCM ]);
  mean_motion_info_bit_use[0] = (double)(stats->bit_use_mode[P_SLICE][0] + stats->bit_use_mode[P_SLICE][1] + stats->bit_use_mode[P_SLICE][2]
  + stats->bit_use_mode[P_SLICE][3] + stats->bit_use_mode[P_SLICE][P8x8])/(double) bit_use[P_SLICE][0];

  // B pictures
  if ((stats->successive_Bframe!=0) && (frame_ctr[B_SLICE]!=0))
  {

    fprintf(p_stat,"\n\n ---------------------|----------------|-----------------|\n");
    fprintf(p_stat,"   B frame            |   Mode used    | MotionInfo bits |\n");
    fprintf(p_stat," ---------------------|----------------|-----------------|");
    fprintf(p_stat,"\n Mode  0  (copy)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[B_SLICE][0   ], (double)stats->bit_use_mode[B_SLICE][0   ]/(double)frame_ctr[B_SLICE]);
    fprintf(p_stat,"\n Mode  1  (16x16)     |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[B_SLICE][1   ], (double)stats->bit_use_mode[B_SLICE][1   ]/(double)frame_ctr[B_SLICE]);
    fprintf(p_stat,"\n Mode  2  (16x8)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[B_SLICE][2   ], (double)stats->bit_use_mode[B_SLICE][2   ]/(double)frame_ctr[B_SLICE]);
    fprintf(p_stat,"\n Mode  3  (8x16)      |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[B_SLICE][3   ], (double)stats->bit_use_mode[B_SLICE][3   ]/(double)frame_ctr[B_SLICE]);
    fprintf(p_stat,"\n Mode  4  (8x8)       |  %5" FORMAT_OFF_T  "         |    %8.2f     |", stats->mode_use[B_SLICE][P8x8], (double)stats->bit_use_mode[B_SLICE][P8x8]/(double)frame_ctr[B_SLICE]);
    fprintf(p_stat,"\n Mode  5  intra 4x4   |  %5" FORMAT_OFF_T  "         |-----------------|", stats->mode_use[B_SLICE][I4MB]);
    fprintf(p_stat,"\n Mode  6  intra 8x8   |  %5" FORMAT_OFF_T  "         |", stats->mode_use[B_SLICE][I8MB]);
    fprintf(p_stat,"\n Mode  7+ intra 16x16 |  %5" FORMAT_OFF_T  "         |", stats->mode_use[B_SLICE][I16MB]);
    fprintf(p_stat,"\n Mode     intra IPCM  |  %5" FORMAT_OFF_T  "         |", stats->mode_use[B_SLICE][IPCM ]);
    mean_motion_info_bit_use[1] = (double)(stats->bit_use_mode[B_SLICE][0] + stats->bit_use_mode[B_SLICE][1] + stats->bit_use_mode[B_SLICE][2]
    + stats->bit_use_mode[B_SLICE][3] + stats->bit_use_mode[B_SLICE][P8x8])/(double) frame_ctr[B_SLICE];
  }

  fprintf(p_stat,"\n\n ---------------------|----------------|----------------|----------------|\n");
  fprintf(p_stat,"  Bit usage:          |      Intra     |      Inter     |    B frame     |\n");
  fprintf(p_stat," ---------------------|----------------|----------------|----------------|\n");

  fprintf(p_stat," Header               |");
  fprintf(p_stat," %10.2f     |", (float) stats->bit_use_header[I_SLICE]/bit_use[I_SLICE][0]);
  fprintf(p_stat," %10.2f     |", (float) stats->bit_use_header[P_SLICE]/bit_use[P_SLICE][0]);
  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," %10.2f     |", (float) stats->bit_use_header[B_SLICE]/frame_ctr[B_SLICE]);
  else fprintf(p_stat," %10.2f     |", 0.);
  fprintf(p_stat,"\n");

  fprintf(p_stat," Mode                 |");
  fprintf(p_stat," %10.2f     |", (float)stats->bit_use_mb_type[I_SLICE]/bit_use[I_SLICE][0]);
  fprintf(p_stat," %10.2f     |", (float)stats->bit_use_mb_type[P_SLICE]/bit_use[P_SLICE][0]);
  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," %10.2f     |", (float)stats->bit_use_mb_type[B_SLICE]/frame_ctr[B_SLICE]);
  else fprintf(p_stat," %10.2f     |", 0.);
  fprintf(p_stat,"\n");

  fprintf(p_stat," Motion Info          |");
  fprintf(p_stat,"        ./.     |");
  fprintf(p_stat," %10.2f     |", mean_motion_info_bit_use[0]);
  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," %10.2f     |", mean_motion_info_bit_use[1]);
  else fprintf(p_stat," %10.2f     |", 0.);
  fprintf(p_stat,"\n");

  fprintf(p_stat," CBP Y/C              |");
  fprintf(p_stat," %10.2f     |", (float)stats->tmp_bit_use_cbp[I_SLICE]/bit_use[I_SLICE][0]);
  fprintf(p_stat," %10.2f     |", (float)stats->tmp_bit_use_cbp[P_SLICE]/bit_use[P_SLICE][0]);
  if ((stats->successive_Bframe != 0) && (bit_use[B_SLICE][0] != 0))
    fprintf(p_stat," %10.2f     |", (float)stats->tmp_bit_use_cbp[B_SLICE]/bit_use[B_SLICE][0]);
  else fprintf(p_stat," %10.2f     |", 0.);
  fprintf(p_stat,"\n");

  if (stats->successive_Bframe != 0 && frame_ctr[B_SLICE] != 0)
    fprintf(p_stat," Coeffs. Y            | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_coeff[0][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[0][P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_coeff[0][B_SLICE]/frame_ctr[B_SLICE]);
  else
    fprintf(p_stat," Coeffs. Y            | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_coeff[0][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[0][P_SLICE]/(float)bit_use[P_SLICE][0], 0.);

  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," Coeffs. C            | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_coeffC[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeffC[P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_coeffC[B_SLICE]/frame_ctr[B_SLICE]);
  else
    fprintf(p_stat," Coeffs. C            | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_coeffC[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeffC[P_SLICE]/bit_use[P_SLICE][0], 0.);
  if(img->P444_joined) 
  {
    if(stats->successive_Bframe!=0 && frame_ctr[B_SLICE]!=0)
      fprintf(p_stat," Coeffs. CB           | %10.2f     | %10.2f     | %10.2f     |\n",
      (float)stats->bit_use_coeff[1][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[1][P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_coeff[1][B_SLICE]/frame_ctr[B_SLICE]);
    else
      fprintf(p_stat," Coeffs. CB           | %10.2f     | %10.2f     | %10.2f     |\n",
      (float)stats->bit_use_coeff[1][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[1][P_SLICE]/bit_use[P_SLICE][0], 0.);

    if(stats->successive_Bframe!=0 && frame_ctr[B_SLICE]!=0)
      fprintf(p_stat," Coeffs. CR           | %10.2f     | %10.2f     | %10.2f     |\n",
      (float)stats->bit_use_coeff[2][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[2][P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_coeff[2][B_SLICE]/frame_ctr[B_SLICE]);
    else
      fprintf(p_stat," Coeffs. CR           | %10.2f     | %10.2f     | %10.2f     |\n",
      (float)stats->bit_use_coeff[2][I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_coeff[2][P_SLICE]/bit_use[P_SLICE][0], 0.);
  }

  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," Delta quant          | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_delta_quant[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_delta_quant[P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_delta_quant[B_SLICE]/frame_ctr[B_SLICE]);
  else
    fprintf(p_stat," Delta quant          | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_delta_quant[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_delta_quant[P_SLICE]/bit_use[P_SLICE][0], 0.);

  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0))
    fprintf(p_stat," Stuffing Bits        | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_stuffingBits[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_stuffingBits[P_SLICE]/bit_use[P_SLICE][0], (float)stats->bit_use_stuffingBits[B_SLICE]/frame_ctr[B_SLICE]);
  else
    fprintf(p_stat," Stuffing Bits        | %10.2f     | %10.2f     | %10.2f     |\n",
    (float)stats->bit_use_stuffingBits[I_SLICE]/bit_use[I_SLICE][0], (float)stats->bit_use_stuffingBits[P_SLICE]/bit_use[P_SLICE][0], 0.);



  fprintf(p_stat," ---------------------|----------------|----------------|----------------|\n");

  fprintf(p_stat," average bits/frame   |");

  fprintf(p_stat," %10.2f     |", (float) bit_use[I_SLICE][1]/(float) bit_use[I_SLICE][0] );
  fprintf(p_stat," %10.2f     |", (float) bit_use[P_SLICE][1]/(float) bit_use[P_SLICE][0] );

  if(stats->successive_Bframe!=0 && frame_ctr[B_SLICE]!=0)
    fprintf(p_stat," %10.2f     |", (float) bit_use[B_SLICE][1]/ (float) frame_ctr[B_SLICE] );
  else fprintf(p_stat," %10.2f     |", 0.);

  fprintf(p_stat,"\n");
  fprintf(p_stat," ---------------------|----------------|----------------|----------------|\n");

  fclose(p_stat);

  // write to log file
  if ((p_log = fopen("log.dat", "r")) == 0)         // check if file exists
  {
    if ((p_log = fopen("log.dat", "a")) == NULL)    // append new statistic at the end
    {
      snprintf(errortext, ET_SIZE, "Error open file %s  \n", "log.dat");
      error(errortext, 500);
    }
    else                                            // Create header for new log file
    {
      fprintf(p_log," ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ \n");
      fprintf(p_log,"|                          Encoder statistics. This file is generated during first encoding session, new sessions will be appended                                                                                                                                                                                 |\n");
      fprintf(p_log," ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ \n");
      fprintf(p_log,"|     ver     | Date  | Time  |               Sequence                 | #Img |P/MbInt| QPI| QPP| QPB| Format  |Iperiod| #B | FMES | Hdmd | S.R |#Ref | Freq |Coding|RD-opt|Intra upd|8x8Tr| SNRY 1| SNRU 1| SNRV 1| SNRY N| SNRU N| SNRV N|#Bitr I|#Bitr P|#Bitr B|#Bitr IPB|     Total Time   |      Me Time     |\n");
      fprintf(p_log," ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ \n");

    }
  }
  else
  {
    fclose (p_log);
    if ((p_log = fopen("log.dat", "a")) == NULL)         // File exists, just open for appending
    {
      snprintf(errortext, ET_SIZE, "Error open file %s  \n", "log.dat");
      error(errortext, 500);
    }
  }
  fprintf(p_log,"|%5s/%-5s", VERSION, EXT_VERSION);

#ifdef WIN32
  _strdate( timebuf );
  fprintf(p_log,"| %1.5s |", timebuf );

  _strtime( timebuf);
  fprintf(p_log," % 1.5s |", timebuf);
#else
  now = time ((time_t *) NULL); // Get the system time and put it into 'now' as 'calender time'
  time (&now);
  l_time = localtime (&now);
  strftime (string, sizeof string, "%d-%b-%Y", l_time);
  fprintf(p_log,"| %1.5s |", string );

  strftime (string, sizeof string, "%H:%M:%S", l_time);
  fprintf(p_log," %1.5s |", string );
#endif

  for (i=0; i < 40; i++)
    name[i] = params->infile[i + imax(0, ((int) strlen(params->infile)) - 40)]; // write last part of path, max 40 chars
  fprintf(p_log,"%40.40s|",name);

  fprintf(p_log,"%5d |  %d/%d  |", params->no_frames, params->PicInterlace, params->MbInterlace);
  fprintf(p_log," %-3d| %-3d| %-3d|", params->qp0, params->qpN, params->qpB);

  fprintf(p_log,"%4dx%-4d|", params->output.width, params->output.height);
  fprintf(p_log,"  %3d  |%3d |", params->intra_period, stats->successive_Bframe);


  switch( params->SearchMode ) 
  {
    case UM_HEX:
      fprintf(p_log,"  HEX |");
      break;
    case UM_HEX_SIMPLE:
      fprintf(p_log," SHEX |");
      break;
    case EPZS:
      fprintf(p_log," EPZS |");
      break;
    case FAST_FULL_SEARCH:
      fprintf(p_log,"  FFS |");
      break;
    default:
      fprintf(p_log,"  FS  |");
      break;
  }

  fprintf(p_log,"  %1d%1d%1d |", params->MEErrorMetric[F_PEL], params->MEErrorMetric[H_PEL], params->MEErrorMetric[Q_PEL]);

  fprintf(p_log," %3d | %2d  |", params->search_range, params->num_ref_frames );

  fprintf(p_log," %5.2f|", (img->framerate *(float) (stats->successive_Bframe + 1)) / (float)(params->jumpd + 1));

  if (params->symbol_mode == CAVLC)
    fprintf(p_log," CAVLC|");
  else
    fprintf(p_log," CABAC|");

  fprintf(p_log,"   %d  |", params->rdopt);

  if (params->intra_upd == 1)
    fprintf(p_log,"   ON    |");
  else
    fprintf(p_log,"   OFF   |");

  fprintf(p_log,"  %d  |", params->Transform8x8Mode);

  fprintf(p_log,"%7.3f|%7.3f|%7.3f|", snr->snr_y1,snr->snr_u1,snr->snr_v1);
  fprintf(p_log,"%7.3f|%7.3f|%7.3f|", snr->snr_ya,snr->snr_ua,snr->snr_va);
  /*
  fprintf(p_log,"%-5.3f|%-5.3f|%-5.3f|", snr->snr_yt[I_SLICE], snr->snr_ut[I_SLICE], snr->snr_vt[I_SLICE]);
  fprintf(p_log,"%-5.3f|%-5.3f|%-5.3f|", snr->snr_yt[P_SLICE], snr->snr_ut[P_SLICE], snr->snr_vt[P_SLICE]);
  fprintf(p_log,"%-5.3f|%-5.3f|%-5.3f|", snr->snr_yt[B_SLICE], snr->snr_ut[B_SLICE], snr->snr_vt[B_SLICE]);
  */
  fprintf(p_log,"%7.0f|%7.0f|%7.0f|%9.0f|", stats->bitrate_I,stats->bitrate_P,stats->bitrate_B, stats->bitrate);

  fprintf(p_log,"   %12d   |   %12d   |", (int)tot_time,(int)me_tot_time);


  fprintf(p_log,"\n");

  fclose(p_log);

  p_log = fopen("data.txt", "a");

  if ((stats->successive_Bframe != 0) && (frame_ctr[B_SLICE] != 0)) // B picture used
  {
    fprintf(p_log, "%3d %2d %2d %2.2f %2.2f %2.2f %5" FORMAT_OFF_T  " "
      "%2.2f %2.2f %2.2f %5d "
      "%2.2f %2.2f %2.2f %5" FORMAT_OFF_T  " %5" FORMAT_OFF_T  " %.3f\n",
      params->no_frames, params->qp0, params->qpN,
      snr->snr_y1,
      snr->snr_u1,
      snr->snr_v1,
      stats->bit_ctr_I,
      0.0,
      0.0,
      0.0,
      0,
      snr->snr_ya,
      snr->snr_ua,
      snr->snr_va,
      (stats->bit_ctr_I+stats->bit_ctr)/(params->no_frames+frame_ctr[B_SLICE]),
      stats->bit_ctr_B/frame_ctr[B_SLICE],
      (double)0.001*tot_time/(params->no_frames+frame_ctr[B_SLICE]));
  }
  else
  {
    if (params->no_frames != 0)
      fprintf(p_log, "%3d %2d %2d %2.2f %2.2f %2.2f %5" FORMAT_OFF_T  " "
      "%2.2f %2.2f %2.2f %5d "
      "%2.2f %2.2f %2.2f %5" FORMAT_OFF_T  " %5d %.3f\n",
      params->no_frames, params->qp0, params->qpN,
      snr->snr_y1,
      snr->snr_u1,
      snr->snr_v1,
      stats->bit_ctr_I,
      0.0,
      0.0,
      0.0,
      0,
      snr->snr_ya,
      snr->snr_ua,
      snr->snr_va,
      (stats->bit_ctr_I+stats->bit_ctr)/params->no_frames,
      0,
      (double)0.001*tot_time/params->no_frames);
  }

  fclose(p_log);

  if (params->ReportFrameStats)
  {
    if ((p_log = fopen("stat_frame.dat", "a")) == NULL)       // append new statistic at the end
    {
      snprintf(errortext, ET_SIZE, "Error open file %s  \n", "stat_frame.dat.dat");
      //    error(errortext, 500);
    }
    else
    {
      fprintf(p_log," --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- \n");
      fclose(p_log);
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Prints the header of the protocol.
 * \par Input:
 *    struct inp_par *inp
 * \par Output:
 *    none
 ************************************************************************
 */
void information_init(void)
{
  int i;
  static char yuv_types[4][10] = {"YUV 4:0:0", "YUV 4:2:0", "YUV 4:2:2", "YUV 4:4:4"};
  switch (params->Verbose)
  {
  case 0:
  case 1:
  default:
    printf("------------------------------- JM %4.4s %7.7s -------------------------------\n", VERSION, EXT_VERSION);
    break;
  case 2:
    printf("--------------------------------------- JM %4.4s %7.7s ----------------------------------------\n", VERSION, EXT_VERSION);
    break;
  }

  fprintf(stdout,  " Input YUV file                    : %s \n", params->infile);
  fprintf(stdout,  " Output H.264 bitstream            : %s \n", params->outfile);
  if (p_dec != -1)
    fprintf(stdout,  " Output YUV file                   : %s \n", params->ReconFile);
  fprintf(stdout,  " YUV Format                        : %s \n", &yuv_types[img->yuv_format][0]);//img->yuv_format==YUV422?"YUV 4:2:2":(img->yuv_format==YUV444)?"YUV 4:4:4":"YUV 4:2:0");
  fprintf(stdout,  " Frames to be encoded I-P/B        : %d/%d\n", params->no_frames, (params->successive_Bframe*(params->no_frames-1)));
  if (params->Verbose != 0)
  {
    fprintf(stdout,  " Freq. for encoded bitstream       : %1.0f\n", img->framerate/(float)(params->jumpd+1));
    fprintf(stdout,  " PicInterlace / MbInterlace        : %d/%d\n", params->PicInterlace, params->MbInterlace);
    fprintf(stdout,  " Transform8x8Mode                  : %d\n", params->Transform8x8Mode);

    for (i=0; i<3; i++)
    {
      fprintf(stdout," ME Metric for Refinement Level %1d  : %s\n", i, DistortionType[params->MEErrorMetric[i]]);
    }
    fprintf(stdout,  " Mode Decision Metric              : %s\n", DistortionType[params->ModeDecisionMetric]);

    switch ( params->ChromaMEEnable )
    {
    case 1:
      fprintf(stdout," Motion Estimation for components  : YCbCr\n");
      break;
    default:
      fprintf(stdout," Motion Estimation for components  : Y\n");
      break;
    }

    fprintf(stdout,  " Image format                      : %dx%d (%dx%d)\n", params->output.width, params->output.height, img->width,img->height);

    if (params->intra_upd)
      fprintf(stdout," Error robustness                  : On\n");
    else
      fprintf(stdout," Error robustness                  : Off\n");
    fprintf(stdout,  " Search range                      : %d\n", params->search_range);

    fprintf(stdout,  " Total number of references        : %d\n", params->num_ref_frames);
    fprintf(stdout,  " References for P slices           : %d\n", params->P_List0_refs? params->P_List0_refs:params->num_ref_frames);
    if (stats->successive_Bframe != 0)
    {
      fprintf(stdout," List0 references for B slices     : %d\n", params->B_List0_refs? params->B_List0_refs:params->num_ref_frames);
      fprintf(stdout," List1 references for B slices     : %d\n", params->B_List1_refs? params->B_List1_refs:params->num_ref_frames);
    }

    // B pictures
    fprintf(stdout,  " Sequence type                     :");

    if (stats->successive_Bframe > 0 && params->HierarchicalCoding)
    {
      fprintf(stdout, " Hierarchy (QP: I %d, P %d, B %d) \n",
        params->qp0, params->qpN, params->qpB);
    }
    else if (stats->successive_Bframe > 0)
    {
      char seqtype[80];
      int i,j;

      strcpy (seqtype,"I");

      for (j=0; j < 2; j++)
      {
        for (i=0; i < stats->successive_Bframe; i++)
        {
          if (params->BRefPictures)
            strncat(seqtype,"-RB", imax(0, (int) (79-strlen(seqtype))));
          else
            strncat(seqtype,"-B", imax(0, (int) (79-strlen(seqtype))));
        }
        strncat(seqtype,"-P", imax(0, (int) (79-strlen(seqtype))));
      }
      if (params->BRefPictures)
        fprintf(stdout, " %s (QP: I %d, P %d, RB %d) \n", seqtype, params->qp0, params->qpN, iClip3(0, 51, params->qpB + params->qpBRSOffset));
      else
        fprintf(stdout, " %s (QP: I %d, P %d, B %d) \n", seqtype, params->qp0, params->qpN, params->qpB);
    }
    else if (stats->successive_Bframe == 0 && params->sp_periodicity == 0) fprintf(stdout, " IPPP (QP: I %d, P %d) \n", params->qp0, params->qpN);

    else fprintf(stdout, " I-P-P-SP-P (QP: I %d, P %d, SP (%d, %d)) \n",  params->qp0, params->qpN, params->qpsp, params->qpsp_pred);

    // report on entropy coding  method
    if (params->symbol_mode == CAVLC)
      fprintf(stdout," Entropy coding method             : CAVLC\n");
    else
      fprintf(stdout," Entropy coding method             : CABAC\n");

    fprintf(stdout,  " Profile/Level IDC                 : (%d,%d)\n", params->ProfileIDC, params->LevelIDC);

    if (params->SearchMode == UM_HEX)
      fprintf(stdout,  " Motion Estimation Scheme          : HEX\n");
    else if (params->SearchMode == UM_HEX_SIMPLE)
      fprintf(stdout,  " Motion Estimation Scheme          : SHEX\n");
    else if (params->SearchMode == EPZS)
    {
      fprintf(stdout,  " Motion Estimation Scheme          : EPZS\n");
      EPZSOutputStats(stdout, 0);
    }
    else if (params->SearchMode == FAST_FULL_SEARCH)
      fprintf(stdout,  " Motion Estimation Scheme          : Fast Full Search\n");
    else
      fprintf(stdout,  " Motion Estimation Scheme          : Full Search\n");

    if (params->full_search == 2)
      fprintf(stdout," Search range restrictions         : none\n");
    else if (params->full_search == 1)
      fprintf(stdout," Search range restrictions         : older reference frames\n");
    else
      fprintf(stdout," Search range restrictions         : smaller blocks and older reference frames\n");

    if (params->rdopt)
      fprintf(stdout," RD-optimized mode decision        : used\n");
    else
      fprintf(stdout," RD-optimized mode decision        : not used\n");

    switch(params->partition_mode)
    {
    case PAR_DP_1:
      fprintf(stdout," Data Partitioning Mode            : 1 partition \n");
      break;
    case PAR_DP_3:
      fprintf(stdout," Data Partitioning Mode            : 3 partitions \n");
      break;
    default:
      fprintf(stdout," Data Partitioning Mode            : not supported\n");
      break;
    }

    switch(params->of_mode)
    {
    case PAR_OF_ANNEXB:
      fprintf(stdout," Output File Format                : H.264/AVC Annex B Byte Stream Format \n");
      break;
    case PAR_OF_RTP:
      fprintf(stdout," Output File Format                : RTP Packet File Format \n");
      break;
    default:
      fprintf(stdout," Output File Format                : not supported\n");
      break;
    }
  }


  switch (params->Verbose)
  {
  case 0:
  default:
    printf("-------------------------------------------------------------------------------\n");
    printf("\nEncoding. Please Wait.\n\n");
    break;    
  case 1:
    printf("-------------------------------------------------------------------------------\n");
    printf("  Frame  Bit/pic    QP   SnrY    SnrU    SnrV    Time(ms) MET(ms) Frm/Fld Ref  \n");
    printf("-------------------------------------------------------------------------------\n");
    break;
  case 2:
    printf("------------------------------------------------------------------------------------------------\n");
    printf("  Frame  Bit/pic WP QP QL   SnrY    SnrU    SnrV    Time(ms) MET(ms) Frm/Fld   I D L0 L1 RDP Ref\n");
    printf("------------------------------------------------------------------------------------------------\n");
    break;
  }
}

/*!
 ************************************************************************
 * \brief
 *    memory allocation for original picture buffers
 ************************************************************************
 */
int init_orig_buffers(void)
{
  int memory_size = 0;
  int nplane;

  // allocate memory for reference frame buffers: imgY_org_frm, imgUV_org_frm
  if( IS_INDEPENDENT(params) )
  {
    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      memory_size += get_mem2Dpel(&imgY_org_frm_JV[nplane], img->height, img->width);
    }
  }
  else
  {
    memory_size += get_mem2Dpel(&imgY_org_frm, img->height, img->width);
  }

  if (img->yuv_format != YUV400)
  {
    int i, j, k;
    memory_size += get_mem3Dpel(&imgUV_org_frm, 2, img->height_cr, img->width_cr);
    for (k = 0; k < 2; k++)
      for (j = 0; j < img->height_cr; j++)
        for (i = 0; i < img->width_cr; i++)
          imgUV_org_frm[k][j][i] = 128;
  }


  if (!active_sps->frame_mbs_only_flag)
  {
    // allocate memory for reference frame buffers: imgY_org, imgUV_org
    init_top_bot_planes(imgY_org_frm, img->height, img->width, &imgY_org_top, &imgY_org_bot);

    if (img->yuv_format != YUV400)
    {
      if ((imgUV_org_top = (imgpel***)calloc(2, sizeof(imgpel**))) == NULL)
        no_mem_exit("init_global_buffers: imgUV_org_top");
      if ((imgUV_org_bot = (imgpel***)calloc(2, sizeof(imgpel**))) == NULL)
        no_mem_exit("init_global_buffers: imgUV_org_bot");

      memory_size += 4*(sizeof(imgpel**));

      memory_size += init_top_bot_planes(imgUV_org_frm[0], img->height_cr, img->width_cr, &(imgUV_org_top[0]), &(imgUV_org_bot[0]));
      memory_size += init_top_bot_planes(imgUV_org_frm[1], img->height_cr, img->width_cr, &(imgUV_org_top[1]), &(imgUV_org_bot[1]));
    }
  }
  return memory_size;
}

/*!
 ************************************************************************
 * \brief
 *    Dynamic memory allocation of frame size related global buffers
 *    buffers are defined in global.h, allocated memory must be freed in
 *    void free_global_buffers()
 * \par Input:
 *    Input Parameters struct inp_par *inp,                            \n
 *    Image Parameters ImageParameters *img
 * \return Number of allocated bytes
 ************************************************************************
 */
int init_global_buffers(void)
{
  int j, memory_size=0;
#ifdef _ADAPT_LAST_GROUP_
  extern int *last_P_no_frm;
  extern int *last_P_no_fld;

//  if ((last_P_no_frm = (int*)malloc(2*img->max_num_references*sizeof(int))) == NULL)
  if ((last_P_no_frm = (int*)malloc(32*sizeof(int))) == NULL)
    no_mem_exit("init_global_buffers: last_P_no");
  if (!active_sps->frame_mbs_only_flag)
//    if ((last_P_no_fld = (int*)malloc(4*img->max_num_references*sizeof(int))) == NULL)
    if ((last_P_no_fld = (int*)malloc(64*sizeof(int))) == NULL)
      no_mem_exit("init_global_buffers: last_P_no");
#endif

  if ((enc_frame_picture = (StorablePicture**)malloc(6 * sizeof(StorablePicture*))) == NULL)
    no_mem_exit("init_global_buffers: *enc_frame_picture");

  for (j = 0; j < 6; j++)
    enc_frame_picture[j] = NULL;

  if ((enc_field_picture = (StorablePicture**)malloc(2 * sizeof(StorablePicture*))) == NULL)
    no_mem_exit("init_global_buffers: *enc_field_picture");

  for (j = 0; j < 2; j++)
    enc_field_picture[j] = NULL;

  if ((frame_pic = (Picture**)malloc(img->frm_iter * sizeof(Picture*))) == NULL)
    no_mem_exit("init_global_buffers: *frame_pic");

  for (j = 0; j < img->frm_iter; j++)
    frame_pic[j] = malloc_picture();

  if (params->si_frame_indicator || params->sp_periodicity)
  {
    si_frame_indicator=0; //indicates whether the frame is SP or SI
    number_sp2_frames=0;

    frame_pic_si = malloc_picture();//picture buffer for the encoded SI picture
    //allocation of lrec and lrec_uv for SI picture
    get_mem2Dint (&lrec, img->height, img->width);
    get_mem3Dint (&lrec_uv, 2, img->height, img->width);
  }

  // Allocate memory for field picture coding
  if (params->PicInterlace != FRAME_CODING)
  { 
    if ((field_pic = (Picture**)malloc(2 * sizeof(Picture*))) == NULL)
      no_mem_exit("init_global_buffers: *field_pic");

    for (j = 0; j < 2; j++)
      field_pic[j] = malloc_picture();
  }

  memory_size += init_orig_buffers();

  memory_size += get_mem2Dint(&PicPos, img->FrameSizeInMbs + 1, 2);

  for (j = 0; j < (int) img->FrameSizeInMbs + 1; j++)
  {
    PicPos[j][0] = (j % img->PicWidthInMbs);
    PicPos[j][1] = (j / img->PicWidthInMbs);
  }

  if (params->WeightedPrediction || params->WeightedBiprediction || params->GenerateMultiplePPS)
  {
    // Currently only use up to 32 references. Need to use different indicator such as maximum num of references in list
    memory_size += get_mem3Dint(&wp_weight, 6, MAX_REFERENCE_PICTURES, 3);
    memory_size += get_mem3Dint(&wp_offset, 6, MAX_REFERENCE_PICTURES, 3);

    memory_size += get_mem4Dint(&wbp_weight, 6, MAX_REFERENCE_PICTURES, MAX_REFERENCE_PICTURES, 3);
  }

  // allocate memory for reference frames of each block: refFrArr

  if ((params->successive_Bframe != 0) || (params->BRefPictures > 0) || (params->ProfileIDC != 66))
  {
    memory_size += get_mem3D((byte ****)(void*)(&direct_ref_idx), 2, img->height_blk, img->width_blk);
    memory_size += get_mem2D((byte ***)(void*)&direct_pdir, img->height_blk, img->width_blk);
  }

  if (params->rdopt == 3)
  {
    memory_size += get_mem2Dint(&decs->resY, MB_BLOCK_SIZE, MB_BLOCK_SIZE);
    if ((decs->decref = (imgpel****) calloc(params->NoOfDecoders, sizeof(imgpel***))) == NULL)
      no_mem_exit("init_global_buffers: decref");
    for (j = 0; j < params->NoOfDecoders; j++)
    {
      memory_size += get_mem3Dpel(&decs->decref[j], img->max_num_references+1, img->height, img->width);
    }
    memory_size += get_mem2Dpel(&decs->RefBlock, BLOCK_SIZE, BLOCK_SIZE);
    memory_size += get_mem3Dpel(&decs->decY, params->NoOfDecoders, img->height, img->width);
    memory_size += get_mem3Dpel(&decs->decY_best, params->NoOfDecoders, img->height, img->width);
    memory_size += get_mem2D(&decs->status_map,  img->FrameHeightInMbs, img->PicWidthInMbs);
    memory_size += get_mem2D(&decs->dec_mb_mode, img->FrameHeightInMbs, img->PicWidthInMbs);
  }
  if (params->RestrictRef)
  {
    memory_size += get_mem2D(&pixel_map,   img->height,   img->width);
    memory_size += get_mem2D(&refresh_map, img->height/8, img->width/8);
  }

  if (!active_sps->frame_mbs_only_flag)
  {
    memory_size += get_mem2Dpel(&imgY_com, img->height, img->width);

    if (img->yuv_format != YUV400)
    {
      memory_size += get_mem3Dpel(&imgUV_com, 2, img->height_cr, img->width_cr);
    }
  }

  // allocate and set memory relating to motion estimation
  if (!params->IntraProfile)
  {  
    if (params->SearchMode == UM_HEX)
    {
      memory_size += UMHEX_get_mem();
    }
    else if (params->SearchMode == UM_HEX_SIMPLE)
    {
      smpUMHEX_init();
      memory_size += smpUMHEX_get_mem();
    }
    else if (params->SearchMode == EPZS)
      memory_size += EPZSInit();
  }

  if (params->RCEnable)
    rc_allocate_memory();

  if (params->redundant_pic_flag)
  {
    memory_size += get_mem2Dpel(&imgY_tmp, img->height, img->width);
    memory_size += get_mem2Dpel(&imgUV_tmp[0], img->height_cr, img->width_cr);
    memory_size += get_mem2Dpel(&imgUV_tmp[1], img->height_cr, img->width_cr);
  }

  memory_size += get_mem2Dint (&imgY_sub_tmp, img->height_padded, img->width_padded);

  if ( params->ChromaMCBuffer )
    chroma_mc_setup();

  img_padded_size_x       = (img->width + 2 * IMG_PAD_SIZE);
  img_padded_size_x2      = (img_padded_size_x << 1);
  img_padded_size_x4      = (img_padded_size_x << 2);
  img_padded_size_x_m8    = (img_padded_size_x - 8);
  img_padded_size_x_m8x8  = (img_padded_size_x - BLOCK_SIZE_8x8);
  img_padded_size_x_m4x4  = (img_padded_size_x - BLOCK_SIZE);
  img_cr_padded_size_x    = (img->width_cr + 2 * img_pad_size_uv_x);
  img_cr_padded_size_x2   = (img_cr_padded_size_x << 1);
  img_cr_padded_size_x4   = (img_cr_padded_size_x << 2);
  img_cr_padded_size_x_m8 = (img_cr_padded_size_x - 8);
  return memory_size;
}


/*!
 ************************************************************************
 * \brief
 *    Free allocated memory of original picture buffers
 ************************************************************************
 */
void free_orig_planes(void)
{
  if( IS_INDEPENDENT(params) )
  {
    int nplane;
    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      free_mem2Dpel(imgY_org_frm_JV[nplane]);      // free ref frame buffers
    }
  }
  else
  {
    free_mem2Dpel(imgY_org_frm);      // free ref frame buffers
  }

  if (img->yuv_format != YUV400)
    free_mem3Dpel(imgUV_org_frm);


  if (!active_sps->frame_mbs_only_flag)
  {
    free_top_bot_planes(imgY_org_top, imgY_org_bot);

    if (img->yuv_format != YUV400)
    {
      free_top_bot_planes(imgUV_org_top[0], imgUV_org_bot[0]);
      free_top_bot_planes(imgUV_org_top[1], imgUV_org_bot[1]);
      free (imgUV_org_top);
      free (imgUV_org_bot);
    }
  }
}


/*!
 ************************************************************************
 * \brief
 *    Free allocated memory of frame size related global buffers
 *    buffers are defined in global.h, allocated memory is allocated in
 *    int get_mem4global_buffers()
 * \par Input:
 *    Input Parameters struct inp_par *inp,                             \n
 *    Image Parameters ImageParameters *img
 * \par Output:
 *    none
 ************************************************************************
 */
void free_global_buffers(void)
{
  int  i,j;

#ifdef _ADAPT_LAST_GROUP_
  extern int *last_P_no_frm;
  extern int *last_P_no_fld;
  free (last_P_no_frm);
  free (last_P_no_fld);
#endif

  if (enc_frame_picture)
    free (enc_frame_picture);
  if (frame_pic)
  {
    for (j = 0; j < img->frm_iter; j++)
    {
      if (frame_pic[j])
        free_picture (frame_pic[j]);
    }
    free (frame_pic);
  }

  if (enc_field_picture)
    free (enc_field_picture);
  if (field_pic)
  {
    for (j = 0; j < 2; j++)
    {
      if (field_pic[j])
        free_picture (field_pic[j]);
    }
    free (field_pic);
  }

  // Deallocation of SI picture related memory
  if (params->si_frame_indicator || params->sp_periodicity)
  {
    free_picture (frame_pic_si);
    //deallocation of lrec and lrec_uv for SI frames
    free_mem2Dint (lrec);
    free_mem3Dint (lrec_uv);
  }

  free_orig_planes();
  // free lookup memory which helps avoid divides with PicWidthInMbs
  free_mem2Dint(PicPos);
  // Free Qmatrices and offsets
  free_QMatrix();
  free_QOffsets();

  if (params->WeightedPrediction || params->WeightedBiprediction || params->GenerateMultiplePPS)
  {
    free_mem3Dint(wp_weight );
    free_mem3Dint(wp_offset );
    free_mem4Dint(wbp_weight);
  }

  if ((stats->successive_Bframe != 0) || (params->BRefPictures > 0)||(params->ProfileIDC != 66))
  {
    free_mem3D((byte ***)direct_ref_idx);
    free_mem2D((byte **) direct_pdir);
  } // end if B frame

  if (imgY_sub_tmp) // free temp quarter pel frame buffers
  {
    free_mem2Dint (imgY_sub_tmp);
    imgY_sub_tmp=NULL;
  }

  // free mem, allocated in init_img()
  // free intra pred mode buffer for blocks
  free_mem2D((byte**)img->ipredmode);
  free_mem2D((byte**)img->ipredmode8x8);
  
  if( IS_INDEPENDENT(params) )
  {
    for( i=0; i<MAX_PLANE; i++ ){
      free(img->mb_data_JV[i]);
    }
  }
  else
  {
    free(img->mb_data);
  }

  free_mem2D((byte**)rddata_top_frame_mb.ipredmode);
  free_mem2D((byte**)rddata_trellis_curr.ipredmode);
  free_mem2D((byte**)rddata_trellis_best.ipredmode);

  if(params->UseConstrainedIntraPred)
  {
    free (img->intra_block);
  }

  if (params->CtxAdptLagrangeMult == 1)
  {
    free(mb16x16_cost_frame);
  }

  if (params->rdopt == 3)
  {
    free(decs->resY[0]);
    free(decs->resY);

    free(decs->RefBlock[0]);
    free(decs->RefBlock);

    for (j=0; j < params->NoOfDecoders; j++)
    {
      free(decs->decY[j][0]);
      free(decs->decY[j]);

      free(decs->decY_best[j][0]);
      free(decs->decY_best[j]);

      for (i=0; i < img->max_num_references+1; i++)
      {
        free(decs->decref[j][i][0]);
        free(decs->decref[j][i]);
      }
      free(decs->decref[j]);
    }
    free(decs->decY);
    free(decs->decY_best);
    free(decs->decref);

    free(decs->status_map[0]);
    free(decs->status_map);

    free(decs->dec_mb_mode[0]);
    free(decs->dec_mb_mode);
  }
  if (params->RestrictRef)
  {
    free(pixel_map[0]);
    free(pixel_map);
    free(refresh_map[0]);
    free(refresh_map);
  }

  if (!active_sps->frame_mbs_only_flag)
  {
    free_mem2Dpel(imgY_com);
    if (img->yuv_format != YUV400)
    {
      free_mem3Dpel(imgUV_com);
    }
  }

  free_mem3Dint(img->nz_coeff);

  free_mem2Ddb_offset (img->lambda_md, img->bitdepth_luma_qp_scale);
  free_mem3Ddb_offset (img->lambda_me, 10, 52 + img->bitdepth_luma_qp_scale, img->bitdepth_luma_qp_scale);
  free_mem3Dint_offset(img->lambda_mf, 10, 52 + img->bitdepth_luma_qp_scale, img->bitdepth_luma_qp_scale);

  if (params->CtxAdptLagrangeMult == 1)
  {
    free_mem2Ddb_offset(img->lambda_mf_factor, img->bitdepth_luma_qp_scale);
  }

  if (!params->IntraProfile)
  {
    if (params->SearchMode == UM_HEX)
    {
      UMHEX_free_mem();
    }
    else if (params->SearchMode == UM_HEX_SIMPLE)
    {
      smpUMHEX_free_mem();
    }
    else if (params->SearchMode == EPZS)
    {
      EPZSDelete();
    }
  }


  if (params->RCEnable)
    rc_free_memory();

  if (params->redundant_pic_flag)
  {
    free_mem2Dpel(imgY_tmp);
    free_mem2Dpel(imgUV_tmp[0]);
    free_mem2Dpel(imgUV_tmp[1]);
  }
}

/*!
 ************************************************************************
 * \brief
 *    Allocate memory for mv
 * \par Input:
 *    Image Parameters ImageParameters *img                             \n
 *    int****** mv
 * \return memory size in bytes
 ************************************************************************
 */
int get_mem_mv (short ******* mv)
{
  // LIST, reference, block_type, block_y, block_x, component
  get_mem6Dshort(mv, 2, img->max_num_references, 9, 4, 4, 2);

  return 2 * img->max_num_references * 9 * 4 * 4 * 2 * sizeof(short);
}

/*!
 ************************************************************************
 * \brief
 *    Free memory from mv
 * \par Input:
 *    int****** mv
 ************************************************************************
 */
void free_mem_mv (short****** mv)
{
  free_mem6Dshort(mv);
}


/*!
 ************************************************************************
 * \brief
 *    Allocate memory for AC coefficients
 ************************************************************************
 */
int get_mem_ACcoeff (int***** cofAC)
{
  int num_blk8x8 = 4 + img->num_blk8x8_uv;
  
  get_mem4Dint(cofAC, num_blk8x8, 4, 2, 65);
  return num_blk8x8*4*2*65*sizeof(int);// 18->65 for ABT
}

/*!
 ************************************************************************
 * \brief
 *    Allocate memory for DC coefficients
 ************************************************************************
 */
int get_mem_DCcoeff (int**** cofDC)
{
  get_mem3Dint(cofDC, 3, 2, 18);
  return 3 * 2 * 18 * sizeof(int); 
}


/*!
 ************************************************************************
 * \brief
 *    Free memory of AC coefficients
 ************************************************************************
 */
void free_mem_ACcoeff (int**** cofAC)
{
  free_mem4Dint(cofAC);
}

/*!
 ************************************************************************
 * \brief
 *    Free memory of DC coefficients
 ************************************************************************
 */
void free_mem_DCcoeff (int*** cofDC)
{
  free_mem3Dint(cofDC);
}


/*!
 ************************************************************************
 * \brief
 *    form frame picture from two field pictures
 ************************************************************************
 */
void combine_field(void)
{
  int i;

  for (i = 0; i < (img->height >> 1); i++)
  {
    memcpy(imgY_com[i*2],     enc_field_picture[0]->imgY[i], img->width*sizeof(imgpel));     // top field
    memcpy(imgY_com[i*2 + 1], enc_field_picture[1]->imgY[i], img->width*sizeof(imgpel)); // bottom field
  }

  if (img->yuv_format != YUV400)
  {
    for (i = 0; i < (img->height_cr >> 1); i++)
    {
      memcpy(imgUV_com[0][i*2],     enc_field_picture[0]->imgUV[0][i], img->width_cr*sizeof(imgpel));
      memcpy(imgUV_com[0][i*2 + 1], enc_field_picture[1]->imgUV[0][i], img->width_cr*sizeof(imgpel));
      memcpy(imgUV_com[1][i*2],     enc_field_picture[0]->imgUV[1][i], img->width_cr*sizeof(imgpel));
      memcpy(imgUV_com[1][i*2 + 1], enc_field_picture[1]->imgUV[1][i], img->width_cr*sizeof(imgpel));
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    RD decision of frame and field coding
 ************************************************************************
 */
int decide_fld_frame(float snr_frame_Y, float snr_field_Y, int bit_field, int bit_frame, double lambda_picture)
{
  double cost_frame, cost_field;

  cost_frame = bit_frame * lambda_picture + snr_frame_Y;
  cost_field = bit_field * lambda_picture + snr_field_Y;

  if (cost_field > cost_frame)
    return 0;
  else
    return 1;
}

/*!
 ************************************************************************
 * \brief
 *    Set the image type for I,P and SP pictures (not B!)
 ************************************************************************
 */
void SetImgType(void)
{
  int intra_refresh = (params->intra_period == 0) ? (IMG_NUMBER == 0) : (( ( img->frm_number - img->lastIntraNumber) % params->intra_period ) == 0);
  int idr_refresh;

  if ( params->idr_period && !params->adaptive_idr_period )
    idr_refresh = (( ( img->frm_number - img->lastIDRnumber  ) % params->idr_period   ) == 0);
  else if ( params->idr_period && params->adaptive_idr_period == 1 )
    idr_refresh = (( ( img->frm_number - imax(img->lastIntraNumber, img->lastIDRnumber)  ) % params->idr_period   ) == 0);
  else
    idr_refresh = (IMG_NUMBER == 0);

  if (intra_refresh || idr_refresh)
  {
    set_slice_type( I_SLICE );        // set image type for first image to I-frame
  }
  else
  {
    set_slice_type((params->sp_periodicity && ((IMG_NUMBER % params->sp_periodicity) == 0))
      ? SP_SLICE  : ((params->BRefPictures == 2) ? B_SLICE : P_SLICE) );
  }
}

/*!
 ************************************************************************
 * \brief
 *    Sets indices to appropriate level constraints, depending on 
 *    current level_idc
 ************************************************************************
 */
void SetLevelIndices(void)
{
  switch(active_sps->level_idc)
  {
  case 9:
    img->LevelIndex=1;
    break;
  case 10:
    img->LevelIndex=0;
    break;
  case 11:
    if (!IS_FREXT_PROFILE(active_sps->profile_idc) && (active_sps->constrained_set3_flag == 0))
      img->LevelIndex=2;
    else
      img->LevelIndex=1;
    break;
  case 12:
    img->LevelIndex=3;
    break;
  case 13:
    img->LevelIndex=4;
    break;
  case 20:
    img->LevelIndex=5;
    break;
  case 21:
    img->LevelIndex=6;
    break;
  case 22:
    img->LevelIndex=7;
    break;
  case 30:
    img->LevelIndex=8;
    break;
  case 31:
    img->LevelIndex=9;
    break;
  case 32:
    img->LevelIndex=10;
    break;
  case 40:
    img->LevelIndex=11;
    break;
  case 41:
    img->LevelIndex=12;
    break;
  case 42:
    if (!IS_FREXT_PROFILE(active_sps->profile_idc))
      img->LevelIndex=13;
    else
      img->LevelIndex=14;
    break;
  case 50:
    img->LevelIndex=15;
    break;
  case 51:
    img->LevelIndex=16;
    break;
  default:
    fprintf ( stderr, "Warning: unknown LevelIDC, using maximum level 5.1 \n" );
    img->LevelIndex=16;
    break;
  }
}

/*!
 ************************************************************************
 * \brief
 *    initialize key frames and corresponding redundant frames.
 ************************************************************************
 */
void Init_redundant_frame()
{
  if (params->redundant_pic_flag)
  {
    if (params->successive_Bframe)
    {
      error("B frame not supported when redundant picture used!", 100);
    }

    if (params->PicInterlace)
    {
      error("Interlace not supported when redundant picture used!", 100);
    }

    if (params->num_ref_frames < params->PrimaryGOPLength)
    {
      error("NumberReferenceFrames must be no less than PrimaryGOPLength", 100);
    }

    if ((1<<params->NumRedundantHierarchy) > params->PrimaryGOPLength)
    {
      error("PrimaryGOPLength must be greater than 2^NumRedundantHeirarchy", 100);
    }

    if (params->Verbose != 1)
    {
      error("Redundant slices not supported when Verbose != 1", 100);
    }
  }

  key_frame = 0;
  redundant_coding = 0;
  img->redundant_pic_cnt = 0;
  frameNuminGOP = img->frm_number % params->PrimaryGOPLength;
  if (img->frm_number == 0)
  {
    frameNuminGOP = -1;
  }
}

/*!
 ************************************************************************
 * \brief
 *    allocate redundant frames in a primary GOP.
 ************************************************************************
 */
void Set_redundant_frame()
{
  int GOPlength = params->PrimaryGOPLength;

  //start frame of GOP
  if (frameNuminGOP == 0)
  {
    redundant_coding = 0;
    key_frame = 1;
    redundant_ref_idx = GOPlength;
  }

  //1/2 position
  if (params->NumRedundantHierarchy > 0)
  {
    if (frameNuminGOP == GOPlength/2)
    {
      redundant_coding = 0;
      key_frame = 1;
      redundant_ref_idx = GOPlength/2;
    }
  }

  //1/4, 3/4 position
  if (params->NumRedundantHierarchy > 1)
  {
    if (frameNuminGOP == GOPlength/4 || frameNuminGOP == GOPlength*3/4)
    {
      redundant_coding = 0;
      key_frame = 1;
      redundant_ref_idx = GOPlength/4;
    }
  }

  //1/8, 3/8, 5/8, 7/8 position
  if (params->NumRedundantHierarchy > 2)
  {
    if (frameNuminGOP == GOPlength/8 || frameNuminGOP == GOPlength*3/8
      || frameNuminGOP == GOPlength*5/8 || frameNuminGOP == GOPlength*7/8)
    {
      redundant_coding = 0;
      key_frame = 1;
      redundant_ref_idx = GOPlength/8;
    }
  }

  //1/16, 3/16, 5/16, 7/16, 9/16, 11/16, 13/16 position
  if (params->NumRedundantHierarchy > 3)
  {
    if (frameNuminGOP == GOPlength/16 || frameNuminGOP == GOPlength*3/16
      || frameNuminGOP == GOPlength*5/16 || frameNuminGOP == GOPlength*7/16
      || frameNuminGOP == GOPlength*9/16 || frameNuminGOP == GOPlength*11/16
      || frameNuminGOP == GOPlength*13/16)
    {
      redundant_coding = 0;
      key_frame = 1;
      redundant_ref_idx = GOPlength/16;
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    encode on redundant frame.
 ************************************************************************
 */
void encode_one_redundant_frame()
{
  key_frame = 0;
  redundant_coding = 1;
  img->redundant_pic_cnt = 1;

  if (img->type == I_SLICE)
  {
    set_slice_type( P_SLICE );
  }

  encode_one_frame();
}

/*!
 ************************************************************************
 * \brief
 *    Setup Chroma MC Variables
 ************************************************************************
 */
void chroma_mc_setup(void)
{
  // initialize global variables used for chroma interpolation and buffering
  if ( img->yuv_format == YUV420 )
  {
    img_pad_size_uv_x = IMG_PAD_SIZE >> 1;
    img_pad_size_uv_y = IMG_PAD_SIZE >> 1;
    chroma_mask_mv_y = 7;
    chroma_mask_mv_x = 7;
    chroma_shift_x = 3;
    chroma_shift_y = 3;
  }
  else if ( img->yuv_format == YUV422 )
  {
    img_pad_size_uv_x = IMG_PAD_SIZE >> 1;
    img_pad_size_uv_y = IMG_PAD_SIZE;
    chroma_mask_mv_y = 3;
    chroma_mask_mv_x = 7;
    chroma_shift_y = 2;
    chroma_shift_x = 3;
  }
  else
  { // YUV444
    img_pad_size_uv_x = IMG_PAD_SIZE;
    img_pad_size_uv_y = IMG_PAD_SIZE;
    chroma_mask_mv_y = 3;
    chroma_mask_mv_x = 3;
    chroma_shift_y = 2;
    chroma_shift_x = 2;
  }
  shift_cr_y  = chroma_shift_y - 2;
  shift_cr_x  = chroma_shift_x - 2;
}

