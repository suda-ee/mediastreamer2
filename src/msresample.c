/*
mediastreamer2 library - modular sound and video processing and streaming
Copyright (C) 2006  Simon MORLAT (simon.morlat@linphone.org)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "mediastreamer2/msfilter.h"

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include <speex/speex_resampler.h>
#include <math.h>

typedef struct _ResampleData{
	MSBufferizer *bz;
	uint32_t ts;
	uint32_t input_rate;
	uint32_t output_rate;
	int nchannels;
	SpeexResamplerState *handle;
} ResampleData;

static ResampleData * resample_data_new(){
	ResampleData *obj=(ResampleData *)ms_new(ResampleData,1);
	obj->bz=ms_bufferizer_new();
	obj->ts=0;
	obj->input_rate=8000;
	obj->output_rate=16000;
	obj->handle=NULL;
	obj->nchannels=1;
	return obj;
}

static void resample_data_destroy(ResampleData *obj){
	if (obj->handle!=NULL)
		speex_resampler_destroy(obj->handle);
	ms_bufferizer_destroy(obj->bz);
	ms_free(obj);
}

static void resample_init(MSFilter *obj){
	obj->data=resample_data_new();
}

static void resample_uninit(MSFilter *obj){
	resample_data_destroy((ResampleData*)obj->data); 
}

static void resample_process_ms2(MSFilter *obj){
	ResampleData *dt=(ResampleData*)obj->data;
	mblk_t *m;
	
	if (dt->output_rate==dt->input_rate){
		while((m=ms_queue_get(obj->inputs[0]))!=NULL){
			ms_queue_put(obj->outputs[0],m);
		}
		return;
	}
	ms_filter_lock(obj);
	if (dt->handle!=NULL){
		unsigned int inrate=0, outrate=0;
		speex_resampler_get_rate(dt->handle,&inrate,&outrate);
		if (inrate!=dt->input_rate || outrate!=dt->output_rate){
			speex_resampler_destroy(dt->handle);
			dt->handle=0;
		}
	}
	if (dt->handle==NULL){
		int err=0;
		dt->handle=speex_resampler_init(dt->nchannels, dt->input_rate, dt->output_rate, SPEEX_RESAMPLER_QUALITY_VOIP, &err);
	}

	
	while((m=ms_queue_get(obj->inputs[0]))!=NULL){
		unsigned int inlen=(m->b_wptr-m->b_rptr)/(2*dt->nchannels);
		unsigned int outlen=((inlen*dt->output_rate)/dt->input_rate)+1;
		unsigned int inlen_orig=inlen;
		mblk_t *om=allocb(outlen*2*dt->nchannels,0);
		if (dt->nchannels==1){
			speex_resampler_process_int(dt->handle, 
					0, 
					(int16_t*)m->b_rptr, 
					&inlen, 
					(int16_t*)om->b_wptr, 
					&outlen);
		}else{
			speex_resampler_process_interleaved_int(dt->handle, 
					(int16_t*)m->b_rptr, 
					&inlen, 
					(int16_t*)om->b_wptr, 
					&outlen);
		}
		if (inlen_orig!=inlen){
			ms_error("Bug in resampler ! only %u samples consumed instead of %u, out=%u",
				inlen,inlen_orig,outlen);
		}
		om->b_wptr+=outlen*2*dt->nchannels;
		mblk_set_timestamp_info(om,dt->ts);
		dt->ts+=outlen;
		ms_queue_put(obj->outputs[0],om);
		freemsg(m);
	}
	ms_filter_unlock(obj);
}


static int ms_resample_set_sr(MSFilter *obj, void *arg){
	ResampleData *dt=(ResampleData*)obj->data;
	dt->input_rate=((int*)arg)[0];
	return 0;
}

static int ms_resample_set_output_sr(MSFilter *obj, void *arg){
	ResampleData *dt=(ResampleData*)obj->data;
	dt->output_rate=((int*)arg)[0];
	return 0;
}

static int set_nchannels(MSFilter *f, void *arg){
	ResampleData *dt=(ResampleData*)f->data;
	int chans=*(int*)arg;
	ms_filter_lock(f);
	if (dt->nchannels!=chans && dt->handle!=NULL){
		speex_resampler_destroy(dt->handle);
		dt->handle=NULL;
	}
	dt->nchannels=*(int*)arg;
	ms_filter_unlock(f);
	return 0;
}

static MSFilterMethod methods[]={
	{	MS_FILTER_SET_SAMPLE_RATE	 ,	ms_resample_set_sr		},
	{	MS_FILTER_SET_OUTPUT_SAMPLE_RATE ,	ms_resample_set_output_sr	},
	{ MS_FILTER_SET_NCHANNELS, set_nchannels },
	{	0				 ,	NULL	}
};

#ifdef _MSC_VER

MSFilterDesc ms_resample_desc={
	MS_RESAMPLE_ID,
	"MSResample",
	N_("Audio resampler"),
	MS_FILTER_OTHER,
	NULL,
	1,
	1,
	resample_init,
	NULL,
	resample_process_ms2,
	NULL,
	resample_uninit,
	methods
};

#else

MSFilterDesc ms_resample_desc={
	.id=MS_RESAMPLE_ID,
	.name="MSResample",
	.text=N_("Audio resampler"),
	.category=MS_FILTER_OTHER,
	.ninputs=1,
	.noutputs=1,
	.init=resample_init,
	.process=resample_process_ms2,
	.uninit=resample_uninit,
	.methods=methods
};

#endif

MS_FILTER_DESC_EXPORT(ms_resample_desc)

