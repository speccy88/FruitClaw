/****************************************************************************
 * apps/system/rtttl/rtttl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mqueue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

#include <audioutils/rtttl.h>

#ifdef CONFIG_ARCH_BOARD_ADAFRUIT_FRUIT_JAM_RP2350
#  include <arch/board/board.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SYSTEM_RTTTL_DEVPATH
#  define CONFIG_SYSTEM_RTTTL_DEVPATH "/dev/audio/pcm0"
#endif

#ifndef CONFIG_SYSTEM_RTTTL_TMPDIR
#  define CONFIG_SYSTEM_RTTTL_TMPDIR "/tmp"
#endif

#ifndef CONFIG_SYSTEM_RTTTL_SAMPLERATE
#  define CONFIG_SYSTEM_RTTTL_SAMPLERATE 8000
#endif

#ifndef CONFIG_SYSTEM_RTTTL_VOLUME
#  define CONFIG_SYSTEM_RTTTL_VOLUME 35
#endif

#define RTTTL_CHANNELS              2
#define RTTTL_BITS_PER_SAMPLE       16
#define RTTTL_BYTES_PER_SAMPLE      2
#define RTTTL_WAV_HEADER_SIZE       44
#define RTTTL_FRAMES_PER_CHUNK      128
#define RTTTL_GATE_PERCENT          88
#define RTTTL_TAIL_SILENCE_US       120000
#define RTTTL_MAX_PERCENT           100
#define RTTTL_AUDIO_WAIT_MS         3000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rtttl_builtin_s
{
  FAR const char *name;
  FAR const char *rtttl;
};

struct rtttl_playback_s
{
  int fd;
  uint32_t samplerate;
  uint8_t volume;
  uint32_t data_bytes;
  int error;
  CODE int (*write_samples)(FAR struct rtttl_playback_s *player,
                            FAR const int16_t *samples,
                            uint32_t nframes);
  FAR void *arg;
};

struct rtttl_audio_stream_s
{
  int fd;
  mqd_t mq;
  char mqname[32];
#ifdef CONFIG_AUDIO_MULTI_SESSION
  FAR void *session;
#endif
  FAR struct ap_buffer_s **buffers;
  uint16_t nbuffers;
  uint16_t next_buffer;
  apb_samp_t buffer_size;
  FAR struct ap_buffer_s *current;
  apb_samp_t current_bytes;
  bool registered;
  bool reserved;
  bool started;
  bool complete;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rtttl_builtin_s g_rtttl_builtins[] =
{
  {
    "gta3",
    "gta3:d=4,o=5,b=100:8b,8b,32b,16p,32p,16a,16b,16d6,16b,"
    "16p,16a,8b.,16p,16g,16p,8g,32g,32p,8f#,8f#,32f#,32p,8a,8a#"
  },
  {
    "scratchy",
    "Scratchy:o=5,d=8,b=160,b=160:c6,a,4p,c6,a6,4p,c6,a,c6,a,"
    "c6,a6,4p,p,c6,d6,e6,p,e6,f6,g6,4p,d6,c6,4d6,f6,4a#6,4a6,2c7"
  },
  {
    "simpsons",
    "Simpsons:o=5,d=8,b=160,b=160:c6.,4e6,4f#6,a6,4g6.,4e6,"
    "4c6,a,f#,f#,f#,2g,p,p,f#,f#,f#,g,4a#.,c6,c6,c6,4c6"
  },
  {
    "cantina",
    "Cantina:o=5,d=8,b=250,b=250:a,p,d6,p,a,p,d6,p,a,d6,p,a,p,"
    "g#,4a,a,g#,a,4g,f#,g,f#,4f.,d.,16p,4p.,a,p,d6,p,a,p,d6,p,"
    "a,d6,p,a,p,g#,a,p,g,p,4g.,f#,g,p,c6,4a#,4a,4g"
  },
};

static FAR struct rtttl_playback_s *g_rtttl_playback;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rtttl_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage: %s [-l] [-d dev] [-o wav] [-r hz] [-v pct] "
          "<name|rtttl-string>\n"
          "\n"
          "Built-in names: gta3 scratchy simpsons cantina\n"
          "Defaults: dev=%s rate=%d volume=%d\n",
          progname, CONFIG_SYSTEM_RTTTL_DEVPATH,
          CONFIG_SYSTEM_RTTTL_SAMPLERATE, CONFIG_SYSTEM_RTTTL_VOLUME);
}

static long rtttl_argtol(FAR const char *value, long minval, long maxval,
                         FAR const char *name)
{
  FAR char *endptr;
  long parsed;

  errno = 0;
  parsed = strtol(value, &endptr, 0);
  if (errno != 0 || endptr == value || *endptr != '\0' ||
      parsed < minval || parsed > maxval)
    {
      fprintf(stderr, "ERROR: invalid %s: %s\n", name, value);
      return -1;
    }

  return parsed;
}

static void rtttl_putle16(FAR uint8_t *buf, uint16_t value)
{
  buf[0] = value & 0xff;
  buf[1] = value >> 8;
}

static void rtttl_putle32(FAR uint8_t *buf, uint32_t value)
{
  buf[0] = value & 0xff;
  buf[1] = (value >> 8) & 0xff;
  buf[2] = (value >> 16) & 0xff;
  buf[3] = value >> 24;
}

static int rtttl_write_all(int fd, FAR const void *buffer, size_t buflen)
{
  FAR const uint8_t *ptr = buffer;
  ssize_t nwritten;

  while (buflen > 0)
    {
      nwritten = write(fd, ptr, buflen);
      if (nwritten < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (nwritten == 0)
        {
          return -EIO;
        }

      ptr += nwritten;
      buflen -= nwritten;
    }

  return OK;
}

static int rtttl_file_write_samples(FAR struct rtttl_playback_s *player,
                                    FAR const int16_t *samples,
                                    uint32_t nframes)
{
  return rtttl_write_all(player->fd, samples,
                         nframes * RTTTL_CHANNELS *
                         RTTTL_BYTES_PER_SAMPLE);
}

static int rtttl_write_wav_header(int fd, uint32_t samplerate,
                                  uint32_t data_bytes)
{
  uint8_t header[RTTTL_WAV_HEADER_SIZE];
  uint32_t byte_rate;
  uint16_t block_align;

  if (data_bytes > UINT32_MAX - 36)
    {
      return -EFBIG;
    }

  memset(header, 0, sizeof(header));
  memcpy(&header[0], "RIFF", 4);
  rtttl_putle32(&header[4], 36 + data_bytes);
  memcpy(&header[8], "WAVE", 4);
  memcpy(&header[12], "fmt ", 4);
  rtttl_putle32(&header[16], 16);
  rtttl_putle16(&header[20], 1);
  rtttl_putle16(&header[22], RTTTL_CHANNELS);
  rtttl_putle32(&header[24], samplerate);

  block_align = RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE;
  byte_rate = samplerate * block_align;
  rtttl_putle32(&header[28], byte_rate);
  rtttl_putle16(&header[32], block_align);
  rtttl_putle16(&header[34], RTTTL_BITS_PER_SAMPLE);
  memcpy(&header[36], "data", 4);
  rtttl_putle32(&header[40], data_bytes);

  return rtttl_write_all(fd, header, sizeof(header));
}

static int rtttl_emit_frames(FAR struct rtttl_playback_s *player,
                             uint32_t frequency_100hz, uint32_t nframes)
{
  int16_t samples[RTTTL_FRAMES_PER_CHUNK * RTTTL_CHANNELS];
  uint32_t active_frames;
  uint32_t phase = 0;
  uint32_t wrap;
  int16_t amplitude;

  active_frames = frequency_100hz == 0 ? 0 :
                  (uint64_t)nframes * RTTTL_GATE_PERCENT / 100;
  wrap = player->samplerate * 100;
  amplitude = (INT16_MAX * player->volume) / RTTTL_MAX_PERCENT;

  while (nframes > 0)
    {
      uint32_t chunk = nframes > RTTTL_FRAMES_PER_CHUNK ?
                       RTTTL_FRAMES_PER_CHUNK : nframes;
      uint32_t i;

      for (i = 0; i < chunk; i++)
        {
          int16_t sample = 0;

          if (active_frames > 0)
            {
              sample = phase < wrap / 2 ? amplitude : -amplitude;
              phase += frequency_100hz;
              while (phase >= wrap)
                {
                  phase -= wrap;
                }

              active_frames--;
            }

          samples[i * 2] = sample;
          samples[i * 2 + 1] = sample;
        }

      player->error = player->write_samples(player, samples, chunk);
      if (player->error < 0)
        {
          return player->error;
        }

      player->data_bytes += chunk * RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE;
      nframes -= chunk;
    }

  return OK;
}

static void rtttl_make_tone(struct rtttl_tone tone)
{
  FAR struct rtttl_playback_s *player = g_rtttl_playback;
  uint32_t nframes;

  if (player == NULL || player->error < 0)
    {
      return;
    }

  nframes = ((uint64_t)tone.duration_us * player->samplerate + 500000) /
            1000000;
  if (nframes == 0)
    {
      nframes = 1;
    }

  rtttl_emit_frames(player, tone.frequency_100hz, nframes);
}

static FAR const char *rtttl_builtin(FAR const char *name)
{
  size_t i;

  for (i = 0; i < nitems(g_rtttl_builtins); i++)
    {
      if (strcasecmp(name, g_rtttl_builtins[i].name) == 0)
        {
          return g_rtttl_builtins[i].rtttl;
        }
    }

  return NULL;
}

static void rtttl_list_builtins(void)
{
  size_t i;

  for (i = 0; i < nitems(g_rtttl_builtins); i++)
    {
      printf("%s\n", g_rtttl_builtins[i].name);
    }
}

static FAR char *rtttl_join_args(int argc, FAR char *argv[], int first)
{
  FAR char *joined;
  size_t len = 1;
  int i;

  for (i = first; i < argc; i++)
    {
      len += strlen(argv[i]) + 1;
    }

  joined = malloc(len);
  if (joined == NULL)
    {
      return NULL;
    }

  joined[0] = '\0';
  for (i = first; i < argc; i++)
    {
      if (i != first)
        {
          strlcat(joined, " ", len);
        }

      strlcat(joined, argv[i], len);
    }

  return joined;
}

static int rtttl_synth_pcm(FAR const char *path, FAR const char *tune,
                           uint32_t samplerate, uint8_t volume,
                           FAR uint32_t *data_bytes)
{
  struct rtttl_playback_s player;
  int ret;

  memset(&player, 0, sizeof(player));
  player.fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (player.fd < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: open %s failed: %d\n", path, errno);
      return ret;
    }

  player.samplerate = samplerate;
  player.volume = volume;
  player.write_samples = rtttl_file_write_samples;

  g_rtttl_playback = &player;
  ret = rtttl_play(tune, rtttl_make_tone);
  g_rtttl_playback = NULL;

  if (ret < 0)
    {
      fprintf(stderr, "ERROR: RTTTL parse/play failed: %d\n", ret);
      goto out;
    }

  if (player.error < 0)
    {
      ret = player.error;
      goto out;
    }

  ret = rtttl_emit_frames(&player, 0,
                          ((uint64_t)RTTTL_TAIL_SILENCE_US * samplerate +
                          500000) / 1000000);
  if (ret < 0)
    {
      goto out;
    }

  if (data_bytes != NULL)
    {
      *data_bytes = player.data_bytes;
    }

out:
  if (close(player.fd) < 0 && ret == OK)
    {
      ret = -errno;
    }

  return ret;
}

static int rtttl_export_wav(FAR const char *wavpath, FAR const char *pcmpath,
                            uint32_t samplerate, uint32_t data_bytes)
{
  uint8_t buffer[512];
  ssize_t nread;
  int pcmfd;
  int wavfd;
  int ret;

  pcmfd = open(pcmpath, O_RDONLY);
  if (pcmfd < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: open %s failed: %d\n", pcmpath, errno);
      return ret;
    }

  wavfd = open(wavpath, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  if (wavfd < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: open %s failed: %d\n", wavpath, errno);
      close(pcmfd);
      return ret;
    }

  ret = rtttl_write_wav_header(wavfd, samplerate, data_bytes);
  if (ret < 0)
    {
      goto out;
    }

  while ((nread = read(pcmfd, buffer, sizeof(buffer))) > 0)
    {
      ret = rtttl_write_all(wavfd, buffer, nread);
      if (ret < 0)
        {
          goto out;
        }
    }

  if (nread < 0)
    {
      ret = -errno;
    }

out:
  if (close(wavfd) < 0 && ret == OK)
    {
      ret = -errno;
    }

  close(pcmfd);
  return ret;
}

static void rtttl_audio_reset_buffer(FAR struct ap_buffer_s *apb)
{
  apb->nbytes = 0;
  apb->curbyte = 0;
  apb->nsamples = 0;
  apb->flags = 0;
}

static void rtttl_audio_cleanup(FAR struct rtttl_audio_stream_s *stream)
{
  struct audio_buf_desc_s bufdesc;
  uint16_t i;

  if (stream->fd >= 0)
    {
      if (stream->started && !stream->complete)
        {
#ifdef CONFIG_AUDIO_MULTI_SESSION
          ioctl(stream->fd, AUDIOIOC_STOP,
                (unsigned long)stream->session);
#else
          ioctl(stream->fd, AUDIOIOC_STOP, 0);
#endif
        }

      if (stream->registered)
        {
          ioctl(stream->fd, AUDIOIOC_UNREGISTERMQ,
                (unsigned long)stream->mq);
        }

      if (stream->buffers != NULL)
        {
          for (i = 0; i < stream->nbuffers; i++)
            {
              if (stream->buffers[i] != NULL)
                {
                  memset(&bufdesc, 0, sizeof(bufdesc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
                  bufdesc.session = stream->session;
#endif
                  bufdesc.u.buffer = stream->buffers[i];
                  ioctl(stream->fd, AUDIOIOC_FREEBUFFER,
                        (unsigned long)&bufdesc);
                }
            }
        }

      if (stream->reserved)
        {
#ifdef CONFIG_AUDIO_MULTI_SESSION
          ioctl(stream->fd, AUDIOIOC_RELEASE,
                (unsigned long)stream->session);
#else
          ioctl(stream->fd, AUDIOIOC_RELEASE, 0);
#endif
        }

      close(stream->fd);
      stream->fd = -1;
    }

  if (stream->mq != (mqd_t)-1)
    {
      mq_close(stream->mq);
      stream->mq = (mqd_t)-1;
    }

  if (stream->mqname[0] != '\0')
    {
      mq_unlink(stream->mqname);
    }

  free(stream->buffers);
  stream->buffers = NULL;
}

static int rtttl_mq_timedreceive(mqd_t mq, FAR struct audio_msg_s *msg,
                                 FAR unsigned int *prio)
{
  struct timespec abstime;
  int ret;

  ret = clock_gettime(CLOCK_REALTIME, &abstime);
  if (ret < 0)
    {
      return -errno;
    }

  abstime.tv_sec += RTTTL_AUDIO_WAIT_MS / 1000;
  abstime.tv_nsec += (RTTTL_AUDIO_WAIT_MS % 1000) * 1000000L;
  if (abstime.tv_nsec >= 1000000000L)
    {
      abstime.tv_sec++;
      abstime.tv_nsec -= 1000000000L;
    }

  ret = mq_timedreceive(mq, (FAR char *)msg, sizeof(*msg),
                        prio, &abstime);
  if (ret < 0)
    {
      return -errno;
    }

  return ret;
}

static int rtttl_audio_setup(FAR struct rtttl_audio_stream_s *stream,
                             FAR const char *devpath, uint32_t samplerate)
{
  struct audio_caps_desc_s capdesc;
  struct audio_buf_desc_s bufdesc;
  struct ap_buffer_info_s bufinfo;
  struct audio_caps_s caps;
  struct mq_attr attr;
  int ret;
  uint16_t i;

  stream->fd = open(devpath, O_RDWR);
  if (stream->fd < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: open %s failed: %d\n", devpath, errno);
      return ret;
    }

  memset(&caps, 0, sizeof(caps));
  caps.ac_len = sizeof(caps);
  caps.ac_type = AUDIO_TYPE_QUERY;
  caps.ac_subtype = AUDIO_TYPE_QUERY;
  if (ioctl(stream->fd, AUDIOIOC_GETCAPS, (unsigned long)&caps) !=
      caps.ac_len)
    {
      fprintf(stderr, "ERROR: %s is not an audio device\n", devpath);
      ret = -ENODEV;
      goto errout;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = ioctl(stream->fd, AUDIOIOC_RESERVE,
              (unsigned long)&stream->session);
#else
  ret = ioctl(stream->fd, AUDIOIOC_RESERVE, 0);
#endif
  if (ret < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: AUDIOIOC_RESERVE failed: %d\n", errno);
      goto errout;
    }

  stream->reserved = true;

  memset(&capdesc, 0, sizeof(capdesc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  capdesc.session = stream->session;
#endif
  capdesc.caps.ac_len = sizeof(struct audio_caps_s);
  capdesc.caps.ac_type = AUDIO_TYPE_OUTPUT;
  capdesc.caps.ac_subtype = AUDIO_FMT_PCM;
  capdesc.caps.ac_channels = RTTTL_CHANNELS;
  capdesc.caps.ac_chmap = 0;
  capdesc.caps.ac_controls.hw[0] = samplerate;
  capdesc.caps.ac_controls.b[3] = samplerate >> 16;
  capdesc.caps.ac_controls.b[2] = RTTTL_BITS_PER_SAMPLE;

  ret = ioctl(stream->fd, AUDIOIOC_CONFIGURE, (unsigned long)&capdesc);
  if (ret < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: AUDIOIOC_CONFIGURE failed: %d\n", errno);
      goto errout;
    }

  memset(&bufinfo, 0, sizeof(bufinfo));
  ret = ioctl(stream->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&bufinfo);
  if (ret != OK || bufinfo.nbuffers == 0 || bufinfo.buffer_size == 0)
    {
      bufinfo.nbuffers = CONFIG_AUDIO_NUM_BUFFERS;
      bufinfo.buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
    }

  stream->nbuffers = bufinfo.nbuffers;
  stream->buffer_size = bufinfo.buffer_size;
  stream->buffers = calloc(stream->nbuffers, sizeof(*stream->buffers));
  if (stream->buffers == NULL)
    {
      ret = -ENOMEM;
      goto errout;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = stream->nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  snprintf(stream->mqname, sizeof(stream->mqname), "/tmp/rtttl%lx",
           (unsigned long)((uintptr_t)stream));

  stream->mq = mq_open(stream->mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (stream->mq == (mqd_t)-1)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: mq_open failed: %d\n", errno);
      goto errout;
    }

  ret = ioctl(stream->fd, AUDIOIOC_REGISTERMQ, (unsigned long)stream->mq);
  if (ret < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: AUDIOIOC_REGISTERMQ failed: %d\n", errno);
      goto errout;
    }

  stream->registered = true;

  for (i = 0; i < stream->nbuffers; i++)
    {
      memset(&bufdesc, 0, sizeof(bufdesc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
      bufdesc.session = stream->session;
#endif
      bufdesc.numbytes = stream->buffer_size;
      bufdesc.u.pbuffer = &stream->buffers[i];

      ret = ioctl(stream->fd, AUDIOIOC_ALLOCBUFFER,
                  (unsigned long)&bufdesc);
      if (ret != sizeof(bufdesc))
        {
          ret = ret < 0 ? -errno : -EIO;
          fprintf(stderr, "ERROR: AUDIOIOC_ALLOCBUFFER failed: %d\n",
                  ret);
          goto errout;
        }

      rtttl_audio_reset_buffer(stream->buffers[i]);
    }

  return OK;

errout:
  rtttl_audio_cleanup(stream);
  return ret;
}

static int rtttl_audio_wait_dequeue(FAR struct rtttl_audio_stream_s *stream,
                                    FAR struct ap_buffer_s **apb)
{
  struct audio_msg_s msg;
  unsigned int prio;
  ssize_t nread;

  for (; ; )
    {
      nread = rtttl_mq_timedreceive(stream->mq, &msg, &prio);
      if (nread < 0)
        {
          if (nread == -EINTR)
            {
              continue;
            }

          return nread;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
          *apb = msg.u.ptr;
          rtttl_audio_reset_buffer(*apb);
          return OK;
        }

      if (msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          stream->complete = true;
          return -EPIPE;
        }
    }
}

static int rtttl_audio_get_buffer(FAR struct rtttl_audio_stream_s *stream,
                                  FAR struct ap_buffer_s **apb)
{
  if (stream->next_buffer < stream->nbuffers)
    {
      *apb = stream->buffers[stream->next_buffer++];
      rtttl_audio_reset_buffer(*apb);
      return OK;
    }

  return rtttl_audio_wait_dequeue(stream, apb);
}

static int rtttl_audio_start(FAR struct rtttl_audio_stream_s *stream)
{
  int ret;

  if (stream->started)
    {
      return OK;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = ioctl(stream->fd, AUDIOIOC_START, (unsigned long)stream->session);
#else
  ret = ioctl(stream->fd, AUDIOIOC_START, 0);
#endif
  if (ret < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: AUDIOIOC_START failed: %d\n", errno);
      return ret;
    }

  stream->started = true;
  return OK;
}

static int rtttl_audio_enqueue_current(FAR struct rtttl_audio_stream_s *stream,
                                       bool final)
{
  struct audio_buf_desc_s bufdesc;
  int ret;

  if (stream->current == NULL)
    {
      return -EINVAL;
    }

  stream->current->nbytes = stream->current_bytes;
  stream->current->curbyte = 0;
  stream->current->nsamples = stream->current_bytes /
                              (RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE);
  stream->current->flags = final ? AUDIO_APB_FINAL : 0;

  memset(&bufdesc, 0, sizeof(bufdesc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  bufdesc.session = stream->session;
#endif
  bufdesc.numbytes = stream->current->nbytes;
  bufdesc.u.buffer = stream->current;

  ret = ioctl(stream->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&bufdesc);
  if (ret < 0)
    {
      ret = -errno;
      fprintf(stderr, "ERROR: AUDIOIOC_ENQUEUEBUFFER failed: %d\n",
              errno);
      return ret;
    }

  stream->current = NULL;
  stream->current_bytes = 0;

  return rtttl_audio_start(stream);
}

static int rtttl_audio_write_samples(FAR struct rtttl_playback_s *player,
                                     FAR const int16_t *samples,
                                     uint32_t nframes)
{
  FAR struct rtttl_audio_stream_s *stream = player->arg;
  FAR const uint8_t *src = (FAR const uint8_t *)samples;
  size_t remaining = nframes * RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE;
  int ret;

  while (remaining > 0)
    {
      size_t space;
      size_t ncopy;

      if (stream->current == NULL)
        {
          ret = rtttl_audio_get_buffer(stream, &stream->current);
          if (ret < 0)
            {
              return ret;
            }

          stream->current_bytes = 0;
        }

      space = stream->current->nmaxbytes - stream->current_bytes;
      ncopy = MIN(space, remaining);
      memcpy(&stream->current->samp[stream->current_bytes], src, ncopy);
      stream->current_bytes += ncopy;
      src += ncopy;
      remaining -= ncopy;

      if (stream->current_bytes >= stream->current->nmaxbytes)
        {
          ret = rtttl_audio_enqueue_current(stream, false);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return OK;
}

static int rtttl_audio_finish(FAR struct rtttl_audio_stream_s *stream)
{
  struct audio_msg_s msg;
  unsigned int prio;
  ssize_t nread;
  int ret;

  if (stream->current == NULL)
    {
      ret = rtttl_audio_get_buffer(stream, &stream->current);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (stream->current_bytes == 0)
    {
      memset(stream->current->samp, 0,
             RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE);
      stream->current_bytes = RTTTL_CHANNELS * RTTTL_BYTES_PER_SAMPLE;
    }

  ret = rtttl_audio_enqueue_current(stream, true);
  if (ret < 0)
    {
      return ret;
    }

  while (!stream->complete)
    {
      nread = rtttl_mq_timedreceive(stream->mq, &msg, &prio);
      if (nread < 0)
        {
          if (nread == -EINTR)
            {
              continue;
            }

          return nread;
        }

      if (msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          stream->complete = true;
        }
    }

  return OK;
}

static int rtttl_play_audio(FAR const char *devpath, FAR const char *tune,
                            uint32_t samplerate, uint8_t volume)
{
  struct rtttl_audio_stream_s stream;
  struct rtttl_playback_s player;
  int ret;

  memset(&stream, 0, sizeof(stream));
  stream.fd = -1;
  stream.mq = (mqd_t)-1;

  ret = rtttl_audio_setup(&stream, devpath, samplerate);
  if (ret < 0)
    {
      return ret;
    }

  memset(&player, 0, sizeof(player));
  player.samplerate = samplerate;
  player.volume = volume;
  player.write_samples = rtttl_audio_write_samples;
  player.arg = &stream;

  g_rtttl_playback = &player;
  ret = rtttl_play(tune, rtttl_make_tone);
  g_rtttl_playback = NULL;

  if (ret < 0)
    {
      fprintf(stderr, "ERROR: RTTTL parse/play failed: %d\n", ret);
      goto out;
    }

  if (player.error < 0)
    {
      ret = player.error;
      goto out;
    }

  ret = rtttl_emit_frames(&player, 0,
                          ((uint64_t)RTTTL_TAIL_SILENCE_US * samplerate +
                          500000) / 1000000);
  if (ret < 0)
    {
      goto out;
    }

  ret = rtttl_audio_finish(&stream);

out:
  rtttl_audio_cleanup(&stream);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *devpath = CONFIG_SYSTEM_RTTTL_DEVPATH;
  FAR const char *selected;
  FAR const char *builtin;
  FAR char *joined = NULL;
  FAR const char *outfile = NULL;
  char tmppath[64];
  uint32_t data_bytes = 0;
  uint32_t samplerate = CONFIG_SYSTEM_RTTTL_SAMPLERATE;
  long parsed;
  int option;
  int ret;
  uint8_t volume = CONFIG_SYSTEM_RTTTL_VOLUME;

  while ((option = getopt(argc, argv, "d:hlo:r:v:")) != ERROR)
    {
      switch (option)
        {
          case 'd':
            devpath = optarg;
            break;

          case 'l':
            rtttl_list_builtins();
            return EXIT_SUCCESS;

          case 'o':
            outfile = optarg;
            break;

          case 'r':
            parsed = rtttl_argtol(optarg, 8000, 96000, "sample rate");
            if (parsed < 0)
              {
                return EXIT_FAILURE;
              }

            samplerate = parsed;
            break;

          case 'v':
            parsed = rtttl_argtol(optarg, 0, 100, "volume");
            if (parsed < 0)
              {
                return EXIT_FAILURE;
              }

            volume = parsed;
            break;

          case 'h':
          default:
            rtttl_usage(argv[0]);
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (optind >= argc)
    {
      rtttl_usage(argv[0]);
      return EXIT_FAILURE;
    }

  joined = rtttl_join_args(argc, argv, optind);
  if (joined == NULL)
    {
      fprintf(stderr, "ERROR: out of memory\n");
      return EXIT_FAILURE;
    }

  builtin = rtttl_builtin(joined);
  selected = builtin != NULL ? builtin : joined;

  if (outfile != NULL)
    {
      snprintf(tmppath, sizeof(tmppath), "%s/rtttl-%ld.pcm",
               CONFIG_SYSTEM_RTTTL_TMPDIR, (long)getpid());

      ret = rtttl_synth_pcm(tmppath, selected, samplerate, volume,
                            &data_bytes);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: PCM synthesis failed: %d\n", ret);
          free(joined);
          unlink(tmppath);

          return EXIT_FAILURE;
        }

      ret = rtttl_export_wav(outfile, tmppath, samplerate, data_bytes);
      unlink(tmppath);

      if (ret < 0)
        {
          fprintf(stderr, "ERROR: WAV export failed: %d\n", ret);
          free(joined);
          return EXIT_FAILURE;
        }
    }

#ifdef CONFIG_ARCH_BOARD_ADAFRUIT_FRUIT_JAM_RP2350
  (void)board_fruitjam_audio_codec_recover();
#endif

  ret = rtttl_play_audio(devpath, selected, samplerate, volume);
  free(joined);

  if (ret < 0)
    {
      fprintf(stderr, "ERROR: audio playback failed: %d\n", ret);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
