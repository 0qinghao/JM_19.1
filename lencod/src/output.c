
/*!
 ************************************************************************
 * \file output.c
 *
 * \brief
 *    Output an image and Trance support
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Karsten Suehring               <suehring@hhi.de>
 ************************************************************************
 */

#include "contributors.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "global.h"
#include "image.h"

void write_out_picture(StorablePicture *p, int p_out);

FrameStore* out_buffer;


/*!
 ************************************************************************
 * \brief
 *    Writes out a storable picture without doing any output modifications
 * \param p
 *    Picture to be written
 * \param p_out
 *    Output file
 * \param real_structure
 *    real picture structure
 ************************************************************************
 */
void write_picture(StorablePicture *p, int p_out, int real_structure)
{
  write_out_picture(p, p_out);
}

/*!
 ************************************************************************
 * \brief
 *    Writes out a storable picture
 * \param p
 *    Picture to be written
 * \param p_out
 *    Output file
 ************************************************************************
 */
void write_out_picture(StorablePicture *p, int p_out)
{
  int i,j;

  int crop_left, crop_right, crop_top, crop_bottom;
  int crop_vert_mult;
  int symbol_size_in_bytes = img->pic_unit_size_on_disk/8;
  Boolean rgb_output = (input->rgb_input_flag && input->yuv_format==3);
  char *buf;

  if (p->non_existing)
    return;

  if (active_sps->frame_mbs_only_flag)
  {
    crop_vert_mult = 2;
  }
  else
  {
    if (p->structure != FRAME)
    {
      crop_vert_mult = 2;
    }
    else
    {
      crop_vert_mult = 4;
    }
    }
  
  if (active_sps->frame_cropping_flag)
  {
    crop_left   = 2 * active_sps->frame_cropping_rect_left_offset;
    crop_right  = 2 * active_sps->frame_cropping_rect_right_offset;
    crop_top    = crop_vert_mult * active_sps->frame_cropping_rect_top_offset;
    crop_bottom = crop_vert_mult * active_sps->frame_cropping_rect_bottom_offset;
  }
  else
  {
    crop_left = crop_right = crop_top = crop_bottom = 0;
  }

  //printf ("write frame size: %dx%d\n", p->size_x-crop_left-crop_right,p->size_y-crop_top-crop_bottom );
  
  // KS: this buffer should actually be allocated only once, but this is still much faster than the previous version
  buf = malloc (p->size_x*p->size_y*symbol_size_in_bytes);
  if (NULL==buf)
  {
    no_mem_exit("write_out_picture: buf");
  }

  if(rgb_output)
  {
    crop_left   /= 2;
    crop_right  /= 2;
    crop_top    /= 2;
    crop_bottom /= 2;

    for(i=crop_top;i<p->size_y_cr-crop_bottom;i++)
      for(j=crop_left;j<p->size_x_cr-crop_right;j++)
      {
        memcpy(buf+((j-crop_left+(i-crop_top)*(p->size_x_cr-crop_left-crop_right))*symbol_size_in_bytes),&(p->imgUV[1][i][j]), symbol_size_in_bytes);
      }
    write(p_out, buf, (p->size_y_cr-crop_bottom-crop_top)*(p->size_x_cr-crop_right-crop_left)*symbol_size_in_bytes);

    if (active_sps->frame_cropping_flag)
    {
      crop_left   = 2 * active_sps->frame_cropping_rect_left_offset;
      crop_right  = 2 * active_sps->frame_cropping_rect_right_offset;
      crop_top    = crop_vert_mult * active_sps->frame_cropping_rect_top_offset;
      crop_bottom = crop_vert_mult * active_sps->frame_cropping_rect_bottom_offset;
    }
    else
    {
      crop_left = crop_right = crop_top = crop_bottom = 0;
    }
  }
  
  
  for(i=crop_top;i<p->size_y-crop_bottom;i++)
    for(j=crop_left;j<p->size_x-crop_right;j++)
    {
      memcpy(buf+((j-crop_left+((i-crop_top)*(p->size_x-crop_left-crop_right)))*symbol_size_in_bytes),&(p->imgY[i][j]), symbol_size_in_bytes);
    }

  write(p_out, buf, (p->size_y-crop_bottom-crop_top)*(p->size_x-crop_right-crop_left)*symbol_size_in_bytes);

  crop_left   /= 2;
  crop_right  /= 2;
  crop_top    /= 2;
  crop_bottom /= 2;

  for(i=crop_top; i<p->size_y_cr-crop_bottom; i++)
    for(j=crop_left;j<p->size_x_cr-crop_right;j++)
    {
      memcpy(buf+((j-crop_left+(i-crop_top)*(p->size_x_cr-crop_left-crop_right))*symbol_size_in_bytes),&(p->imgUV[0][i][j]), symbol_size_in_bytes);
    }
  write(p_out, buf, (p->size_y_cr-crop_bottom-crop_top)*(p->size_x_cr-crop_right-crop_left)*symbol_size_in_bytes);

  if (!rgb_output)
  {
    for(i=crop_top;i<p->size_y_cr-crop_bottom;i++)
      for(j=crop_left;j<p->size_x_cr-crop_right;j++)
      {
        memcpy(buf+((j-crop_left+(i-crop_top)*(p->size_x_cr-crop_left-crop_right))*symbol_size_in_bytes),&(p->imgUV[1][i][j]), symbol_size_in_bytes);
      }
    write(p_out, buf, (p->size_y_cr-crop_bottom-crop_top)*(p->size_x_cr-crop_right-crop_left)*symbol_size_in_bytes);
    }

  free(buf);
    
//  fsync(p_out);
}

/*!
 ************************************************************************
 * \brief
 *    Initialize output buffer for direct output
 ************************************************************************
 */
void init_out_buffer()
{
  out_buffer = alloc_frame_store();
}

/*!
 ************************************************************************
 * \brief
 *    Uninitialize output buffer for direct output
 ************************************************************************
 */
void uninit_out_buffer()
{
  free_frame_store(out_buffer);
  out_buffer=NULL;
}

/*!
 ************************************************************************
 * \brief
 *    Initialize picture memory with (Y:0,U:128,V:128)
 ************************************************************************
 */
void clear_picture(StorablePicture *p)
{
  int i;

  for(i=0;i<p->size_y;i++)
    memset(p->imgY[i], img->dc_pred_value, p->size_x*sizeof(imgpel));
  for(i=0;i<p->size_y_cr;i++)
    memset(p->imgUV[0][i], img->dc_pred_value ,p->size_x_cr*sizeof(imgpel));
  for(i=0;i<p->size_y_cr;i++)
    memset(p->imgUV[1][i], img->dc_pred_value ,p->size_x_cr*sizeof(imgpel));
}

/*!
 ************************************************************************
 * \brief
 *    Write out not paired direct output fields. A second empty field is generated
 *    and combined into the frame buffer.
 * \param fs
 *    FrameStore that contains a single field
 * \param p_out
 *    Output file
 ************************************************************************
 */
void write_unpaired_field(FrameStore* fs, int p_out)
{
  StorablePicture *p;
  assert (fs->is_used<3);
  if(fs->is_used &1)
  {
    // we have a top field
    // construct an empty bottom field
    p = fs->top_field;
    fs->bottom_field = alloc_storable_picture(BOTTOM_FIELD, p->size_x, p->size_y, p->size_x_cr, p->size_y_cr);
    clear_picture(fs->bottom_field);
    dpb_combine_field(fs);
    write_picture (fs->frame, p_out, TOP_FIELD);
  }

  if(fs->is_used &2)
  {
    // we have a bottom field
    // construct an empty top field
    p = fs->bottom_field;
    fs->top_field = alloc_storable_picture(TOP_FIELD, p->size_x, p->size_y, p->size_x_cr, p->size_y_cr);
    clear_picture(fs->top_field);
    dpb_combine_field(fs);
    write_picture (fs->frame, p_out, BOTTOM_FIELD);
  }

  fs->is_used=3;
}

/*!
 ************************************************************************
 * \brief
 *    Write out unpaired fields from output buffer.
 * \param p_out
 *    Output file
 ************************************************************************
 */
void flush_direct_output(int p_out)
{
  write_unpaired_field(out_buffer, p_out);

  free_storable_picture(out_buffer->frame);
  out_buffer->frame = NULL;
  free_storable_picture(out_buffer->top_field);
  out_buffer->top_field = NULL;
  free_storable_picture(out_buffer->bottom_field);
  out_buffer->bottom_field = NULL;
  out_buffer->is_used = 0;
}


/*!
 ************************************************************************
 * \brief
 *    Write a frame (from FrameStore)
 * \param fs
 *    FrameStore containing the frame
 * \param p_out
 *    Output file
 ************************************************************************
 */
void write_stored_frame( FrameStore *fs,int p_out)
{
  // make sure no direct output field is pending
  flush_direct_output(p_out);

  if (fs->is_used<3)
  {
    write_unpaired_field(fs, p_out);
  }
  else
  {
    write_picture(fs->frame, p_out, FRAME);
  }

  fs->is_output = 1;
}

/*!
 ************************************************************************
 * \brief
 *    Directly output a picture without storing it in the DPB. Fields 
 *    are buffered before they are written to the file.
 * \param p
 *    Picture for output
 * \param p_out
 *    Output file
 ************************************************************************
 */
void direct_output(StorablePicture *p, int p_out)
{
  if (p->structure==FRAME)
  {
    // we have a frame (or complementary field pair)
    // so output it directly
    flush_direct_output(p_out);
    write_picture (p, p_out, FRAME);
    free_storable_picture(p);
    return;
  }

  if (p->structure == TOP_FIELD)
  {
    if (out_buffer->is_used &1)
      flush_direct_output(p_out);
    out_buffer->top_field = p;
    out_buffer->is_used |= 1;
  }

  if (p->structure == BOTTOM_FIELD)
  {
    if (out_buffer->is_used &2)
      flush_direct_output(p_out);
    out_buffer->bottom_field = p;
    out_buffer->is_used |= 2;
  }

  if (out_buffer->is_used == 3)
  {
    // we have both fields, so output them
    dpb_combine_field(out_buffer);
    write_picture (out_buffer->frame, p_out, FRAME);
    free_storable_picture(out_buffer->frame);
    out_buffer->frame = NULL;
    free_storable_picture(out_buffer->top_field);
    out_buffer->top_field = NULL;
    free_storable_picture(out_buffer->bottom_field);
    out_buffer->bottom_field = NULL;
    out_buffer->is_used = 0;
  }
}

