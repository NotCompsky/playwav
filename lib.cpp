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
const AVCodec* codec = nullptr;
AVPacket packet;
AVFrame* frame = nullptr;
SwrContext* swrCtx = nullptr;

AVAudioFifo* audio_fifo;
AVSampleFormat audio_fifo_fmt = AV_SAMPLE_FMT_NONE;

void* interleaved_data_buf;
unsigned interleaved_data_buf_sz = 0;

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
	
	interleaved_data_buf = malloc(0);
	
	return failed;
}
extern "C"
int uninit_all(){
	uninit_pulseaudio();
	uninitFFMPEG();
	
	if (audio_fifo_fmt != AV_SAMPLE_FMT_NONE)
		av_audio_fifo_free(audio_fifo);
	if (interleaved_data_buf_sz != 0)
		free(interleaved_data_buf);
	
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
	
	const unsigned n_channels = codecCtx->ch_layout.nb_channels;
	codecCtx->channel_layout = av_get_default_channel_layout(n_channels);
	
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
	
	if ((n_channels != 1) and (n_channels != 2)){
		printf("Too many channels: n_channels == %i\n", n_channels);
		avformat_close_input(formatCtx_ref);
		return -1;
	}
		
	return audioStreamIndex;
}

template<bool is_planar, typename T>
void mainloop(AVAudioFifo* const audio_fifo,  pa_simple* const pulseAudioConnection,  AVFormatContext* const formatCtx_val,  const int audioStreamIndex,  const int64_t startFrame,  const int64_t endFrame,  int64_t currentFrame,  const float user_volume_scale_ratio){
	int pa_simple_write__prev_error = 0;
	while (av_read_frame(formatCtx_val, &packet) >= 0){
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
				
				const unsigned n_channels = frame->ch_layout.nb_channels; // previously ->channels
				const int n_frame_samples = frame->nb_samples;
				if (unlikely(n_frame_samples == 0))
					continue;
				const size_t data_buf_sz = n_channels*n_frame_samples * sizeof(T);
				if constexpr (is_planar){
					if (interleaved_data_buf_sz < data_buf_sz){
						[[unlikely]]
						free(interleaved_data_buf);
						interleaved_data_buf = malloc(data_buf_sz);
						interleaved_data_buf_sz = data_buf_sz;
					}
				}
				av_audio_fifo_write(audio_fifo, (void**)frame->data, n_frame_samples);
				
				while (av_audio_fifo_size(audio_fifo) >= n_frame_samples){
					av_audio_fifo_read(audio_fifo, (void**)frame->data, n_frame_samples);
					
					if (currentFrame >= startFrame){
						T* interleaved_data;
						if constexpr (is_planar){
							interleaved_data = reinterpret_cast<T*>(interleaved_data_buf);
						} else {
							interleaved_data = reinterpret_cast<T*>(frame->data[0]);
						}
						for (int i = 0;  i < n_frame_samples;  ++i){
							for (int j = 0;  j < n_channels;  ++j){
								const unsigned offset = i*n_channels;
								T value;
								if constexpr (is_planar){
									value = reinterpret_cast<T*>(frame->data[j])[i];
								} else {
									value = reinterpret_cast<T*>(frame->data[0] + offset*sizeof(T))[j];
								}
								interleaved_data[offset+j] = value * user_volume_scale_ratio;
							}
						}
						
						int error = 0;
						pa_simple_write(pulseAudioConnection, interleaved_data, data_buf_sz, &error);
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
					currentFrame += n_frame_samples;
					if (currentFrame >= endFrame)
						break;
				}
				if (currentFrame >= endFrame)
					break;
			}
		}
		av_packet_unref(&packet);
	}
}

extern "C"
void playAudio(const char* const filePath,  const float startTime,  const float endTime,  const float user_volume_scale_ratio){
	FormatCtx formatCtx;
	
	int audioStreamIndex = openFile(filePath, &formatCtx.val);
	if (unlikely(audioStreamIndex < 0)){
		printf("Failed to open file: %s\n", filePath);
		return;
	}
	
	int64_t startFrame = static_cast<int64_t>(startTime * codecCtx->sample_rate);
	int64_t endFrame   = static_cast<int64_t>(endTime * codecCtx->sample_rate);
	int64_t currentFrame = 0;
	
	if (endTime == 0.0)
		endFrame = UINT_MAX;
	
	const AVSampleFormat fmt = static_cast<AVSampleFormat>(formatCtx.val->streams[audioStreamIndex]->codecpar->format);
	const unsigned n_channels = codecCtx->ch_layout.nb_channels;
	
	pa_sample_spec sampleSpec;
	sampleSpec.rate = codecCtx->sample_rate;
	sampleSpec.channels = n_channels;
	switch(fmt){
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			sampleSpec.format = PA_SAMPLE_FLOAT32LE;
			break;
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			sampleSpec.format = PA_SAMPLE_S16LE;
			break;
		default:
			printf("Not implemented this format yet for sampleSpec.format = <stuff>\n");
			return;
	}
	int pacerrorintval;
	const PulseAudioConnection pulseAudioConnection(&sampleSpec, &pacerrorintval);
	
	if (unlikely(pulseAudioConnection.val == nullptr)){
		printf("Failed to connect to PulseAudio: %s\n", pa_strerror(pacerrorintval));
		return;
	}
	
	if (audio_fifo_fmt != fmt){
		if (audio_fifo_fmt != AV_SAMPLE_FMT_NONE)
			av_audio_fifo_free(audio_fifo);
		audio_fifo = av_audio_fifo_alloc(fmt, n_channels, 1);
		audio_fifo_fmt = fmt;
	}
	if (!audio_fifo){
		[[unlikely]]
		printf("Failed to allocate audio FIFO\n");
		return;
	}
	switch(fmt){
		case AV_SAMPLE_FMT_FLT:
			mainloop<false, float>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_DBL:
			mainloop<false, double>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_U8:
			mainloop<false, uint8_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S16:
			mainloop<false, int16_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S32:
			mainloop<false, int32_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S64:
			mainloop<false, int64_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		
		case AV_SAMPLE_FMT_FLTP:
			mainloop<true,  float>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_DBLP:
			mainloop<true,  double>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_U8P:
			mainloop<true,  uint8_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S16P:
			mainloop<true,  int16_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S32P:
			mainloop<true,  int32_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		case AV_SAMPLE_FMT_S64P:
			mainloop<true,  int64_t>(audio_fifo, pulseAudioConnection.val, formatCtx.val, audioStreamIndex, startFrame, endFrame, currentFrame, user_volume_scale_ratio);
			break;
		/*
		AV_SAMPLE_FMT_U8P,         ///< unsigned 8 bits, planar
		AV_SAMPLE_FMT_S16P,        ///< signed 16 bits, planar
		AV_SAMPLE_FMT_S32P,        ///< signed 32 bits, planar
		AV_SAMPLE_FMT_FLTP,        ///< float, planar
		AV_SAMPLE_FMT_DBLP,        ///< double, planar
		AV_SAMPLE_FMT_S64P
		*/
		default:
			printf("Unrecognised format: %u\n", fmt);
			return;
	}
}
