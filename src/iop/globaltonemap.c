/*
    This file is part of darktable,
    copyright (c) 2012 Henrik Andersson.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/opencl.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <xmmintrin.h>

#define BLOCKSIZE 2048		/* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */
#define REDUCESIZE 64

// NaN-safe clip: NaN compares false and will result in 0.0
#define CLIP(x) (((x)>=0.0)?((x)<=1.0?(x):1.0):0.0)
DT_MODULE(3)

typedef enum _iop_operator_t
{
  OPERATOR_REINHARD,
  OPERATOR_FILMIC,
  OPERATOR_DRAGO
} _iop_operator_t;

typedef struct dt_iop_global_tonemap_params_t
{
  _iop_operator_t operator;
  struct
  {
    float bias;
    float max_light; // cd/m2
  } drago;
  float detail;
}
dt_iop_global_tonemap_params_t;

typedef struct dt_iop_global_tonemap_data_t
{
  _iop_operator_t operator;
  struct
  {
    float bias;
    float max_light; // cd/m2
  } drago;
  float detail;
}
dt_iop_global_tonemap_data_t;

typedef struct dt_iop_global_tonemap_gui_data_t
{
  GtkWidget *operator;
  struct
  {
    GtkWidget *bias;
    GtkWidget *max_light;
  } drago;
  GtkWidget *detail;
}
dt_iop_global_tonemap_gui_data_t;

typedef struct dt_iop_global_tonemap_global_data_t
{
  int kernel_pixelmax_first;
  int kernel_pixelmax_second;
  int kernel_global_tonemap_reinhard;
  int kernel_global_tonemap_drago;
  int kernel_global_tonemap_filmic;
}
dt_iop_global_tonemap_global_data_t;

const char *name()
{
  return _("global tonemap");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int
groups ()
{
  return IOP_GROUP_TONE;
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version < 3 && new_version == 3)
  {
    dt_iop_global_tonemap_params_t *o = (dt_iop_global_tonemap_params_t *)old_params;
    dt_iop_global_tonemap_params_t *n = (dt_iop_global_tonemap_params_t *)new_params;

    // only appended detail, 0 is no-op
    memcpy(n, o, sizeof(dt_iop_global_tonemap_params_t) - sizeof(float));
    n->detail = 0.0f;
    return 0;
  }
  return 1;
}

static inline void process_reinhard(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                                    dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    float *outp = out + ch*k;
    float l = inp[0]/100.0;
    outp[0] = 100.0 * (l/(1.0f+l));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_drago(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                                 dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* precalcs */
  const float eps = 0.0001f;
  float lwmax = eps;

  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    lwmax = fmaxf(lwmax, (inp[0]*0.01f));
  }
  const float ldc = data->drago.max_light * 0.01 / log10f(lwmax+1);
  const float bl = logf(fmaxf(eps, data->drago.bias)) / logf(0.5);

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, lwmax) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    float *outp = out + ch*k;
    float lw = inp[0]*0.01f;
    outp[0] = 100.0f * (ldc * logf(fmaxf(eps, lw + 1.0f)) / logf(fmaxf(eps, 2.0f + (powf(lw/lwmax,bl)) * 8.0f)));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_filmic(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                  void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                                  dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    float *outp = out + ch*k;
    float l = inp[0]/100.0;
    float x = fmaxf(0.0f, l-0.004f);
    outp[0] = 100.0 * ((x*(6.2*x+.5))/(x*(6.2*x+1.7)+0.06));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_global_tonemap_data_t *data = (dt_iop_global_tonemap_data_t *)piece->data;
  const float scale = piece->iscale/roi_in->scale;
  const float sigma_r = 8.0f; // does not depend on scale
  const float iw = piece->buf_in.width /scale;
  const float ih = piece->buf_in.height/scale;
  const float sigma_s = fminf(iw, ih)*0.03f;
  dt_bilateral_t *b = NULL;
  if(data->detail != 0.0f)
  {
    b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    // get detail from unchanged input buffer
    dt_bilateral_splat(b, (float *)ivoid);
  }

  switch(data->operator)
  {
    case OPERATOR_REINHARD:
      process_reinhard(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_DRAGO:
      process_drago(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_FILMIC:
      process_filmic(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
  }

  if(data->detail != 0.0f)
  {
    dt_bilateral_blur(b);
    // and apply it to output buffer after logscale
    dt_bilateral_slice_to_output(b, (float *)ivoid, (float *)ovoid, data->detail);
    dt_bilateral_free(b);
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;
  dt_iop_global_tonemap_global_data_t *gd = (dt_iop_global_tonemap_global_data_t *)self->data;
  dt_bilateral_cl_t *b = NULL;

  // check if we are in a tiling context and want OPERATOR_DRAGO. This does not work as drago
  // needs the maximum L-value of the whole image. Let's return FALSE, which will then fall back
  // to cpu processing
  if(piece->pipe->tiling && d->operator == OPERATOR_DRAGO) return FALSE;

  cl_int err = -999;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  const int devid = piece->pipe->devid;
  int gtkernel = -1;

  const int width = roi_out->width;
  const int height = roi_out->height;
  float parameters[4] = { 0.0f };

  // prepare local work group
  size_t maxsizes[3] = { 0 };        // the maximum dimensions for a work group
  size_t workgroupsize = 0;          // the maximum number of items in a work group
  unsigned long localmemsize = 0;    // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0;    // the maximum amount of items in work group for this kernel

  // make sure blocksize is not too large
  int blocksize = BLOCKSIZE;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS &&
      dt_opencl_get_kernel_work_group_size(devid, gd->kernel_pixelmax_first, &kernelworkgroupsize) == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize*blocksize > kernelworkgroupsize
          || blocksize*blocksize > workgroupsize || blocksize*blocksize*sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;
    }
  }
  else
  {
    blocksize = 1;   // slow but safe
  }

  if(blocksize < 2)
  {
    // very small blocksize. this is really unlikely to happen, but let's be prepared: give up on opencl in that case
    return FALSE;
  }

  switch(d->operator)
  {
    case OPERATOR_REINHARD:
      gtkernel = gd->kernel_global_tonemap_reinhard;
      break;
    case OPERATOR_DRAGO:
      gtkernel = gd->kernel_global_tonemap_drago;
      break;
    case OPERATOR_FILMIC:
      gtkernel = gd->kernel_global_tonemap_filmic;
      break;
  }

  if(d->operator == OPERATOR_DRAGO)
  {
    size_t sizes[3];
    size_t local[3];

    const int bwidth = ROUNDUP(width, blocksize);
    const int bheight = ROUNDUP(height, blocksize);

    const int bufsize = (bwidth / blocksize) * (bheight / blocksize);
    const int groupsize = maxsizes[0];
    const int reducesize = MIN(REDUCESIZE, ROUNDUP(bufsize, groupsize) / groupsize);

    dev_m = dt_opencl_alloc_device_buffer(devid, bufsize*sizeof(float));
    if(dev_m == NULL) goto error;

    dev_r = dt_opencl_alloc_device_buffer(devid, reducesize*sizeof(float));
    if(dev_r == NULL) goto error;

    sizes[0] = bwidth;
    sizes[1] = bheight;
    sizes[2] = 1;
    local[0] = blocksize;
    local[1] = blocksize;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_first, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_first, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_first, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_first, 3, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_first, 4, blocksize*blocksize*sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pixelmax_first, sizes, local);
    if(err != CL_SUCCESS) goto error;

    sizes[0] = reducesize*groupsize;
    sizes[1] = 1;
    sizes[2] = 1;
    local[0] = groupsize;
    local[1] = 1;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_second, 0, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_second, 1, sizeof(cl_mem), &dev_r);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_second, 2, sizeof(int), &bufsize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_pixelmax_second, 3, groupsize*sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pixelmax_second, sizes, local);
    if(err != CL_SUCCESS) goto error;

    float maximum[reducesize];
    err = dt_opencl_read_buffer_from_device(devid, (void*)maximum, dev_r, 0, reducesize*sizeof(float), CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_release_mem_object(dev_r);
    dt_opencl_release_mem_object(dev_m);
    dev_r = dev_m = NULL;

    for(int k = 1; k < reducesize; k++)
    {
      float mine = maximum[0];
      float other = maximum[k];
      maximum[0] = (other > mine) ? other : mine;
    }

    const float eps = 0.0001f;
    const float lwmax = MAX(eps, (maximum[0]*0.01f));
    const float ldc = d->drago.max_light * 0.01f / log10f(lwmax + 1.0f);
    const float bl = logf(MAX(eps, d->drago.bias)) / logf(0.5f);

    parameters[0] = eps;
    parameters[1] = ldc;
    parameters[2] = bl;
    parameters[3] = lwmax;
  }

  const float scale = piece->iscale/roi_in->scale;
  const float sigma_r = 8.0f; // does not depend on scale
  const float iw = piece->buf_in.width /scale;
  const float ih = piece->buf_in.height/scale;
  const float sigma_s = fminf(iw, ih)*0.03f;

  if(d->detail != 0.0f)
  {
    b = dt_bilateral_init_cl(devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
    if(!b) goto error;
    // get detail from unchanged input buffer
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
  }

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gtkernel, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gtkernel, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gtkernel, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gtkernel, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gtkernel, 4, 4*sizeof(float), &parameters);
  err = dt_opencl_enqueue_kernel_2d(devid, gtkernel, sizes);
  if(err != CL_SUCCESS) goto error;

  if(d->detail != 0.0f)
  {
    err = dt_bilateral_blur_cl(b);
    if (err != CL_SUCCESS) goto error;
    // and apply it to output buffer after logscale
    err = dt_bilateral_slice_to_output_cl(b, dev_in, dev_out, d->detail);
    if (err != CL_SUCCESS) goto error;
    dt_bilateral_free_cl(b);
  }

  return TRUE;

error:
  if(b) dt_bilateral_free_cl(b);
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  dt_print(DT_DEBUG_OPENCL, "[opencl_global_tonemap] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  const float scale = piece->iscale/roi_in->scale;
  const float iw = piece->buf_in.width /scale;
  const float ih = piece->buf_in.height/scale;
  const float sigma_s = fminf(iw, ih)*0.03f;
  const float sigma_r = 8.0f;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width*height*channels*sizeof(float);

  tiling->factor = 2.0f + (float)dt_bilateral_memory_use(width,height,sigma_s,sigma_r)/basebuffer;    
  tiling->maxbuf = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width,height,sigma_s,sigma_r)/basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4*sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void
commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)p1;
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;

  d->operator = p->operator;
  d->drago.bias = p->drago.bias;
  d->drago.max_light = p->drago.max_light;
  d->detail = p->detail;

#ifdef HAVE_OPENCL
  if(d->detail != 0.0f)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_global_tonemap_data_t));
  memset(piece->data,0,sizeof(dt_iop_global_tonemap_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_global_tonemap_global_data_t *gd = (dt_iop_global_tonemap_global_data_t *)malloc(sizeof(dt_iop_global_tonemap_global_data_t));
  module->data = gd;
  gd->kernel_pixelmax_first = dt_opencl_create_kernel(program, "pixelmax_first");
  gd->kernel_pixelmax_second = dt_opencl_create_kernel(program, "pixelmax_second");
  gd->kernel_global_tonemap_reinhard = dt_opencl_create_kernel(program, "global_tonemap_reinhard");
  gd->kernel_global_tonemap_drago = dt_opencl_create_kernel(program, "global_tonemap_drago");
  gd->kernel_global_tonemap_filmic = dt_opencl_create_kernel(program, "global_tonemap_filmic");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_global_tonemap_global_data_t *gd = (dt_iop_global_tonemap_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_pixelmax_first);
  dt_opencl_free_kernel(gd->kernel_pixelmax_second);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_reinhard);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_drago);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_filmic);
  free(module->data);
  module->data = NULL;
}



static void
operator_callback (GtkWidget *combobox, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->operator = dt_bauhaus_combobox_get(combobox);

  gtk_widget_set_visible(g->drago.bias, FALSE);
  gtk_widget_set_visible(g->drago.max_light, FALSE);
  /* show ui for selected operator */
  if (p->operator == OPERATOR_DRAGO)
  {
    gtk_widget_set_visible(g->drago.bias, TRUE);
    gtk_widget_set_visible(g->drago.max_light, TRUE);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
_drago_bias_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->drago.bias = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
_drago_max_light_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->drago.max_light = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
detail_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->detail = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)module->params;
  dt_bauhaus_combobox_set(g->operator, p->operator);
  gtk_widget_set_visible(g->drago.bias, FALSE);
  gtk_widget_set_visible(g->drago.max_light, FALSE);
  /* show ui for selected operator */
  if (p->operator == OPERATOR_DRAGO)
  {
    gtk_widget_set_visible(g->drago.bias, TRUE);
    gtk_widget_set_visible(g->drago.max_light, TRUE);
  }

  /* drago */
  dt_bauhaus_slider_set(g->drago.bias, p->drago.bias);
  dt_bauhaus_slider_set(g->drago.max_light, p->drago.max_light);
  dt_bauhaus_slider_set(g->detail, p->detail);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_global_tonemap_params_t));
  module->default_params = malloc(sizeof(dt_iop_global_tonemap_params_t));
  module->default_enabled = 0;
  module->priority = 462; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_global_tonemap_params_t);
  module->gui_data = NULL;
  dt_iop_global_tonemap_params_t tmp = (dt_iop_global_tonemap_params_t)
  {
    OPERATOR_DRAGO,
    {0.85f, 100.0f},
    0.0f
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_global_tonemap_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_global_tonemap_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_global_tonemap_gui_data_t));
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);

  /* operator */
  g->operator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->operator,_("operator"));

  dt_bauhaus_combobox_add(g->operator, "reinhard");
  dt_bauhaus_combobox_add(g->operator, "filmic");
  dt_bauhaus_combobox_add(g->operator, "drago");

  g_object_set(G_OBJECT(g->operator), "tooltip-text", _("the global tonemap operator"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->operator), "value-changed",
                    G_CALLBACK (operator_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->operator), TRUE, TRUE, 0);

  /* drago bias */
  g->drago.bias = dt_bauhaus_slider_new_with_range(self,0.5, 1.0, 0.05, p->drago.bias, 2);
  dt_bauhaus_widget_set_label(g->drago.bias,_("bias"));
  g_object_set(G_OBJECT(g->drago.bias), "tooltip-text", _("the bias for tonemapper controls the linearity, the higher the more details in blacks"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->drago.bias), "value-changed",
                    G_CALLBACK (_drago_bias_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->drago.bias, TRUE, TRUE, 0);


  /* drago bias */
  g->drago.max_light = dt_bauhaus_slider_new_with_range(self,1, 500, 10, p->drago.max_light, 2);
  dt_bauhaus_widget_set_label(g->drago.max_light,_("target"));
  g_object_set(G_OBJECT(g->drago.max_light), "tooltip-text", _("the target light for tonemapper specified as cd/m2"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->drago.max_light), "value-changed",
                    G_CALLBACK (_drago_max_light_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->drago.max_light, TRUE, TRUE, 0);

  /* detail */
  g->detail = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, 0.0, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->detail, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->detail, _("detail"));

  g_signal_connect (G_OBJECT (g->detail), "value-changed", G_CALLBACK (detail_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
