#include "Decoder.h"

Decoder::Decoder(std::string filename, 
    bool decodeVideo, bool decodeAudio, bool sync_local, int audio_stream) : 
  vframes(BUFFERED_FRAMES_COUNT),
  aframes(BUFFERED_FRAMES_COUNT),
  vtim(106), atim(107), //random values, are overwritten anyways
  done(false),
  _decodeVideo(decodeVideo),
  _decodeAudio(decodeAudio),
  _sync_local(sync_local),
  _seek_seconds(0),
  current_video_pts(0),
  currentAudioStream(audio_stream),
  _seeking(false)
{
  try {
    av_register_all();
  
    // Open video file
    if(avformat_open_input(&pFormatCtx, filename.c_str(), NULL, NULL)!=0) {
      throw 0;
    }
  
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0) {
      throw 1;
    }
  
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, filename.c_str(), 0);
  
    // Find the media streams
    videoStream = -1;
    
    for(int i=0; i<pFormatCtx->nb_streams; i++) {
      if (pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO) {
        videoStream = i;
      }
      if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        audioStreams.push_back(i);
      }
    }
    if(videoStream==-1) {
      throw 2;
    }

    //if no audio stream is present, put -1 as stream index
    if (audioStreams.size() == 0) { audioStreams.push_back(-1); }

    if (audioStreams[0] != -1) {
      if (currentAudioStream >= audioStreams.size()) { currentAudioStream = 0; }

      aCodecCtx = pFormatCtx->streams[audioStreams[currentAudioStream]]->codec;
      aFrame = av_frame_alloc();
	  
      aCodecCtx->codec = avcodec_find_decoder(aCodecCtx->codec_id);
      if (aCodecCtx->codec == NULL) {
        throw 7;
      }
      else if (avcodec_open2(aCodecCtx, aCodecCtx->codec, NULL) != 0) {
        throw 8;
      }

      atim.set_interval(av_q2d(pFormatCtx->streams[audioStreams[currentAudioStream]]->time_base)*1000);
    
      swr_ctx = swr_alloc_set_opts(NULL, aCodecCtx->channel_layout,
                  AV_SAMPLE_FMT_U8, aCodecCtx->sample_rate,
                  aCodecCtx->channel_layout, aCodecCtx->sample_fmt,
                  aCodecCtx->sample_rate, 0, NULL);
	    swr_init(swr_ctx);
    }
  
    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig=pFormatCtx->streams[videoStream]->codec;
    
    vtim.set_interval(av_q2d(pFormatCtx->streams[videoStream]->time_base)*1000);
    //std::cout << "video interval: " << av_q2d(pFormatCtx->streams[videoStream]->time_base)*1000 << '\n';

    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if(pCodec==NULL) {
      throw 3;
    }
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if(avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
      throw 4;
    }
  
    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0) {
      throw 5;
    }
    
    // Allocate video frame
    pFrame=av_frame_alloc();
  
    // Allocate an AVFrame structure
    pFrameRGB=av_frame_alloc();
    if(pFrameRGB==NULL) {
      throw 6;
    }
    
    // Determine required buffer size and allocate buffer
    numBytes=avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
  
    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
  
    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(	pCodecCtx->width, pCodecCtx->height,
                  pCodecCtx->pix_fmt, pCodecCtx->width, 
                  pCodecCtx->height, PIX_FMT_RGB24, SWS_BILINEAR,
                  NULL, NULL, NULL);

  } catch (int e) {
    std::vector<std::string> error_strings = 
    { "Error opening file\n",
      "Error reading stream information\n",
      "no videostream found\n",
      "unsupported codec\n",
      "Couldn't copy codec context\n",
      "Couldn't open codec\n",
      "Couldn't allocate pFrameRGB\n",
      "Couldn't find a proper audio decoder",
      "Couldn't open the context with the decoder"};
    std::cerr << "EXCEPTION:\n" << error_strings[e];    
  }
}

Decoder::~Decoder() {
  // Free the audio frame and close codec
  av_frame_free(&aFrame);
  avcodec_close(aCodecCtx);
	
  // Free the RGB image
	av_free(buffer);
	av_frame_free(&pFrameRGB);
	
	// Free the YUV frame
	av_frame_free(&pFrame);
  
	// Close the codecs
	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOrig);
	
	// Close the video file
	avformat_close_input(&pFormatCtx);

  std::cout << "Decoder is destroyed\n";
}
 
void Decoder::run() {
  vtim.set_start_now();
  atim.set_start(vtim.get_start());

  while(!done && read_frame()) { 
    if (_seek_seconds != 0) {
      seek();
      _seek_seconds = 0;
    }
  }
  stop();
}

void Decoder::stop() {
  //aframes.stop();
  //vframes.stop();
  done = true;
}
 
void Decoder::seek(const int & seconds) {
  _seek_seconds = seconds;
}

void Decoder::seek() {
  _seeking = true;
  
  int seek_target = current_video_pts + 
    (_seek_seconds / av_q2d(pFormatCtx->streams[videoStream]->time_base));


  seek_target = std::max(seek_target, 0);

  av_free_packet(&packet);
  av_freep(&packet);

  av_seek_frame(pFormatCtx, videoStream, seek_target, AVSEEK_FLAG_BACKWARD);
  
  
  if (audioStreams[0] != -1) {
    avcodec_flush_buffers(aCodecCtx);
    aframes.clear();
    atim.add_offset(-_seek_seconds);
  }
  
  avcodec_flush_buffers(pCodecCtx);
  vframes.clear();
  vtim.add_offset(-_seek_seconds);

  if (seek_target == 0) {
    vtim.set_start_now();
    atim.set_start(vtim.get_start());
  }

  _seeking = false;
}


/*void Decoder::next_audio_stream() {
  currentAudioStream = 
    currentAudioStream == audioStreams.size() - 1 ?
    0 : currentAudioStream + 1;
}

void Decoder::previous_audio_stream() {
  currentAudioStream = 
    currentAudioStream == 0 ?
    audioStreams.size() - 1 : currentAudioStream - 1;
}
*/

void Decoder::SaveFrame(int iFrame) {
  FILE *pFile;
  char szFilename[32];
  int  y;
  
  // Open file
  sprintf(szFilename, "frame%d.ppm", iFrame);
  pFile=fopen(szFilename, "wb");
  if(pFile==NULL)
    return;
  
  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", pCodecCtx->width, pCodecCtx->height);
  
  // Write pixel data
  for(y=0; y<pCodecCtx->height; y++)
    fwrite(pFrameRGB->data[0]+y*pFrameRGB->linesize[0], 1, pCodecCtx->width*3, pFile);
  
  // Close file
  fclose(pFile);
}
 
bool Decoder::read_frame() {
  if (av_read_frame(pFormatCtx, &packet) >= 0) {
    // Is this a packet from the video stream?
    if (packet.stream_index == videoStream && _decodeVideo) {
      // Decode video frame
      avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
      // Did we get a video frame?
      if (frameFinished) {
        // Convert the image from its native format to RGB
        sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
          pFrame->linesize, 0, pCodecCtx->height,
          pFrameRGB->data, pFrameRGB->linesize);

        while(!vframes.put(MediaFrame(buffer, numBytes, pFrame->pkt_pts))) {
          if (done) break;
        }
        current_video_pts = pFrame->pkt_pts;
      }
    }
    else if (packet.stream_index == audioStreams[currentAudioStream] && _decodeAudio) {
      while (packet.size > 0 || done) {
        int gotFrame = 0;
        int result = avcodec_decode_audio4(aCodecCtx, aFrame, &gotFrame, &packet);
        if (result >= 0 && gotFrame) {
          packet.size -= result;
          packet.data += result;

          const uint8_t **in = (const uint8_t **)aFrame->extended_data;
          uint8_t *out = NULL;
          int out_linesize;
          av_samples_alloc(&out,
            &out_linesize,
            2,
            44100,
            AV_SAMPLE_FMT_U8,
            0);

          int ret = swr_convert(swr_ctx,
            &out,
            44100,
            in,
            aFrame->nb_samples);

          while(!aframes.put(MediaFrame(out, ret*aCodecCtx->channels, aFrame->pkt_pts))) {
            if (done) break;
          }

          if (done) {
            break;
          }
          free(out);
        }
        else {
          packet.size = 0;
          packet.data = nullptr;
        }
      }
    }
    // Free the packet that was allocated by av_read_frame
    av_free_packet(&packet);
  } else {
    std::cout << "end of stream?\n";
    done = true;
  }
  return true;
}

MediaFrame Decoder::get_video_frame() {
  while(_seeking){}
  if (!_decodeVideo) {
    return MediaFrame();
  }

  while(!vframes.get(videoframe) || done) {
    if (done) { return MediaFrame(); }
  }

  if (_sync_local) {
    if (vtim.wait(videoframe.pts)<0) {
      //std::cout << "video too late, throwing away frame\n";
      return MediaFrame(); 
    }
  }
  
  return videoframe;
}

MediaFrame Decoder::get_audio_frame() {
  while(_seeking){}
  if (!_decodeAudio) {
    return MediaFrame();
  }
  
  while (!aframes.get(audioframe) || done) {
    if (done) { return MediaFrame(); }
  }
  
  //if too late, skip this audioframe (return an empty frame)
  if (_sync_local && atim.wait(audioframe.pts)<0) {
    //std::cout << "audio too late, throwing away frame\n";
    return MediaFrame();
  }

  return audioframe;
}

const int & Decoder::get_width() {
  return pCodecCtx->width;
}

const int & Decoder::get_height() {
  return pCodecCtx->height;
}

const double & Decoder::get_aspect_ratio() {
  return aspect_ratio;
}

int Decoder::get_sample_rate() {
  if (audioStreams[0] == -1) { return 44100; }
  return aCodecCtx->sample_rate;
}

int Decoder::get_channels() {
  if (audioStreams[0] == -1) { return 2; }
  return aCodecCtx->channels;
}
