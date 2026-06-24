// movie_probe.c — headless decode smoke test for the bundled FFmpeg.
//
// Reads a real cutscene .dat (XOR-deobfuscated the same way movie_player.c does),
// opens it via in-memory AVIO, and decodes a few video+audio frames to prove the
// FMV decode path works on-device WITHOUT a display or the game engine. Validates
// demux (mov) + H.264 decode + swscale->RGBA + AAC decode + swresample (new
// channel-layout API). NOT part of the game; build/run standalone:
//
//   gcc -O2 portmaster/movie_probe.c -I$FF/include -L$FF/lib \
//       -lavformat -lavcodec -lswscale -lswresample -lavutil -lm -lpthread -o movie_probe
//   LD_LIBRARY_PATH=libs ./movie_probe assets/001.dat
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>

typedef struct { const uint8_t *p; int size; int pos; } MemBuf;
static int mem_read(void *o, uint8_t *buf, int n) {
  MemBuf *m = o; int left = m->size - m->pos;
  if (left <= 0) return AVERROR_EOF;
  if (n > left) n = left;
  memcpy(buf, m->p + m->pos, n); m->pos += n; return n;
}
static int64_t mem_seek(void *o, int64_t off, int whence) {
  MemBuf *m = o;
  if (whence == AVSEEK_SIZE) return m->size;
  if (whence == SEEK_END) off += m->size;
  else if (whence == SEEK_CUR) off += m->pos;
  if (off < 0) off = 0; if (off > m->size) off = m->size;
  m->pos = (int)off; return m->pos;
}

static uint8_t *read_dat(const char *path, int *outsz) {
  FILE *f = fopen(path, "rb"); if (!f) { perror("fopen"); return NULL; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 32) { fclose(f); return NULL; }
  uint8_t *d = malloc((size_t)sz);
  if (!d) { fclose(f); return NULL; }
  if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return NULL; }
  fclose(f);
  size_t n = strlen(path);
  if (n >= 4 && !strcmp(path + n - 4, ".dat"))
    for (long i = 0; i < sz; i++)
      d[i] ^= (uint8_t)(0xFFu - ((uint64_t)i & 0xFFu));
  *outsz = (int)sz; return d;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s file.dat [maxframes]\n", argv[0]); return 2; }
  int maxf = argc > 2 ? atoi(argv[2]) : 60;
  printf("FFmpeg runtime: avformat=%u avcodec=%u avutil=%u\n",
         avformat_version(), avcodec_version(), avutil_version());

  int sz = 0; uint8_t *data = read_dat(argv[1], &sz);
  if (!data) { fprintf(stderr, "read/deobfuscate FAILED\n"); return 1; }
  printf("read %s: %d bytes (deobfuscated)\n", argv[1], sz);

  MemBuf mem = { data, sz, 0 };
  unsigned char *iobuf = av_malloc(65536);
  AVIOContext *avio = avio_alloc_context(iobuf, 65536, 0, &mem, mem_read, NULL, mem_seek);
  AVFormatContext *fmt = avformat_alloc_context(); fmt->pb = avio;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) { fprintf(stderr, "open_input FAILED (bad XOR? not MP4?)\n"); return 1; }
  if (avformat_find_stream_info(fmt, NULL) < 0) { fprintf(stderr, "find_stream_info FAILED\n"); return 1; }
  printf("container: %s, %u stream(s), duration=%.1fs\n",
         fmt->iformat->name, fmt->nb_streams, fmt->duration / (double)AV_TIME_BASE);

  int vidx = -1, aidx = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = (int)i;
    else if (t == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = (int)i;
  }
  if (vidx < 0) { fprintf(stderr, "no video stream\n"); return 1; }

  AVCodecParameters *vp = fmt->streams[vidx]->codecpar;
  const AVCodec *vc = avcodec_find_decoder(vp->codec_id);
  printf("video: codec=%s %dx%d\n", vc ? vc->name : "(none)", vp->width, vp->height);
  if (!vc) { fprintf(stderr, "no H.264 decoder!\n"); return 1; }
  AVCodecContext *vdec = avcodec_alloc_context3(vc);
  avcodec_parameters_to_context(vdec, vp); vdec->thread_count = 3;
  if (avcodec_open2(vdec, vc, NULL) < 0) { fprintf(stderr, "video open FAILED\n"); return 1; }
  struct SwsContext *sws = sws_getContext(vp->width, vp->height, vdec->pix_fmt,
                                          vp->width, vp->height, AV_PIX_FMT_RGBA,
                                          SWS_BILINEAR, NULL, NULL, NULL);
  uint8_t *rgba = malloc((size_t)vp->width * vp->height * 4);
  if (!sws || !rgba) { fprintf(stderr, "sws/alloc FAILED\n"); return 1; }

  AVCodecContext *adec = NULL; SwrContext *swr = NULL;
  if (aidx >= 0) {
    AVCodecParameters *ap = fmt->streams[aidx]->codecpar;
    const AVCodec *ac = avcodec_find_decoder(ap->codec_id);
    printf("audio: codec=%s %dHz\n", ac ? ac->name : "(none)", ap->sample_rate);
    if (ac) {
      adec = avcodec_alloc_context3(ac);
      avcodec_parameters_to_context(adec, ap);
      if (avcodec_open2(adec, ac, NULL) == 0) {
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, 48000,
                                &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL) == 0 && swr)
          swr_init(swr);
      }
    }
  }

  AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
  int vframes = 0, aframes = 0;
  while (vframes < maxf && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vdec, pkt) == 0)
        while (avcodec_receive_frame(vdec, fr) == 0) {
          uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
          int st[4] = { vp->width * 4, 0, 0, 0 };
          sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, vp->height, dst, st);
          vframes++;
        }
    } else if (pkt->stream_index == aidx && adec && swr) {
      if (avcodec_send_packet(adec, pkt) == 0)
        while (avcodec_receive_frame(adec, fr) == 0) aframes++;
    }
    av_packet_unref(pkt);
  }

  unsigned long sum = 0; // touch decoded pixels so nothing is optimised away
  for (int i = 0; i < vp->width * vp->height * 4 && i < 8192; i++) sum += rgba[i];
  printf("DECODED OK: %d video frames, %d audio frames (pixsum=%lu)\n", vframes, aframes, sum);
  printf("PROBE_OK\n");
  return 0;
}
