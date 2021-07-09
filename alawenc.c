#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include "ringbuf.h"
#include "codec.h"
#include "utils.h"

#define OUT_BUF_SIZE (1024 * 1 * 1)
typedef struct {
    CODEC_INTERFACE_FUNCS

    uint8_t  obuff[OUT_BUF_SIZE];
    int      ohead;
    int      otail;
    int      osize;

    #define TS_EXIT  (1 << 0)
    #define TS_START (1 << 1)
    int      status;

    pthread_mutex_t omutex;
    pthread_cond_t  ocond ;
    pthread_t       thread;
} ALAWENC;

static void alawenc_uninit(void *ctxt)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    pthread_mutex_destroy(&enc->omutex);
    pthread_cond_destroy (&enc->ocond );
    free(enc);
}

static uint8_t pcm2alaw(int16_t pcm)
{
    uint8_t sign = (pcm >> 8) & (1 << 7);
    int  mask, eee, wxyz, alaw;
    if (sign) pcm = -pcm;
    for (mask=0x4000,eee=7; (pcm&mask)==0&&eee>0; eee--,mask>>=1);
    wxyz  = (pcm >> ((eee == 0) ? 4 : (eee + 3))) & 0xf;
    alaw  = sign | (eee << 4) | wxyz;
    return (alaw ^ 0xd5);
}

static void alawenc_write(void *ctxt, void *buf, int len)
{
    uint32_t timestamp, typelen, i;
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    pthread_mutex_lock(&enc->omutex);
    typelen = len / sizeof(int16_t);
    if (sizeof(uint32_t) + sizeof(uint32_t) + typelen <= sizeof(enc->obuff) - enc->osize) {
        timestamp = get_tick_count();
        typelen   = 'A' | (typelen << 8);
        enc->otail = ringbuf_write(enc->obuff, sizeof(enc->obuff), enc->otail, (uint8_t*)&timestamp, sizeof(timestamp));
        enc->otail = ringbuf_write(enc->obuff, sizeof(enc->obuff), enc->otail, (uint8_t*)&typelen  , sizeof(typelen  ));
        enc->osize+= sizeof(timestamp) + sizeof(typelen);
        for (i=0; i<len/sizeof(int16_t); i++,enc->osize++) {
            if (enc->otail == sizeof(enc->obuff)) enc->otail = 0;
            enc->obuff[enc->otail++] = pcm2alaw(((int16_t*)buf)[i]);
        }
        pthread_cond_signal(&enc->ocond);
    } else {
        printf("aenc drop data %d !\n", len);
    }
    pthread_mutex_unlock(&enc->omutex);
}

static int alawenc_read(void *ctxt, void *buf, int len, int *fsize, int *key, uint32_t *pts, int timeout)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    uint32_t timestamp = 0;
    int32_t  framesize = 0, readsize = 0, ret = 0;
    struct   timespec ts;
    if (!ctxt) return 0;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += timeout*1000*1000;
    ts.tv_sec  += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;

    pthread_mutex_lock(&enc->omutex);
    while (timeout && enc->osize <= 0 && (enc->status & TS_START) && ret != ETIMEDOUT) ret = pthread_cond_timedwait(&enc->ocond, &enc->omutex, &ts);
    if (enc->osize > 0) {
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead, (uint8_t*)&timestamp, sizeof(timestamp));
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead, (uint8_t*)&framesize, sizeof(framesize));
        enc->osize-= sizeof(timestamp) + sizeof(framesize);
        framesize  = ((uint32_t)framesize >> 8);
        readsize   = MIN(len, framesize);
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead,  buf , readsize);
        enc->ohead = ringbuf_read(enc->obuff, sizeof(enc->obuff), enc->ohead,  NULL, framesize - readsize);
        enc->osize-= framesize;
    }
    if (pts  ) *pts   = timestamp;
    if (fsize) *fsize = framesize;
    if (key  ) *key   = 1;
    pthread_mutex_unlock(&enc->omutex);
    return readsize;
}

static void alawenc_start(void *ctxt, int start)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    if (start) {
        enc->status |= TS_START;
    } else {
        pthread_mutex_lock(&enc->omutex);
        enc->status &= ~TS_START;
        pthread_cond_signal(&enc->ocond);
        pthread_mutex_unlock(&enc->omutex);
    }
}

static void alawenc_reset(void *ctxt, int type)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    if (type & (CODEC_CLEAR_INBUF|CODEC_CLEAR_OUTBUF)) {
        pthread_mutex_lock(&enc->omutex);
        enc->ohead = enc->otail = enc->osize = 0;
        pthread_mutex_unlock(&enc->omutex);
    }
}

static void alawenc_obuflock(void *ctxt, uint8_t **pbuf, int *max, int *head, int *tail, int *size)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    pthread_mutex_lock(&enc->omutex);
    *pbuf = enc->obuff;
    *max  = sizeof(enc->obuff);
    *head = enc->ohead;
    *tail = enc->otail;
    *size = enc->osize;
}

static void alawenc_obufunlock(void *ctxt, int head, int tail, int size)
{
    ALAWENC *enc = (ALAWENC*)ctxt;
    if (!ctxt) return;
    enc->ohead = head;
    enc->otail = tail;
    enc->osize = size;
    pthread_mutex_unlock(&enc->omutex);
}

CODEC* alawenc_init(void)
{
    ALAWENC *enc = calloc(1, sizeof(ALAWENC));
    if (!enc) return NULL;

    strncpy(enc->name, "alawenc", sizeof(enc->name));
    enc->uninit     = alawenc_uninit;
    enc->write      = alawenc_write;
    enc->read       = alawenc_read;
    enc->start      = alawenc_start;
    enc->reset      = alawenc_reset;
    enc->obuflock   = alawenc_obuflock;
    enc->obufunlock = alawenc_obufunlock;

    // init mutex & cond
    pthread_mutex_init(&enc->omutex, NULL);
    pthread_cond_init (&enc->ocond , NULL);
    return (CODEC*)enc;
}
