#include "output_hss.h"
#include <mist/defines.h>
#include <mist/mp4.h>
#include <mist/mp4_ms.h>
#include <mist/mp4_generic.h>
#include <mist/base64.h>
#include <mist/http_parser.h>
#include <mist/stream.h>
#include <unistd.h>



///\todo Maybe move to util?
long long unsigned int binToInt(std::string & binary) {
  long long int result = 0;
  for (int i = 0; i < 8; i++) {
    result <<= 8;
    result += binary[i];
  }
  return result;
}

std::string intToBin(long long unsigned int number) {
  std::string result;
  result.resize(8);
  for (int i = 7; i >= 0; i--) {
    result[i] = number & 0xFF;
    number >>= 8;
  }
  return result;
}

std::string toUTF16(std::string original) {
  std::string result;
  result += (char)0xFF;
  result += (char)0xFE;
  for (std::string::iterator it = original.begin(); it != original.end(); it++) {
    result += (*it);
    result += (char)0x00;
  }
  return result;
}



namespace Mist {
  OutHSS::OutHSS(Socket::Connection & conn) : Output(conn) { 
    myConn.setHost(config->getString("ip"));
    streamName = config->getString("streamname");
  }

  OutHSS::~OutHSS() {}

  void OutHSS::init(Util::Config * cfg) {
    Output::init(cfg);
    capa["name"] = "HSS";
    capa["desc"] = "Enables HTTP protocol Microsoft-specific smooth streaming through silverlight (also known as HSS).";
    capa["deps"] = "HTTP";
    capa["url_rel"] = "/smooth/$.ism/Manifest";
    capa["url_prefix"] = "/smooth/$.ism/";
    capa["socket"] = "http_hss";
    capa["codecs"][0u][0u].append("H264");
    capa["codecs"][0u][1u].append("AAC");
    capa["methods"][0u]["handler"] = "http";
    capa["methods"][0u]["type"] = "html5/application/vnd.ms-ss";
    capa["methods"][0u]["priority"] = 9ll;
    capa["methods"][0u]["nolive"] = 1;
    capa["methods"][1u]["handler"] = "http";
    capa["methods"][1u]["type"] = "silverlight";
    capa["methods"][1u]["priority"] = 1ll;
    capa["methods"][1u]["nolive"] = 1;
    cfg->addBasicConnectorOptions(capa);
    config = cfg;
  }

  void OutHSS::sendNext() {
    if (currentPacket.getTime() >= playUntil) {
      stop();
      wantRequest = true;
      HTTP_S.Chunkify("", 0, myConn);
      HTTP_R.Clean();
      return;
    }
    char * dataPointer = 0;
    unsigned int len = 0;
    currentPacket.getString("data", dataPointer, len);
    HTTP_S.Chunkify(dataPointer, len, myConn);
  }

  void OutHSS::onFail(){
    HTTP_S.Clean(); //make sure no parts of old requests are left in any buffers
    HTTP_S.SetBody("Stream not found. Sorry, we tried.");
    HTTP_S.SendResponse("404", "Stream not found", myConn);
    Output::onFail();
  }
  
  int OutHSS::canSeekms(unsigned int ms) {
    //no tracks? Frame too new by definition.
    if (!myMeta.tracks.size()) {
      DEBUG_MSG(DLVL_DONTEVEN, "HSS Canseek to %d returns 1 because no tracks", ms);
      return 1;
    }
    //loop trough all selected tracks
    for (std::set<unsigned long>::iterator it = selectedTracks.begin(); it != selectedTracks.end(); it++) {
      //return "too late" if one track is past this point
      if (ms < myMeta.tracks[*it].firstms) {
        DEBUG_MSG(DLVL_DONTEVEN, "HSS Canseek to %d returns -1 because track %lu firstms == %llu", ms, *it, myMeta.tracks[*it].firstms);
        return -1;
      }
      //return "too early" if one track is not yet at this point
      if (ms > myMeta.tracks[*it].lastms) {
        DEBUG_MSG(DLVL_DONTEVEN, "HSS Canseek to %d returns 1 because track %lu lastms == %llu", ms, *it, myMeta.tracks[*it].lastms);
        return 1;
      }
    }
    return 0;
  }


  void OutHSS::sendHeader() {
    //We have a non-manifest request, parse it.
    std::string Quality = HTTP_R.url.substr(HTTP_R.url.find("TrackID=", 8) + 8);
    Quality = Quality.substr(0, Quality.find(")"));
    std::string parseString = HTTP_R.url.substr(HTTP_R.url.find(")/") + 2);
    parseString = parseString.substr(parseString.find("(") + 1);
    long long int seekTime = atoll(parseString.substr(0, parseString.find(")")).c_str()) / 10000;
    unsigned int tid = atoll(Quality.c_str());
    selectedTracks.clear();
    selectedTracks.insert(tid);
    if (myMeta.live) {
      updateMeta();
      int seekable = canSeekms(seekTime);
      if (seekable == 0){
        // iff the fragment in question is available, check if the next is available too
        for (std::deque<DTSC::Key>::iterator it = myMeta.tracks[tid].keys.begin(); it != myMeta.tracks[tid].keys.end(); it++){
          if (it->getTime() >= seekTime){
            if ((it + 1) == myMeta.tracks[tid].keys.end()){
              seekable = 1;
            }
            break;
          }
        }
      }
      if (seekable < 0){
        HTTP_S.Clean();
        HTTP_S.SetBody("The requested fragment is no longer kept in memory on the server and cannot be served.\n");
        myConn.SendNow(HTTP_S.BuildResponse("412", "Fragment out of range"));
        HTTP_R.Clean(); //clean for any possible next requests
        std::cout << "Fragment @ " << seekTime << "ms too old (" << myMeta.tracks[tid].firstms << " - " << myMeta.tracks[tid].lastms << " ms)" << std::endl;
        stop();
        wantRequest = true;
        return;
      }
      if (seekable > 0){
        HTTP_S.Clean();
        HTTP_S.SetBody("Proxy, re-request this in a second or two.\n");
        myConn.SendNow(HTTP_S.BuildResponse("208", "Ask again later"));
        HTTP_R.Clean(); //clean for any possible next requests
        std::cout << "Fragment @ " << seekTime << "ms not available yet (" << myMeta.tracks[tid].firstms << " - " << myMeta.tracks[tid].lastms << " ms)" << std::endl;
        stop();
        wantRequest = true;
        return;
      }
    }
    seek(seekTime);
    playUntil = (*(keyTimes[tid].upper_bound(seekTime)));
    myTrackStor = tid;
    myKeyStor = seekTime;
    keysToSend = 1;
    //Seek to the right place and send a play-once for a single fragment.
    std::stringstream sstream;

    int partOffset = 0;
    DTSC::Key keyObj;
    for (std::deque<DTSC::Key>::iterator it = myMeta.tracks[tid].keys.begin(); it != myMeta.tracks[tid].keys.end(); it++) {
      if (it->getTime() >= seekTime) {
        keyObj = (*it);
        std::deque<DTSC::Key>::iterator nextIt = it;
        nextIt++;
        if (nextIt == myMeta.tracks[tid].keys.end()) {
          if (myMeta.live) {
            HTTP_S.Clean();
            HTTP_S.SetBody("Proxy, re-request this in a second or two.\n");
            myConn.SendNow(HTTP_S.BuildResponse("208", "Ask again later"));
            HTTP_R.Clean(); //clean for any possible next requests
            std::cout << "Fragment after fragment @ " << seekTime << " not available yet" << std::endl;
          }
        }
        break;
      }
      partOffset += it->getParts();
    }
    if (HTTP_R.url == "/") {
      return; //Don't continue, but continue instead.
    }
    /*
    if (myMeta.live) {
      if (mstime == 0 && seekTime > 1){
        HTTP_S.Clean();
        HTTP_S.SetBody("The requested fragment is no longer kept in memory on the server and cannot be served.\n");
        myConn.SendNow(HTTP_S.BuildResponse("412", "Fragment out of range"));
        HTTP_R.Clean(); //clean for any possible next requests
        std::cout << "Fragment @ " << seekTime << " too old" << std::endl;
        continue;
      }
    }
    */

    ///\todo Select correct track (tid);

    //Wrap everything in mp4 boxes
    MP4::MFHD mfhd_box;
    mfhd_box.setSequenceNumber(((keyObj.getNumber() - 1) * 2) + tid);///\todo Urgent: Check this for multitrack... :P wtf... :P

    MP4::TFHD tfhd_box;
    tfhd_box.setFlags(MP4::tfhdSampleFlag);
    tfhd_box.setTrackID(tid);
    if (myMeta.tracks[tid].type == "video") {
      tfhd_box.setDefaultSampleFlags(0x00004001);
    } else {
      tfhd_box.setDefaultSampleFlags(0x00008002);
    }

    MP4::TRUN trun_box;
    trun_box.setDataOffset(42);///\todo Check if this is a placeholder, or an actually correct number
    unsigned int keySize = 0;
    if (myMeta.tracks[tid].type == "video") {
      trun_box.setFlags(MP4::trundataOffset | MP4::trunfirstSampleFlags | MP4::trunsampleDuration | MP4::trunsampleSize | MP4::trunsampleOffsets);
    } else {
      trun_box.setFlags(MP4::trundataOffset | MP4::trunsampleDuration | MP4::trunsampleSize);
    }
    trun_box.setFirstSampleFlags(0x00004002);
    for (int i = 0; i < keyObj.getParts(); i++) {
      MP4::trunSampleInformation trunSample;
      trunSample.sampleSize = myMeta.tracks[tid].parts[i + partOffset].getSize();
      keySize += myMeta.tracks[tid].parts[i + partOffset].getSize();
      trunSample.sampleDuration = myMeta.tracks[tid].parts[i + partOffset].getDuration() * 10000;
      if (myMeta.tracks[tid].type == "video") {
        trunSample.sampleOffset = myMeta.tracks[tid].parts[i + partOffset].getOffset() * 10000;
      }
      trun_box.setSampleInformation(trunSample, i);
    }

    MP4::SDTP sdtp_box;
    sdtp_box.setVersion(0);
    if (myMeta.tracks[tid].type == "video") {
      sdtp_box.setValue(36, 4);
      for (int i = 1; i < keyObj.getParts(); i++) {
        sdtp_box.setValue(20, 4 + i);
      }
    } else {
      sdtp_box.setValue(40, 4);
      for (int i = 1; i < keyObj.getParts(); i++) {
        sdtp_box.setValue(40, 4 + i);
      }
    }

    MP4::TRAF traf_box;
    traf_box.setContent(tfhd_box, 0);
    traf_box.setContent(trun_box, 1);
    traf_box.setContent(sdtp_box, 2);

    //If the stream is live, we want to have a fragref box if possible
    //////HEREHEREHERE
    if (myMeta.live) {
      MP4::UUID_TrackFragmentReference fragref_box;
      fragref_box.setVersion(1);
      fragref_box.setFragmentCount(0);
      int fragCount = 0;
      for (unsigned int i = 0; fragCount < 2 && i < myMeta.tracks[tid].keys.size() - 1; i++) {
        if (myMeta.tracks[tid].keys[i].getTime() > seekTime) {
          DEBUG_MSG(DLVL_HIGH, "Key %d added to fragRef box, time %ld > %lld", i, myMeta.tracks[tid].keys[i].getTime(), seekTime);
          fragref_box.setTime(fragCount, myMeta.tracks[tid].keys[i].getTime() * 10000);
          fragref_box.setDuration(fragCount, myMeta.tracks[tid].keys[i].getLength() * 10000);
          fragref_box.setFragmentCount(++fragCount);
        }
      }
      traf_box.setContent(fragref_box, 3);
    }

    MP4::MOOF moof_box;
    moof_box.setContent(mfhd_box, 0);
    moof_box.setContent(traf_box, 1);
    //Setting the correct offsets.
    moof_box.setContent(traf_box, 1);
    trun_box.setDataOffset(moof_box.boxedSize() + 8);
    traf_box.setContent(trun_box, 1);
    moof_box.setContent(traf_box, 1);

    HTTP_S.Clean();
    HTTP_S.SetHeader("Content-Type", "video/mp4");
    HTTP_S.StartResponse(HTTP_R, myConn);
    HTTP_S.Chunkify(moof_box.asBox(), moof_box.boxedSize(), myConn);
    int size = htonl(keySize + 8);
    HTTP_S.Chunkify((char *)&size, 4, myConn);
    HTTP_S.Chunkify("mdat", 4, myConn);
    sentHeader = true;
    HTTP_R.Clean();
  }


  ///\brief Builds an index file for HTTP Smooth streaming.
  ///\return The index file for HTTP Smooth Streaming.
  std::string OutHSS::smoothIndex(){
    updateMeta();
    std::stringstream Result;
    Result << "<?xml version=\"1.0\" encoding=\"utf-16\"?>\n";
    Result << "<SmoothStreamingMedia "
           "MajorVersion=\"2\" "
           "MinorVersion=\"0\" "
           "TimeScale=\"10000000\" ";
    std::deque<std::map<int, DTSC::Track>::iterator> audioIters;
    std::deque<std::map<int, DTSC::Track>::iterator> videoIters;
    long long int maxWidth = 0;
    long long int maxHeight = 0;
    long long int minWidth = 99999999;
    long long int minHeight = 99999999;
    for (std::map<int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      if (it->second.codec == "AAC") {
        audioIters.push_back(it);
      }
      if (it->second.codec == "H264") {
        videoIters.push_back(it);
        if (it->second.width > maxWidth) {
          maxWidth = it->second.width;
        }
        if (it->second.width < minWidth) {
          minWidth = it->second.width;
        }
        if (it->second.height > maxHeight) {
          maxHeight = it->second.height;
        }
        if (it->second.height < minHeight) {
          minHeight = it->second.height;
        }
      }
    }
    DEBUG_MSG(DLVL_DONTEVEN, "Buffer window here %lld", myMeta.bufferWindow);
    if (myMeta.vod) {
      Result << "Duration=\"" << (*videoIters.begin())->second.lastms << "0000\"";
    } else {
      Result << "Duration=\"0\" "
             "IsLive=\"TRUE\" "
             "LookAheadFragmentCount=\"2\" "
             "DVRWindowLength=\"" << myMeta.bufferWindow << "0000\" "
             "CanSeek=\"TRUE\" "
             "CanPause=\"TRUE\" ";
    }
    Result << ">\n";

    //Add audio entries
    if (audioIters.size()) {
      Result << "<StreamIndex "
             "Type=\"audio\" "
             "QualityLevels=\"" << audioIters.size() << "\" "
             "Name=\"audio\" "
             "Chunks=\"" << (*audioIters.begin())->second.keys.size() << "\" "
             "Url=\"Q({bitrate},{CustomAttributes})/A({start time})\">\n";
      int index = 0;
      for (std::deque<std::map<int, DTSC::Track>::iterator>::iterator it = audioIters.begin(); it != audioIters.end(); it++) {
        Result << "<QualityLevel "
               "Index=\"" << index << "\" "
               "Bitrate=\"" << (*it)->second.bps * 8 << "\" "
               "CodecPrivateData=\"" << std::hex;
        for (unsigned int i = 0; i < (*it)->second.init.size(); i++) {
          Result << std::setfill('0') << std::setw(2) << std::right << (int)(*it)->second.init[i];
        }
        Result << std::dec << "\" "
               "SamplingRate=\"" << (*it)->second.rate << "\" "
               "Channels=\"2\" "
               "BitsPerSample=\"16\" "
               "PacketSize=\"4\" "
               "AudioTag=\"255\" "
               "FourCC=\"AACL\" >\n";
        Result << "<CustomAttributes>\n"
               "<Attribute Name = \"TrackID\" Value = \"" << (*it)->first << "\" />"
               "</CustomAttributes>";
        Result << "</QualityLevel>\n";
        index++;
      }
      if ((*audioIters.begin())->second.keys.size()) {
        for (std::deque<DTSC::Key>::iterator it = (*audioIters.begin())->second.keys.begin(); it != (((*audioIters.begin())->second.keys.end()) - 1); it++) {
          Result << "<c ";
          if (it == (*audioIters.begin())->second.keys.begin()) {
            Result << "t=\"" << it->getTime() * 10000 << "\" ";
          }
          Result << "d=\"" << it->getLength() * 10000 << "\" />\n";
        }
      }
      Result << "</StreamIndex>\n";
    }
    //Add video entries
    if (videoIters.size()) {
      Result << "<StreamIndex "
             "Type=\"video\" "
             "QualityLevels=\"" << videoIters.size() << "\" "
             "Name=\"video\" "
             "Chunks=\"" << (*videoIters.begin())->second.keys.size() << "\" "
             "Url=\"Q({bitrate},{CustomAttributes})/V({start time})\" "
             "MaxWidth=\"" << maxWidth << "\" "
             "MaxHeight=\"" << maxHeight << "\" "
             "DisplayWidth=\"" << maxWidth << "\" "
             "DisplayHeight=\"" << maxHeight << "\">\n";
      int index = 0;
      for (std::deque<std::map<int, DTSC::Track>::iterator>::iterator it = videoIters.begin(); it != videoIters.end(); it++) {
        //Add video qualities
        Result << "<QualityLevel "
               "Index=\"" << index << "\" "
               "Bitrate=\"" << (*it)->second.bps * 8 << "\" "
               "CodecPrivateData=\"" << std::hex;
        MP4::AVCC avccbox;
        avccbox.setPayload((*it)->second.init);
        std::string tmpString = avccbox.asAnnexB();
        for (unsigned int i = 0; i < tmpString.size(); i++) {
          Result << std::setfill('0') << std::setw(2) << std::right << (int)tmpString[i];
        }
        Result << std::dec << "\" "
               "MaxWidth=\"" << (*it)->second.width << "\" "
               "MaxHeight=\"" << (*it)->second.height << "\" "
               "FourCC=\"AVC1\" >\n";
        Result << "<CustomAttributes>\n"
               "<Attribute Name = \"TrackID\" Value = \"" << (*it)->first << "\" />"
               "</CustomAttributes>";
        Result << "</QualityLevel>\n";
        index++;
      }
      if ((*videoIters.begin())->second.keys.size()) {
        for (std::deque<DTSC::Key>::iterator it = (*videoIters.begin())->second.keys.begin(); it != (((*videoIters.begin())->second.keys.end()) - 1); it++) {
          Result << "<c ";
          if (it == (*videoIters.begin())->second.keys.begin()) {
            Result << "t=\"" << it->getTime() * 10000 << "\" ";
          }
          Result << "d=\"" << it->getLength() * 10000 << "\" />\n";
        }
      }
      Result << "</StreamIndex>\n";
    }
    Result << "</SmoothStreamingMedia>\n";

#if DEBUG >= 8
    std::cerr << "Sending this manifest:" << std::endl << Result << std::endl;
#endif
    return toUTF16(Result.str());
  } //smoothIndex


  void OutHSS::onRequest() {
    sentHeader = false;
    while (HTTP_R.Read(myConn)) {
      initialize();
      if (HTTP_R.url.find("Manifest") != std::string::npos) {
        //Manifest, direct reply
        HTTP_S.Clean();
        HTTP_S.SetHeader("Content-Type", "text/xml");
        HTTP_S.SetHeader("Cache-Control", "no-cache");
        std::string manifest = smoothIndex();
        HTTP_S.SetBody(manifest);
        HTTP_S.SendResponse("200", "OK", myConn);
        HTTP_R.Clean();
      } else {
        parseData = true;
        wantRequest = false;
      }
    }
  }

  void OutHSS::initialize() {
    Output::initialize();
    for (std::map<int, DTSC::Track>::iterator it = myMeta.tracks.begin(); it != myMeta.tracks.end(); it++) {
      for (std::deque<DTSC::Key>::iterator it2 = it->second.keys.begin(); it2 != it->second.keys.end(); it2++) {
        keyTimes[it->first].insert(it2->getTime());
      }
    }
  }


}

