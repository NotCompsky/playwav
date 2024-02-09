#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <compsky/macros/likely.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libavutil/frame.h>
}

AVCodecContext* codecCtx = nullptr;
AVCodec* codec = nullptr;
AVPacket packet;
AVFrame* frame = nullptr;
SwrContext* swrCtx = nullptr;
AVAudioFifo* audioFifo[2];

pa_simple* pulseAudioConnection = nullptr;

struct FormatCtx {
	AVFormatContext* val;
	FormatCtx()
	: val(nullptr)
	{}
	~FormatCtx(){
		if (likely(this->val != nullptr))
			avformat_close_input(&this->val);
	}
};

struct PulseAudioConnection {
	pa_simple* val;
	PulseAudioConnection(pa_sample_spec* const sample_spec,  int* const errorptr){
		this->val = pa_simple_new(
			nullptr,			   // Use the default server
			"FFMPEG Audio Player", // Application name
			PA_STREAM_PLAYBACK,	 // Stream direction
			nullptr,			   // Use the default device
			"Audio Playback",	  // Stream description
			sample_spec,		   // Sample format specification
			nullptr,			   // Default channel map
			nullptr,			   // Default buffering attributes
			errorptr				 // Error code
		);
	}
	~PulseAudioConnection(){
		if (likely(this->val != nullptr))
			pa_simple_free(this->val);
	}
};

bool initFFMPEG(){
	codecCtx = avcodec_alloc_context3(codec);
	if (!codecCtx){
		return true;
	}
	swrCtx = swr_alloc();
	
	audioFifo[0] = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, 1, 1);
	audioFifo[1] = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, 2, 1);
	if (!audioFifo[0]){
		return true;
	}
	if (!audioFifo[1]){
		return true;
	}
	
	frame = av_frame_alloc();
	if (!frame){
		return true;
	}
	
	{
		[[likely]]
		return false;
	}
}

void uninitFFMPEG(){
	avcodec_free_context(&codecCtx);
	swr_free(&swrCtx);
	av_frame_free(&frame);
	av_audio_fifo_free(audioFifo[0]);
	av_audio_fifo_free(audioFifo[1]);
}

bool init_pulseaudio(){
	return false;
}

bool uninit_pulseaudio(){
	return false;
}


extern "C"
int init_all(){
	bool failed = false;
	failed |= initFFMPEG();
	failed |= init_pulseaudio();
	return failed;
}
extern "C"
int uninit_all(){
	uninit_pulseaudio();
	uninitFFMPEG();
	return 0;
}


int openFile(const char* filePath,  AVFormatContext** formatCtx_ref){
	printf("Opening file %s\n", filePath);
	
	if (avformat_open_input(formatCtx_ref, filePath, nullptr, nullptr) != 0){
		return -1;
	}

	if (avformat_find_stream_info(formatCtx_ref[0], nullptr) < 0){
		avformat_close_input(formatCtx_ref);
		return -1;
	}

	int audioStreamIndex = -1;
	for (int i = 0; i < formatCtx_ref[0]->nb_streams; i++) {
		if (formatCtx_ref[0]->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioStreamIndex = i;
			break;
		}
	}

	if (audioStreamIndex == -1){
		avformat_close_input(formatCtx_ref);
		return -1;
	}

	codec = avcodec_find_decoder(formatCtx_ref[0]->streams[audioStreamIndex]->codecpar->codec_id);
	if (!codec){
		avformat_close_input(formatCtx_ref);
		return -1;
	}

	if (avcodec_parameters_to_context(codecCtx, formatCtx_ref[0]->streams[audioStreamIndex]->codecpar) < 0){
		avformat_close_input(formatCtx_ref);
		return -1;
	}

	if (avcodec_open2(codecCtx, codec, nullptr) < 0){
		avformat_close_input(formatCtx_ref);
		return -1;
	}
	
	codecCtx->channel_layout = av_get_default_channel_layout(codecCtx->channels);
		
	av_opt_set_int(swrCtx, "in_channel_layout", codecCtx->channel_layout, 0);
	av_opt_set_int(swrCtx, "out_channel_layout", codecCtx->channel_layout, 0);
	av_opt_set_int(swrCtx, "in_sample_rate", codecCtx->sample_rate, 0);
	av_opt_set_int(swrCtx, "out_sample_rate", codecCtx->sample_rate, 0);
	av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", codecCtx->sample_fmt, 0);
	av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	
	if (swr_init(swrCtx) < 0){
		avformat_close_input(formatCtx_ref);
		return -1;
	}
	
	if ((codecCtx->channels != 1) and (codecCtx->channels != 2)){
		printf("Too many channels: codecCtx->channels == %i\n", codecCtx->channels);
		avformat_close_input(formatCtx_ref);
		return -1;
	}
		
	return audioStreamIndex;
}

extern "C"
void playAudio(const char* const filePath){
	FormatCtx formatCtx;
	
	int audioStreamIndex = openFile(filePath, &formatCtx.val);
	if (unlikely(audioStreamIndex < 0)){
		printf("Failed to open file: %s\n", filePath);
		return;
	}
	
	pa_sample_spec sampleSpec;
	sampleSpec.format = PA_SAMPLE_S16LE;
	sampleSpec.rate = codecCtx->sample_rate;
	sampleSpec.channels = codecCtx->channels;
	int pacerrorintval;
	const PulseAudioConnection pulseAudioConnection(&sampleSpec, &pacerrorintval);
	
	if (unlikely(pulseAudioConnection.val == nullptr)){
		printf("Failed to connect to PulseAudio: %s\n", pa_strerror(pacerrorintval));
		return;
	}
	
	int pa_simple_write__prev_error = 0;
	
	while (av_read_frame(formatCtx.val, &packet) >= 0){
		if (packet.stream_index == audioStreamIndex){
			const int rc1 = avcodec_send_packet(codecCtx, &packet);
			if (unlikely(rc1 < 0)){
				printf("avcodec_send_packet error\n");
				continue;
			}
			const int receiveFrameResult = avcodec_receive_frame(codecCtx, frame);
			if (unlikely(receiveFrameResult < 0)){
				if ((receiveFrameResult == AVERROR(EAGAIN)) || (receiveFrameResult == AVERROR_EOF)){
					// EAGAIN: The decoder needs more data to produce a frame (not an error).
					// AVERROR_EOF: The decoder has been flushed, and no more frames are available.
					// Handle these cases as needed.
					continue;
				} else {
					printf("Error receiving frame from codec: %i\n", receiveFrameResult);
					break;
				}
			} else {
				[[likely]]
			
				if (unlikely(frame->nb_samples == 0))
					continue;
				/*const int convertedSamples = swr_convert(swrCtx, frame->data, frame->nb_samples, nullptr, 0);
				if (unlikely(convertedSamples < 0)){
					printf("Error converting input samples\n");
					break;
				}
				if (convertedSamples == 0){
					printf("swr_convert == 0\n");
					break;
				}*/
				AVAudioFifo* const audio_fifo = audioFifo[codecCtx->channels-1];
				av_audio_fifo_write(audio_fifo, (void**)frame->data, frame->nb_samples);
				
				//printf("swr_convert(swrCtx, frame->data, %u, nullptr, 0)\n", frame->nb_samples);
				//printf("convertedSamples == %i\n%u == av_audio_fifo_size(audio_fifo) >=? frame->nb_samples == %u\n", convertedSamples, av_audio_fifo_size(audio_fifo), frame->nb_samples);
				
				while (av_audio_fifo_size(audio_fifo) >= frame->nb_samples){
					av_audio_fifo_read(audio_fifo, (void**)frame->data, frame->nb_samples);
					// Play the audio data using PulseAudio
					int error = 0;
					pa_simple_write(pulseAudioConnection.val, frame->data[0], frame->nb_samples * sizeof(int16_t), &error);
					if (error){
						if (error != pa_simple_write__prev_error){
							if (pa_simple_write__prev_error != 0){
								printf("Prev error was repeated %i times\n", pa_simple_write__prev_error);
							}
							printf("Error %i writing to PulseAudio: %s\n", error, pa_strerror(error));
							pa_simple_write__prev_error = error;
						}
						break;
					}
				}
			}
		}
		av_packet_unref(&packet);
	}
}
