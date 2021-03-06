// Use the newer ALSA API
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>
extern "C"
{
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>
}
#include <stdio.h>
#include <stdint.h>



#define RES_NOT_MUL_OF_TWO 1
#define COULD_NOT_FIND_VID_CODEC 2
#define CONTEXT_CREATION_ERROR 3
#define COULD_NOT_OPEN_VID_CODEC 4
#define COULD_NOT_OPEN_FILE 5
#define COULD_NOT_ALLOCATE_FRAME 6
#define COULD_NOT_ALLOCATE_PIC_BUF 7
#define ERROR_ENCODING_FRAME_SEND 8
#define ERROR_ENCODING_FRAME_RECEIVE 9
#define COULD_NOT_FIND_AUD_CODEC 10
#define COULD_NOT_OPEN_AUD_CODEC 11
#define COULD_NOT_ALL_RESMPL_CONTEXT 12
#define FAILED_TO_INIT_RESMPL_CONTEXT 13
#define COULD_NOT_ALLOC_SAMPLES 14
#define COULD_NOT_CONVERT_AUD 15
#define ERROR_ENCODING_SAMPLES_SEND 16
#define ERROR_ENCODING_SAMPLES_RECEIVE 17

AVCodec *vid_codec, *aud_codec;
AVCodecContext *vid_codec_context = NULL;
AVCodecContext *aud_codec_context = NULL;
AVFormatContext *outctx;
AVStream *video_st, *audio_st;
AVFrame *vid_frame, *aud_frame;
struct SwsContext *sws_ctx;
struct SwrContext *swr_ctx = NULL;

int vid_frame_counter, aud_frame_counter;
int vid_width, vid_height;

// Audio converting
//uint8_t **src_samples_data;
int src_samples_linesize;
//int src_nb_samples;
int max_dst_nb_samples;

uint8_t **dst_samples_data;
int dst_samples_linesize;
int dst_samples_size;


int init_capturer(snd_pcm_t **handle, snd_pcm_uframes_t frames, char **buffer, int *size) {
    int err;
    snd_pcm_hw_params_t *params;
    int dir;
    char *device = (char*) "default";

    // Settings
    unsigned int sample_rate = 44100; // CD Quality
    unsigned int bits_per_sample = 16; // As we are using S16_LE forma
    unsigned int number_of_channels = 2; // stereo
    uint32_t duration = 5000; // duration to record in milliseconds

    printf("Capture device is %s\n", device);

    /* Open PCM device for recording (capture). */
    err = snd_pcm_open(handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err)
    {
        fprintf(stderr, "Unable to open PCM device: %s\n", snd_strerror(err));
        return err;
    }

    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca(&params);

    /* Fill it in with default values. */
    snd_pcm_hw_params_any(*handle, params);

    /* ### Set the desired hardware parameters. ### */

    /* Interleaved mode */
    err = snd_pcm_hw_params_set_access(*handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err)
    {
        fprintf(stderr, "Error setting interleaved mode: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Signed capture format (16-bit little-endian format) */
    if (bits_per_sample == 16) err = snd_pcm_hw_params_set_format(*handle, params, SND_PCM_FORMAT_S16_LE);
    else err = snd_pcm_hw_params_set_format(*handle, params, SND_PCM_FORMAT_U8);
    if (err)
    {
        fprintf(stderr, "Error setting format: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Setting number of channels */
    err = snd_pcm_hw_params_set_channels(*handle, params, number_of_channels);
    if (err)
    {
        fprintf(stderr, "Error setting channels: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Setting sampling rate */
    err = snd_pcm_hw_params_set_rate_near(*handle, params, &sample_rate, &dir);
    if (err)
    {
        fprintf(stderr, "Error setting sampling rate (%d): %s\n", sample_rate, snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Set period size*/
    err = snd_pcm_hw_params_set_period_size_near(*handle, params, &frames, &dir);
    if (err)
    {
        fprintf(stderr, "Error setting period size: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Write the parameters to the driver */
    err = snd_pcm_hw_params(*handle, params);
    if (err < 0)
    {
        fprintf(stderr, "Unable to set HW parameters: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Use a buffer large enough to hold one period (Find number of frames in one period) */
    err = snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    if (err)
    {
        fprintf(stderr, "Error retrieving period size: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        return err;
    }

    /* Allocating buffer in number of bytes per period (2 bytes/sample, 2 channels) */
    *size = frames * bits_per_sample / 8 * number_of_channels; 
    *buffer = (char *) malloc(*size);
    if (!buffer)
    {
        fprintf(stdout, "Buffer error.\n");
        snd_pcm_close(*handle);
        return -1;
    }

    unsigned int period_time;
    err = snd_pcm_hw_params_get_period_time(params, &period_time, &dir);
    if (err)
    {
        fprintf(stderr, "Error retrieving period time: %s\n", snd_strerror(err));
        snd_pcm_close(*handle);
        free(buffer);
        return err;
    }

    unsigned int bytes_per_frame = bits_per_sample / 8 * number_of_channels;
    uint32_t pcm_data_size = period_time * bytes_per_frame * duration / 1000;

    printf("Sample rate: %d Hz\n", sample_rate);
    printf("Channels: %d\n", number_of_channels);
    printf("Duration: %d millisecs\n", duration);
    printf("Number of frames: %d", frames);
}

void close_capturer(snd_pcm_t **handle, char** buffer) {
    snd_pcm_drain(*handle);
    snd_pcm_close(*handle);
    free(*buffer);
}

int init_resampler(struct SwrContext **swr_ctx, 
                    int *src_nb_samples, uint8_t ***src_data,
                    int *dst_nb_samples, uint8_t ***dst_data) {
    int64_t src_ch_layout = AV_CH_LAYOUT_STEREO;
    int src_rate = 44100;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_S16;
    
    int64_t dst_ch_layout = AV_CH_LAYOUT_STEREO;
    int dst_rate = 44100;
    enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_FLTP;

    int ret;

    int src_nb_channels = 0;
    int src_linesize;

    int max_dst_nb_samples;
    int dst_nb_channels = 0;
    int dst_linesize;

    /* create resampler context */
    *swr_ctx = swr_alloc();
    if (!*swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        return -1;
    }

    /* set options */
    av_opt_set_int(*swr_ctx, "in_channel_layout",    src_ch_layout, 0);
    av_opt_set_int(*swr_ctx, "in_sample_rate",       src_rate, 0);
    av_opt_set_sample_fmt(*swr_ctx, "in_sample_fmt", src_sample_fmt, 0);

    av_opt_set_int(*swr_ctx, "out_channel_layout",    dst_ch_layout, 0);
    av_opt_set_int(*swr_ctx, "out_sample_rate",       dst_rate, 0);
    av_opt_set_sample_fmt(*swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

    /* initialize the resampling context */
    if ((ret = swr_init(*swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        return -1;
    }

    /* allocate source and destination samples buffers 
        - For S16, 2 channels 16 bits per sample. So, each sample takes 4 bytes.
        - Linesize is the total bytes allocated in multiples of 128. 
        - Based on src_nb_samples, src_linesize is decided and space allocated in src_data
    */

    src_nb_channels = av_get_channel_layout_nb_channels(src_ch_layout);
    ret = av_samples_alloc_array_and_samples(src_data, &src_linesize, src_nb_channels,
                                             *src_nb_samples, src_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate source samples\n");
        return -1;
    }

    printf("Line size : %d\n", src_linesize);

    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    max_dst_nb_samples = *dst_nb_samples =
        av_rescale_rnd(*src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

    printf("Number of samples in dest: %d\n", *dst_nb_samples);

    /* buffer is going to be directly written to a rawaudio file, no alignment */
    dst_nb_channels = av_get_channel_layout_nb_channels(dst_ch_layout);
    ret = av_samples_alloc_array_and_samples(dst_data, &dst_linesize, dst_nb_channels,
                                             *dst_nb_samples, dst_sample_fmt, 0);
}

int initialize_encoding_audio(const char *filename)
{
    int ret;
    enum AVCodecID aud_codec_id = AV_CODEC_ID_AAC;
    enum AVSampleFormat sample_fmt = AV_SAMPLE_FMT_FLTP;

    avcodec_register_all();
    av_register_all();

    aud_codec = avcodec_find_encoder(aud_codec_id);
    avcodec_register(aud_codec);

    if (!aud_codec)
        return COULD_NOT_FIND_AUD_CODEC;

    aud_codec_context = avcodec_alloc_context3(aud_codec);
    if (!aud_codec_context)
        return CONTEXT_CREATION_ERROR;

    aud_codec_context->bit_rate = 192000;
    aud_codec_context->sample_rate = 44100;
    printf("Sample rate selected : %d\n", aud_codec_context->sample_rate);
    aud_codec_context->sample_fmt = sample_fmt;
    aud_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    aud_codec_context->channels = av_get_channel_layout_nb_channels(aud_codec_context->channel_layout);

    aud_codec_context->codec = aud_codec;
    aud_codec_context->codec_id = aud_codec_id;

    ret = avcodec_open2(aud_codec_context, aud_codec, NULL);

    if (ret < 0)
        return COULD_NOT_OPEN_AUD_CODEC;

    outctx = avformat_alloc_context();
    ret = avformat_alloc_output_context2(&outctx, NULL, "mp4", filename);

    outctx->audio_codec = aud_codec;
    outctx->audio_codec_id = aud_codec_id;

    audio_st = avformat_new_stream(outctx, aud_codec);

    audio_st->codecpar->bit_rate = aud_codec_context->bit_rate;
    audio_st->codecpar->sample_rate = aud_codec_context->sample_rate;
    audio_st->codecpar->channels = aud_codec_context->channels;
    audio_st->codecpar->channel_layout = aud_codec_context->channel_layout;
    audio_st->codecpar->codec_id = aud_codec_id;
    audio_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_st->codecpar->format = sample_fmt;
    audio_st->codecpar->frame_size = aud_codec_context->frame_size;
    audio_st->codecpar->block_align = aud_codec_context->block_align;
    audio_st->codecpar->initial_padding = aud_codec_context->initial_padding;

    // outctx->streams = new AVStream*[1];
    // outctx->streams[0] = audio_st;

    av_dump_format(outctx, 0, filename, 1);

    if (!(outctx->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&outctx->pb, filename, AVIO_FLAG_WRITE) < 0)
            return COULD_NOT_OPEN_FILE;
    }

    ret = avformat_write_header(outctx, NULL);

    aud_frame = av_frame_alloc();
    aud_frame->nb_samples = aud_codec_context->frame_size;
    aud_frame->format = aud_codec_context->sample_fmt;
    aud_frame->channel_layout = aud_codec_context->channel_layout;

    int buffer_size = av_samples_get_buffer_size(NULL, aud_codec_context->channels, aud_codec_context->frame_size,
        aud_codec_context->sample_fmt, 0);

    av_frame_get_buffer(aud_frame, buffer_size / aud_codec_context->channels);

    if (!aud_frame)
        return COULD_NOT_ALLOCATE_FRAME;

    aud_frame_counter = 0;

    return 0;
}

int encode_audio_samples(uint8_t **aud_samples)
{
    int ret;

    int buffer_size = av_samples_get_buffer_size(NULL, aud_codec_context->channels, aud_codec_context->frame_size,
        aud_codec_context->sample_fmt, 0);

    for (size_t i = 0; i < buffer_size / aud_codec_context->channels; i++)
    {
        aud_frame->data[0][i] = aud_samples[0][i];
        aud_frame->data[1][i] = aud_samples[1][i];
    }

    aud_frame->pts = aud_frame_counter++;

    ret = avcodec_send_frame(aud_codec_context, aud_frame);
    if (ret < 0)
        return ERROR_ENCODING_SAMPLES_SEND;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    fflush(stdout);

    while (1)
    {
        ret = avcodec_receive_packet(aud_codec_context, &pkt);
        if (!ret)
        {
            av_packet_rescale_ts(&pkt, aud_codec_context->time_base, audio_st->time_base);

            pkt.stream_index = audio_st->index;
            av_write_frame(outctx, &pkt);
            av_packet_unref(&pkt);
        }
        if (ret == AVERROR(EAGAIN))
            break;
        else if (ret < 0)
            return ERROR_ENCODING_SAMPLES_RECEIVE;
        else
            break;
    }

    return 0;
}

int finish_audio_encoding()
{
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    fflush(stdout);

    int ret = avcodec_send_frame(aud_codec_context, NULL);
    if (ret < 0)
        return ERROR_ENCODING_FRAME_SEND;

    while (1)
    {
        ret = avcodec_receive_packet(aud_codec_context, &pkt);
        if (!ret)
        {
            if (pkt.pts != AV_NOPTS_VALUE)
                pkt.pts = av_rescale_q(pkt.pts, aud_codec_context->time_base, audio_st->time_base);
            if (pkt.dts != AV_NOPTS_VALUE)
                pkt.dts = av_rescale_q(pkt.dts, aud_codec_context->time_base, audio_st->time_base);

            av_write_frame(outctx, &pkt);
            av_packet_unref(&pkt);
        }
        if (ret == -AVERROR(AVERROR_EOF))
            break;
        else if (ret < 0)
            return ERROR_ENCODING_FRAME_RECEIVE;
    }

    av_write_trailer(outctx);
}

void cleanup()
{
    if (vid_frame)
    {
        av_frame_free(&vid_frame);
    }
    if (aud_frame)
    {
        av_frame_free(&aud_frame);
    }
    if (outctx)
    {
        for (int i = 0; i < outctx->nb_streams; i++)
            av_freep(&outctx->streams[i]);

        avio_close(outctx->pb);
        av_free(outctx);
    }

    if (aud_codec_context)
    {
        avcodec_close(aud_codec_context);
        av_free(aud_codec_context);
    }

    if (vid_codec_context)
    {
        avcodec_close(vid_codec_context);
        av_free(vid_codec_context);
    }
}

int main(int argc, char *argv[]) {
    snd_pcm_t *handle;
    snd_pcm_uframes_t frames = 1024;
    int size;
    char *buffer;
    int err;
    int filedesc;
    int filedesc_resampled;
    int filedesc_resampled_c2; //channel 2

    /* Resampling related */
    struct SwrContext *swr_ctx;

    uint8_t **src_data = NULL;
    int src_nb_samples = frames;

    uint8_t **dst_data = NULL;
    int dst_nb_samples;

    int ret;
    /* Resampling related end */

    

    // Read file name
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s (output file name)\n", argv[0]);
        return -1;
    }
    char* fileName = argv[1];

    filedesc = open(fileName, O_WRONLY | O_CREAT, 0644);

    strcat(fileName, "_res");
    filedesc_resampled = open(fileName, O_WRONLY | O_CREAT, 0644);

    strcat(fileName, "2");
    filedesc_resampled_c2 = open(fileName, O_WRONLY | O_CREAT, 0644);

    strcat(fileName, ".aac");
    FILE *file_enc = fopen(fileName, "wb");

    err = init_capturer(&handle, frames, &buffer, &size);
    
    init_resampler(&swr_ctx, &src_nb_samples, &src_data, &dst_nb_samples, &dst_data);
    printf("Buffer size allocated : %d\n", size);

    initialize_encoding_audio("result.mp4");

    float_t** aud_samples;
    int src_samples_linesize;
    int src_channels = 2;

    ret = av_samples_alloc_array_and_samples((uint8_t***)&aud_samples, &src_samples_linesize, src_channels,
        src_nb_samples, AV_SAMPLE_FMT_FLTP, 0);

    for(int i = 0; i < 100; i++)
    {
        err = snd_pcm_readi(handle, buffer, frames);
        // if (err == -EPIPE) fprintf(stderr, "Overrun occurred: %d\n", err);
        // if (err) err = snd_pcm_recover(handle, err, 0);
        // Still an error, need to exit.
        if (err <= 0)
        {
            fprintf(stderr, "Error occured while recording: %s\n", snd_strerror(err));
            snd_pcm_close(handle);
            free(buffer);
            close(filedesc);
            return err;
        }
        write(filedesc, buffer, size);
        memcpy(src_data[0], buffer, size);
        ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const uint8_t **)src_data, src_nb_samples);
        // printf("Number of samples converted %d\n", ret);
        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            return -1;
        }

        write(filedesc_resampled, dst_data[0], size);
        write(filedesc_resampled_c2, dst_data[1], size);

        encode_audio_samples((uint8_t **)dst_data);

    }
    close(filedesc);
    close(filedesc_resampled);
    close_capturer(&handle, &buffer);

    if (src_data)
        av_freep(&src_data[0]);
    av_freep(&src_data);

    if (dst_data)
        av_freep(&dst_data[0]);
    av_freep(&dst_data);

    swr_free(&swr_ctx);

    finish_audio_encoding();
    cleanup();

}