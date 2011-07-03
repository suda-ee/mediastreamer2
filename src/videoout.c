/*
 * mediastreamer2 library - modular sound and video processing and
 * streaming Copyright (C) 2011 Wenfeng CAI (evil@xoox.org), Soochow
 * University Copyright (C) 2006 Simon MORLAT (simon.morlat@linphone.org)
 * 
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA. 
 */

#ifdef HAVE_CONFIG_H
#include "mediastreamer-config.h"
#endif

#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msvideo.h"

#include "mediastreamer2/msvideoout.h"

#include "ffmpeg-priv.h"

#include <xdc/std.h>
#include <ti/sdo/dmai/Dmai.h>
#include <ti/sdo/dmai/Display.h>
#include <ti/sdo/dmai/BufferGfx.h>
#include <ti/sdo/dmai/Resize.h>

#define SCALE_FACTOR 4.0f
#define SELVIEW_POS_INACTIVE -100.0

/* Error message */
#define ERR(fmt, args...) fprintf(stderr, "Error: " fmt, ## args)

/*
 * Used for UYVY packed buffer, 1 plane
 */
static void
uyvy_buf_init(YuvBuf *buf, int w, int h, uint8_t *ptr)
{
    int ysize,usize;
    ysize = w * h * 2; 
    usize = 0;
    buf->w = w;
    buf->h = h;
    buf->planes[0] = ptr;
    buf->planes[1] = NULL;
    buf->planes[2] = NULL;
    buf->planes[3] = NULL;
    buf->strides[0] = w * 2;
    buf->strides[1] = 0;
    buf->strides[2] = 0;
    buf->strides[3] = 0;
}

static int      video_out_set_vsize(MSFilter * f, void *arg);

int
ms_display_poll_event(MSDisplay * d, MSDisplayEvent * ev)
{
    if (d->desc->pollevent)
	return d->desc->pollevent(d, ev);
    else
	return -1;
}

static int
gcd(int m, int n)
{
    if (n == 0)
	return m;
    else
	return gcd(n, m % n);
}

static void
reduce(int *num, int *denom)
{
    int             divisor = gcd(*num, *denom);
    *num /= divisor;
    *denom /= divisor;
}

// changed to TI DMAI implimentation. Framework refactored.

typedef struct _DmaiDisplay {
    MSFilter       *filter;
    bool_t          dmai_initialized;
    ms_mutex_t      dmai_mutex;
    float           sv_scalefactor;
    MSVideoSize     screen_size;

    Display_Handle  hDisplay;
    Resize_Handle   hRsz;
    Buffer_Handle   vBuf;

} DmaiDisplay;

static void
dmai_show_window(bool_t show)
{
    ms_warning("DMAI window show/hide not implemented");
}

static long
dmai_get_native_window_id()
{
    ms_warning("dmai_get_native_window_id not implemented");
    return 0;
}

static void     dmai_display_uninit(MSDisplay * obj);

static          bool_t
dmai_display_init(MSDisplay * obj, MSFilter * f, MSPicture * fbuf,
		 MSPicture * fbuf_selfview)
{
    DmaiDisplay     *wd = (DmaiDisplay *) obj->data;
    Display_Attrs    dAttrs = Display_Attrs_DM6446_DM355_VID_DEFAULT;
    Buffer_Handle    hDstBuf;
    Resize_Attrs     rszAttrs = Resize_Attrs_DEFAULT;
    BufferGfx_Attrs  gfxAttrs = BufferGfx_Attrs_DEFAULT;
    Int32            bufSize;

    /*
     * Initialize the DMAI library 
     */
    wd = (DmaiDisplay *) ms_new0(DmaiDisplay, 1);
    wd->filter = f;
    obj->data = wd;

    Dmai_init();
    wd->dmai_initialized = TRUE;
    ms_mutex_init(&wd->dmai_mutex, NULL);
    ms_mutex_lock(&wd->dmai_mutex);
    wd->screen_size.width = fbuf->w;
    wd->screen_size.height = fbuf->h;
    /*
     * Create the display device instance 
     */
    dAttrs.displayStd = Display_Std_FBDEV;
    dAttrs.displayDevice = "/dev/fb1";
    wd->hDisplay = Display_create(NULL, &dAttrs);

    if (wd->hDisplay == NULL) {
	ERR("Failed to create display device\n");
    }
    /*
    * create the resize job 
    */
    wd->hRsz = Resize_create(&rszAttrs);
    if (wd->hRsz == NULL) {
        ERR("Failed to create resize job in display\n");
    }

    /*
     * Get a buffer from the display device driver 
     */
    if (Display_get(wd->hDisplay, &hDstBuf) < 0) {
	ERR("Failed to get display buffer\n");
    }

    bufSize = 480 * 320 * 2;//BufferGfx_calcSize(VideoStd_CIF, ColorSpace_UYVY);
    gfxAttrs.colorSpace = ColorSpace_UYVY;
    // BufferGfx_calcDimensions(VideoStd_CIF, ColorSpace_UYVY, &gfxAttrs.dim);
    gfxAttrs.dim.width = 480;
    gfxAttrs.dim.height = 320;
    gfxAttrs.dim.lineLength = BufferGfx_calcLineLength(gfxAttrs.dim.width,
						       gfxAttrs.
						       colorSpace);
    wd->vBuf = Buffer_create(bufSize + 16, BufferGfx_getBufferAttrs(&gfxAttrs));
    uyvy_buf_init(fbuf, fbuf->w, fbuf->h,
                  (uint8_t *)Buffer_getUserPtr(wd->vBuf));

    /*
     * config the reszie job 
     */
    if (Resize_config(wd->hRsz, wd->vBuf, hDstBuf) < 0) {
         ERR("Failed to config smooth job\n");
    }
    /*
     * Give the buffer back to the display device driver 
     */
    if (Display_put(wd->hDisplay, hDstBuf) < 0) {
	ERR("Failed to put display buffer\n");
    }

    ms_mutex_unlock(&wd->dmai_mutex);
    return TRUE;
}

static void
dmai_display_update(MSDisplay * obj, int new_image, int new_selfview)
{
    DmaiDisplay     *wd = (DmaiDisplay *) obj->data;
    Buffer_Handle   hDstBuf;

    ms_mutex_lock(&wd->dmai_mutex);
    /*
     * Get a buffer from the display device driver 
     */
    if (Display_get(wd->hDisplay, &hDstBuf) < 0) {
	ERR("Failed to get display buffer\n");
    }
    /*
     * resize the video 
     */
    if (Resize_execute(wd->hRsz, wd->vBuf, hDstBuf) < 0) {
        ERR("Failed to execute resize job\n");
    }
    
    // BufferGfx_resetDimensions(wd->vBuf);
    // BufferGfx_resetDimensions(hDstBuf);
    /*
     * Give a filled buffer back to the display device driver 
     */
    if (Display_put(wd->hDisplay, hDstBuf) < 0) {
	ERR("Failed to put display buffer\n");
    }

    ms_mutex_unlock(&wd->dmai_mutex);
}

static void
dmai_display_uninit(MSDisplay * obj)
{
    DmaiDisplay     *wd = (DmaiDisplay *) obj->data;
    int              bitt;
    if (wd == NULL)
	return;

    if (wd->hRsz) {
        Resize_delete(wd->hRsz);
    }

    if (wd->vBuf) {
        Buffer_delete(wd->vBuf);
    }
    if (wd->hDisplay != NULL)
	bitt = Display_delete(wd->hDisplay);
    ms_message("Display_delete return: %d", bitt);
    ms_free(wd);
}

MSDisplayDesc   ms_dmai_display_desc = {
    .init = dmai_display_init,
    .update = dmai_display_update,
    .uninit = dmai_display_uninit,
};

MSDisplay      *
ms_display_new(MSDisplayDesc * desc)
{
    MSDisplay      *obj = (MSDisplay *) ms_new0(MSDisplay, 1);
    obj->desc = desc;
    obj->data = NULL;
    return obj;
}

void
ms_display_set_window_id(MSDisplay * d, long id)
{
    d->window_id = id;
    d->use_external_window = TRUE;
}

void
ms_display_destroy(MSDisplay * obj)
{
    obj->desc->uninit(obj);
    ms_free(obj);
}

static MSDisplayDesc *default_display_desc = &ms_dmai_display_desc;

void
ms_display_desc_set_default(MSDisplayDesc * desc)
{
    default_display_desc = desc;
}

MSDisplayDesc  *
ms_display_desc_get_default(void)
{
    return default_display_desc;
}

void
ms_display_desc_set_default_window_id(MSDisplayDesc * desc, long id)
{
    desc->default_window_id = id;
}

typedef struct VideoOut {
    AVRational      ratio;
    MSPicture       fbuf;
    MSPicture       fbuf_selfview;
    MSPicture       local_pic;
    MSRect          local_rect;
    mblk_t         *local_msg;
    MSVideoSize     prevsize;
    int             corner;	/* for selfview */
    float           scale_factor;	/* for selfview */
    float           sv_posx,
                    sv_posy;
    int             background_color[3];

    struct ms_SwsContext *sws1;
    struct ms_SwsContext *sws2;
    MSDisplay      *display;
    bool_t          own_display;
    bool_t          ready;
    bool_t          autofit;
    bool_t          mirror;
} VideoOut;

static void
set_corner(VideoOut * s, int corner)
{
    s->corner = corner;
    s->local_pic.w = ((int) (s->fbuf.w / s->scale_factor)) & ~0x1;
    s->local_pic.h = ((int) (s->fbuf.h / s->scale_factor)) & ~0x1;
    s->local_rect.w = s->local_pic.w;
    s->local_rect.h = s->local_pic.h;
    if (corner == 1) {
	/*
	 * top left corner 
	 */
	s->local_rect.x = 0;
	s->local_rect.y = 0;
    } else if (corner == 2) {
	/*
	 * top right corner 
	 */
	s->local_rect.x = s->fbuf.w - s->local_pic.w;
	s->local_rect.y = 0;
    } else if (corner == 3) {
	/*
	 * bottom left corner 
	 */
	s->local_rect.x = 0;
	s->local_rect.y = s->fbuf.h - s->local_pic.h;
    } else {
	/*
	 * default: bottom right corner 
	 */
	/*
	 * corner can be set to -1: to disable the self view... 
	 */
	s->local_rect.x = s->fbuf.w - s->local_pic.w;
	s->local_rect.y = s->fbuf.h - s->local_pic.h;
    }
    s->fbuf_selfview.w = (s->fbuf.w / 1) & ~0x1;
    s->fbuf_selfview.h = (s->fbuf.h / 1) & ~0x1;
}

static void
re_vsize(VideoOut * s, MSVideoSize * sz)
{
    ms_message("Windows size set to %ix%i", sz->width, sz->height);
}

static void
set_vsize(VideoOut * s, MSVideoSize * sz)
{
    s->fbuf.w = sz->width & ~0x1;
    s->fbuf.h = sz->height & ~0x1;
    set_corner(s, s->corner);
    ms_message("Video size set to %ix%i", s->fbuf.w, s->fbuf.h);
}

static void
video_out_init(MSFilter * f)
{
    VideoOut       *obj = (VideoOut *) ms_new0(VideoOut, 1);
    MSVideoSize     def_size;
    obj->ratio.num = 11;
    obj->ratio.den = 9;
    def_size.width = MS_VIDEO_SIZE_CIF_W;
    def_size.height = MS_VIDEO_SIZE_CIF_H;
    obj->prevsize.width = 0;
    obj->prevsize.height = 0;
    obj->local_msg = NULL;
    obj->corner = 0;
    obj->scale_factor = SCALE_FACTOR;
    obj->sv_posx = obj->sv_posy = SELVIEW_POS_INACTIVE;
    obj->background_color[0] = obj->background_color[1] =
	obj->background_color[2] = 0;
    obj->sws1 = NULL;
    obj->sws2 = NULL;
    obj->display = NULL;
    obj->own_display = FALSE;
    obj->ready = FALSE;
    obj->autofit = FALSE;
    obj->mirror = FALSE;
    set_vsize(obj, &def_size);
    f->data = obj;
    ms_message("video out inited......");
}

static void
video_out_uninit(MSFilter * f)
{
    VideoOut       *obj = (VideoOut *) f->data;
    if (obj->display != NULL && obj->own_display)
	ms_display_destroy(obj->display);
    if (obj->sws1 != NULL) {
	ms_sws_freeContext(obj->sws1);
	obj->sws1 = NULL;
    }
    if (obj->sws2 != NULL) {
	ms_sws_freeContext(obj->sws2);
	obj->sws2 = NULL;
    }
    if (obj->local_msg != NULL) {
	freemsg(obj->local_msg);
	obj->local_msg = NULL;
    }
    ms_message("video out uninited......");
    ms_free(obj);
}

static void
video_out_prepare(MSFilter * f)
{
    VideoOut       *obj = (VideoOut *) f->data;
    if (obj->display == NULL) {
	if (default_display_desc == NULL) {
	    ms_error("No default display built in !");
	    return;
	}
	obj->display = ms_display_new(default_display_desc);
	obj->own_display = TRUE;
    }
    if (!ms_display_init(obj->display, f, &obj->fbuf, &obj->fbuf_selfview)) {
        if (obj->own_display)
            ms_display_destroy(obj->display);
        obj->display = NULL;
    }
    if (obj->sws1 != NULL) {
	ms_sws_freeContext(obj->sws1);
	obj->sws1 = NULL;
    }
    if (obj->sws2 != NULL) {
	ms_sws_freeContext(obj->sws2);
	obj->sws2 = NULL;
    }
    if (obj->local_msg != NULL) {
	freemsg(obj->local_msg);
	obj->local_msg = NULL;
    }
    set_corner(obj, obj->corner);
    obj->ready = TRUE;
    ms_message("video out prepareed......");
}

static int
video_out_handle_resizing(MSFilter * f, void *data)
{
    /*
     * to be removed 
     */
    return -1;
}

static int
_video_out_handle_resizing(MSFilter * f, void *data)
{
    VideoOut       *s = (VideoOut *) f->data;
    MSDisplay      *disp = s->display;
    int             ret = -1;
    return ret;
}

static void
video_out_preprocess(MSFilter * f)
{
    video_out_prepare(f);
}

static void
video_out_process(MSFilter * f)
{
    VideoOut       *obj = (VideoOut *) f->data;
    mblk_t         *inm;
    int             update = 0;
    int             update_selfview = 0;
    int             i;

    for (i = 0; i < 100; ++i) {
	int             ret = _video_out_handle_resizing(f, NULL);
	if (ret < 0)
	    break;
    }
    ms_filter_lock(f);
    if (!obj->ready)
	video_out_prepare(f);
    if (obj->display == NULL) {
	ms_filter_unlock(f);
	if (f->inputs[0] != NULL)
	    ms_queue_flush(f->inputs[0]);
	if (f->inputs[1] != NULL)
	    ms_queue_flush(f->inputs[1]);
	return;
    }
    /*
     * get most recent message and draw it 
     */
    if (f->inputs[1] != NULL
	&& (inm = ms_queue_peek_last(f->inputs[1])) != 0) {
	// if (obj->corner == -1) {
	//     if (obj->local_msg != NULL) {
	// 	freemsg(obj->local_msg);
	// 	obj->local_msg = NULL;
	//     }
	// /* } else if (obj->fbuf_selfview.planes[0] != NULL) {
	//     MSPicture       src;
	//     if (yuv_buf_init_from_mblk(&src, inm) == 0) {

	// 	if (obj->sws2 == NULL) {
	// 	    obj->sws2 =
	// 		ms_sws_getContext(src.w, src.h,
	// 				  PIX_FMT_YUV420P,
	// 				  obj->fbuf_selfview.w,
	// 				  obj->fbuf_selfview.h,
	// 				  PIX_FMT_UYVY422,
	// 				  SWS_FAST_BILINEAR, NULL, NULL,
	// 				  NULL);
	// 	}
	// 	ms_display_lock(obj->display);
	// 	if (ms_sws_scale
	// 	    (obj->sws2, src.planes, src.strides, 0,
	// 	     src.h, obj->fbuf_selfview.planes,
	// 	     obj->fbuf_selfview.strides) < 0) {
	// 	    ms_error("Error in ms_sws_scale().");
	// 	}
	// 	// if (!mblk_get_precious_flag(inm))
	// 	//     ms_yuv_buf_mirror(&obj->fbuf_selfview);
	// 	ms_display_unlock(obj->display);
	// 	update_selfview = 1;
	//     } */
	// } else {
	//     MSPicture       src;
	//     if (yuv_buf_init_from_mblk(&src, inm) == 0) {

	// 	if (obj->sws2 == NULL) {
	// 	    obj->sws2 =
	// 		ms_sws_getContext(src.w, src.h,
	// 				  PIX_FMT_YUV420P,
	// 				  obj->local_pic.w,
	// 				  obj->local_pic.h,
	// 				  PIX_FMT_UYVY422,
	// 				  SWS_FAST_BILINEAR, NULL, NULL,
	// 				  NULL);
	// 	}
	// 	// if (obj->local_msg == NULL) {
	// 	//     obj->local_msg =
	// 	// 	uyvy_buf_alloc(&obj->local_pic,
	// 	// 		      obj->local_pic.w, obj->local_pic.h);
	// 	// }
	// 	// if (obj->local_pic.planes[0] != NULL) {
	// 	if (obj->fbuf.planes[0] != NULL) {
	// 	    if (ms_sws_scale
	// 		(obj->sws2, src.planes, src.strides,
	// 		 0, src.h, obj->fbuf.planes,
	// 		 obj->fbuf.strides) < 0) {
	// 		ms_error("Error in ms_sws_scale().");
	// 	    }
	// 	    // if (!mblk_get_precious_flag(inm))
	// 	    //     ms_yuv_buf_mirror(&obj->local_pic);
	// 	    update = 1;
	// 	}
	//     }
	// }
	ms_queue_flush(f->inputs[1]);
    }
    
    if (f->inputs[0] != NULL
        && (inm = ms_queue_peek_last(f->inputs[0])) != 0) {
        MSPicture       src;
        if (yuv_buf_init_from_mblk(&src, inm) == 0) {
            MSVideoSize     cur,
                            newsize;
            cur.width = obj->fbuf.w;
            cur.height = obj->fbuf.h;
            newsize.width = src.w;
            newsize.height = src.h;
            // if (obj->autofit
            //     && !ms_video_size_equal(newsize, obj->prevsize)) {
            //     MSVideoSize     qvga_size;
            //     qvga_size.width = MS_VIDEO_SIZE_QVGA_W;
            //     qvga_size.height = MS_VIDEO_SIZE_QVGA_H;
            //     obj->prevsize = newsize;
            //     ms_message("received size is %ix%i",
            //     	   newsize.width, newsize.height);
            //     /*
            //      * don't resize less than QVGA, it is too small 
            //      */
            //     if (ms_video_size_greater_than(qvga_size, newsize)) {
            //         newsize.width = MS_VIDEO_SIZE_QVGA_W;
            //         newsize.height = MS_VIDEO_SIZE_QVGA_H;
            //     }
            //     if (!ms_video_size_equal(newsize, cur)) {
            //         set_vsize(obj, &newsize);
            //         ms_message("autofit: new size is %ix%i",
            //     	       newsize.width, newsize.height);
            //         video_out_prepare(f);
            //     }
            // }
            if (obj->sws1 == NULL) {
        	obj->sws1 =
        	    ms_sws_getContext(src.w, src.h,
        			      PIX_FMT_YUV420P,
        			      obj->fbuf.w, obj->fbuf.h,
        			      PIX_FMT_UYVY422,
        			      SWS_FAST_BILINEAR, NULL, NULL, NULL);
            }
            ms_display_lock(obj->display);
            if (ms_sws_scale(obj->sws1, src.planes, src.strides, 0,
        		     src.h, obj->fbuf.planes,
        		     obj->fbuf.strides) < 0) {
        	ms_error("Error in ms_sws_scale().");
            }
            // if (obj->mirror && !mblk_get_precious_flag(inm))
            //     ms_yuv_buf_mirror(&obj->fbuf);
            ms_display_unlock(obj->display);
        }
        update = 1;
        ms_queue_flush(f->inputs[0]);
    }

    /*
     * copy resized local view into main buffer, at bottom left corner: 
     */
    // if (obj->local_msg != NULL) {
    //     MSPicture       corner = obj->fbuf;
    //     MSVideoSize     roi;
    //     roi.width = obj->local_pic.w;
    //     roi.height = obj->local_pic.h;
    //     corner.w = obj->local_pic.w;
    //     corner.h = obj->local_pic.h;
    //     corner.planes[0] +=
    //         obj->local_rect.x + (obj->local_rect.y * corner.strides[0]);
    //     corner.planes[1] +=
    //         (obj->local_rect.x / 2) +
    //         ((obj->local_rect.y / 2) * corner.strides[1]);
    //     corner.planes[2] +=
    //         (obj->local_rect.x / 2) +
    //         ((obj->local_rect.y / 2) * corner.strides[2]);
    //     corner.planes[3] = 0;
    //     ms_display_lock(obj->display);
    //     ms_yuv_buf_copy(obj->local_pic.planes, obj->local_pic.strides,
    //     		corner.planes, corner.strides, roi);
    //     ms_display_unlock(obj->display);
    // }

    ms_display_update(obj->display, update, update_selfview);
    ms_filter_unlock(f);
}

static int
video_out_set_vsize(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    ms_filter_lock(f);
    set_vsize(s, (MSVideoSize *) arg);
    ms_filter_unlock(f);
    return 0;
}

static int
video_out_set_display(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->display = (MSDisplay *) arg;
    return 0;
}

static int
video_out_auto_fit(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->autofit = *(int *) arg;
    return 0;
}

static int
video_out_set_corner(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->sv_posx = s->sv_posy = SELVIEW_POS_INACTIVE;
    ms_filter_lock(f);
    set_corner(s, *(int *) arg);
    if (s->display) {
	ms_display_lock(s->display);
	{
	    int             w = s->fbuf.w;
	    int             h = s->fbuf.h;
	    int             ysize = w * h;
	    int             usize = ysize >> 1;

	    memset(s->fbuf.planes[0], 0, ysize);
	    memset(s->fbuf.planes[1], 0, usize);
	    memset(s->fbuf.planes[2], 0, usize);
	    s->fbuf.planes[3] = NULL;
	}
	ms_display_unlock(s->display);
    }
    ms_filter_unlock(f);
    return 0;
}

static int
video_out_get_corner(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    *((int *) arg) = s->corner;
    return 0;
}

static int
video_out_set_scalefactor(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->scale_factor = *(float *) arg;
    if (s->scale_factor < 0.5f)
	s->scale_factor = 0.5f;
    ms_filter_lock(f);
    set_corner(s, s->corner);
    if (s->display) {
	ms_display_lock(s->display);
	{
	    int             w = s->fbuf.w;
	    int             h = s->fbuf.h;
	    int             ysize = w * h;
	    int             usize = ysize / 4;

	    memset(s->fbuf.planes[0], 0, ysize);
	    memset(s->fbuf.planes[1], 0, usize);
	    memset(s->fbuf.planes[2], 0, usize);
	    s->fbuf.planes[3] = NULL;
	}
	ms_display_unlock(s->display);
    }
    ms_filter_unlock(f);
    return 0;
}

static int
video_out_get_scalefactor(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    *((float *) arg) = (float) s->scale_factor;
    return 0;
}

static int
video_out_enable_mirroring(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->mirror = *(int *) arg;
    return 0;
}

static int
video_out_get_native_window_id(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    unsigned long  *id = (unsigned long *) arg;
    *id = 0;
    if (s->display) {
	*id = s->display->window_id;
	return 0;
    }
    return -1;
}

static int
video_out_set_selfview_pos(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->sv_posx = ((float *) arg)[0];
    s->sv_posy = ((float *) arg)[1];
    s->scale_factor = (float) 100.0 / ((float *) arg)[2];
    return 0;
}

static int
video_out_get_selfview_pos(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    ((float *) arg)[0] = s->sv_posx;
    ((float *) arg)[1] = s->sv_posy;
    ((float *) arg)[2] = (float) 100.0 / s->scale_factor;
    return 0;
}

static int
video_out_set_background_color(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    s->background_color[0] = ((int *) arg)[0];
    s->background_color[1] = ((int *) arg)[1];
    s->background_color[2] = ((int *) arg)[2];
    return 0;
}

static int
video_out_get_background_color(MSFilter * f, void *arg)
{
    VideoOut       *s = (VideoOut *) f->data;
    ((int *) arg)[0] = s->background_color[0];
    ((int *) arg)[1] = s->background_color[1];
    ((int *) arg)[2] = s->background_color[2];
    return 0;
}

static MSFilterMethod methods[] = {
    {MS_FILTER_SET_VIDEO_SIZE, video_out_set_vsize},
    {MS_VIDEO_OUT_SET_DISPLAY, video_out_set_display},
    {MS_VIDEO_OUT_SET_CORNER, video_out_set_corner},
    {MS_VIDEO_OUT_AUTO_FIT, video_out_auto_fit},
    {MS_VIDEO_OUT_HANDLE_RESIZING, video_out_handle_resizing},
    {MS_VIDEO_OUT_ENABLE_MIRRORING, video_out_enable_mirroring},
    {MS_VIDEO_OUT_GET_NATIVE_WINDOW_ID, video_out_get_native_window_id},
    {MS_VIDEO_OUT_GET_CORNER, video_out_get_corner},
    {MS_VIDEO_OUT_SET_SCALE_FACTOR, video_out_set_scalefactor},
    {MS_VIDEO_OUT_GET_SCALE_FACTOR, video_out_get_scalefactor},
    {MS_VIDEO_OUT_SET_SELFVIEW_POS, video_out_set_selfview_pos},
    {MS_VIDEO_OUT_GET_SELFVIEW_POS, video_out_get_selfview_pos},
    {MS_VIDEO_OUT_SET_BACKGROUND_COLOR, video_out_set_background_color},
    {MS_VIDEO_OUT_GET_BACKGROUND_COLOR, video_out_get_background_color},
    /*
     * methods for compatibility with the MSVideoDisplay interface
     */
    {MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE, video_out_set_corner},
    {MS_VIDEO_DISPLAY_ENABLE_AUTOFIT, video_out_auto_fit},
    {MS_VIDEO_DISPLAY_ENABLE_MIRRORING, video_out_enable_mirroring},
    {MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID,
     video_out_get_native_window_id},
    {MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_SCALEFACTOR,
     video_out_set_scalefactor},
    {MS_VIDEO_DISPLAY_SET_SELFVIEW_POS, video_out_set_selfview_pos},
    {MS_VIDEO_DISPLAY_GET_SELFVIEW_POS, video_out_get_selfview_pos},
    {MS_VIDEO_DISPLAY_SET_BACKGROUND_COLOR,
     video_out_set_background_color},

    {0, NULL}
};

MSFilterDesc    ms_video_out_desc = {
    .id = MS_VIDEO_OUT_ID,
    .name = "MSVideoOut",
    .text = N_("A generic video display"),
    .category = MS_FILTER_OTHER,
    .ninputs = 2,
    .noutputs = 0,
    .init = video_out_init,
    .preprocess = video_out_preprocess,
    .process = video_out_process,
    .uninit = video_out_uninit,
    .methods = methods
};

MS_FILTER_DESC_EXPORT(ms_video_out_desc)
