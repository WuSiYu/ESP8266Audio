/*
  AudioFileSourceICYStream
  Streaming Shoutcast ICY source

  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(ESP32) || defined(ESP8266)

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "AudioFileSourceICYStream.h"
#include <string.h>

AudioFileSourceICYStream::AudioFileSourceICYStream()
{
  pos = 0;
  reconnectTries = 0;
  saveURL[0] = 0;
}

AudioFileSourceICYStream::AudioFileSourceICYStream(const char *url)
{
  saveURL[0] = 0;
  reconnectTries = 0;
  open(url);
}

bool AudioFileSourceICYStream::open(const char *url)
{
  static const char *hdr[] = { "icy-metaint", "icy-name", "icy-genre", "icy-br", "Transfer-Encoding" };
  pos = 0;
  _setup_client(url);
  http.begin(*_client, url);
  http.addHeader("Icy-MetaData", "1");
  http.collectHeaders( hdr, 5 );
  http.setReuse(true);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    cb.st(STATUS_HTTPFAIL, PSTR("Can't open HTTP request"));
    return false;
  }
  if (http.hasHeader(hdr[0])) {
    String ret = http.header(hdr[0]);
    icyMetaInt = ret.toInt();
  } else {
    icyMetaInt = 0;
  }
  if (http.hasHeader(hdr[1])) {
    String ret = http.header(hdr[1]);
//    cb.md("SiteName", false, ret.c_str());
  }
  if (http.hasHeader(hdr[2])) {
    String ret = http.header(hdr[2]);
//    cb.md("Genre", false, ret.c_str());
  }
  if (http.hasHeader(hdr[3])) {
    String ret = http.header(hdr[3]);
//    cb.md("Bitrate", false, ret.c_str());
  }
  if (http.hasHeader(hdr[4])) {
    audioLogger->printf_P(PSTR("Transfer-Encoding: %s\n"), http.header("Transfer-Encoding").c_str());
    if(http.header("Transfer-Encoding") == String(PSTR("chunked"))) {

      next_chunk = getChunkSize();
      if(-1 == next_chunk)
      {
        return false;
      }
      is_chunked = true;
      readImpl = &AudioFileSourceHTTPStream::readChunked;
    } else {
      is_chunked = false;
      readImpl = &AudioFileSourceHTTPStream::readRegular;
    }

  } else {
    readImpl = &AudioFileSourceHTTPStream::readRegular;
    audioLogger->printf_P(PSTR("No Transfer-Encoding\n"));
    is_chunked = false;
  }

  icyByteCount = 0;
  mdSize = 0;
  readingIcy = false;
  memset(icyBuff, 0, sizeof(icyBuff));
  size = http.getSize();
  strncpy(saveURL, url, sizeof(saveURL));
  saveURL[sizeof(saveURL)-1] = 0;
  return true;
}

AudioFileSourceICYStream::~AudioFileSourceICYStream()
{
  http.end();
}

uint32_t AudioFileSourceICYStream::read(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHTTPStream::read passed NULL data\n"));
    return 0;
  }

  uint32_t readLen = 0;
  uint32_t toRead = len;
  unsigned long start = millis();
  while ((readLen < len) && (((signed long)(millis() - start)) < 1500)){
    if (!isOpen()) break;
    uint32_t ret = readWithIcy((void*)(((uint8_t*)data) + readLen), toRead, true);
    readLen += ret;
    toRead -= ret;
  }

  return readLen;
}

uint32_t AudioFileSourceICYStream::readNonBlock(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHTTPStream::readNonBlock passed NULL data\n"));
    return 0;
  }

  uint32_t readLen = readWithIcy(data, len, true);

  return readLen;
}

uint32_t AudioFileSourceICYStream::readWithIcy(void *data, uint32_t len, bool nonBlock){
  int read = 0;
  int ret = 0;

  if (readingIcy){
    if (mdSize>0){
      uint32_t readMdLen = std::min((int)(mdSize), (int)len);
      readMdLen = std::min((int)(readMdLen), (int)256); //the buffer is only 256 big
      ret = (this->*readImpl)(data, readMdLen, nonBlock);
      if (ret < 0) ret = 0;
      len -= ret;
      mdSize -= ret;

      if (ret > 0){
        char *readInto = icyBuff + 256;
        memcpy(readInto, data, ret);
        int end = 256 + ret; // The last byte of valid data
        char *header = (char *)memmem((void*)icyBuff, end, (void*)"StreamTitle=", 12);
        if (header) {
          char *p = header+12;
          if (*p=='\'' || *p== '"' ) {
            char closing[] = { *p, ';', '\0' };
            char *psz = strstr( p+1, closing );
            if( !psz ) psz = strchr( p+1, ';' );
            if( psz ) *psz = '\0';
            p++;
          } else {
            char *psz = strchr( p, ';' );
            if( psz ) *psz = '\0';
          }
          cb.md("StreamTitle", false, p);
        }
        memmove(icyBuff, icyBuff+ret, 256);

      }
      if (ret != readMdLen) return read; // Partial read
    }
    if (mdSize<=0){
      icyByteCount = 0;
      mdSize = 0;
      readingIcy = false;
    }
  }else{
    int beforeIcy;
    if (((int)(icyByteCount + len) >= (int)icyMetaInt) && (icyMetaInt > 0)) {
      beforeIcy = icyMetaInt - icyByteCount;
    }else{
      beforeIcy = len;
    }
    if (beforeIcy > 0) {
      ret = (this->*readImpl)(data, beforeIcy, nonBlock);
      if (ret < 0) ret = 0;
      read += ret;
      len -= ret;
      data = (void *)(reinterpret_cast<char*>(data) + ret);
      icyByteCount += ret;
      if (ret != beforeIcy) return read; // Partial read
    }
    if (len >= 1 ){  //try to read mdSize
      ret = (this->*readImpl)(data, 1, nonBlock);
      if (ret < 0) ret = 0;
      if (ret==1){
        mdSize = *((uint8_t*)data) * 16;
        readingIcy = true;
        memset(icyBuff, 0, 256); // Ensure no residual matches occur
      }
      len -= ret;
      data = (void *)(reinterpret_cast<char*>(data) + ret);
      if (ret != 1) return read; // Partial read
    }
  }

  return read;
}

#endif
