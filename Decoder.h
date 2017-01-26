#ifndef DECODER_H
#define DECODER_H

#ifdef __cplusplus
extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
}
#endif

#include <string>
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>

#define BUFFERED_FRAMES_COUNT 120

class Decoder {
private:
  AVFormatContext   *pFormatCtx = NULL;
  int               videoStream;
  AVCodecContext    *pCodecCtxOrig = NULL;
  AVCodecContext    *pCodecCtx = NULL;
  AVCodec           *pCodec = NULL;
  AVFrame           *pFrame = NULL;
  AVFrame           *pFrameRGB = NULL;
  AVPacket          packet;
  int               frameFinished;
  int               numBytes;
  uint8_t           *buffer = NULL;
  struct SwsContext *sws_ctx = NULL;
  
  std::vector<std::vector<uint8_t>> buffered_frames;
  
  bool done;

  void SaveFrame(int iFrame);
  bool read_frame();
  
public:
  Decoder() = delete;
  Decoder(std::string filename = "CarRace.mp4");
  ~Decoder();
  
  void run();
  uint8_t* get_frame();
  int get_width();
  int get_height();
};

#endif