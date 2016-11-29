#include <cstdio>
#include <csignal>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stdint.h>
#include <limits.h>
#include <cstring>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <mutex>

#include "framelink.hpp"

// NDI SDK headers
#include "Processing.NDI.Lib.h"
#include "Processing.NDI.Find.h"

std::atomic_bool mustdie;
std::mutex mustdiemut;
std::condition_variable mustdiecond;
void diesignal_handler(int sig) {
  mustdie = true;
  mustdiecond.notify_all();
  printf("Stopping\n");
}

void await_death() {
  std::unique_lock<std::mutex> lock(mustdiemut);
  mustdiecond.wait(lock, []{ return mustdie == true; });
}

NDIlib_FourCC_type_e framelink_to_ndi_format(int x) {
  switch(x) {
    case frame_link::frame_video::video_format::VIDEO_FORMAT_UYVY:
      return NDIlib_FourCC_type_UYVY;
    case frame_link::frame_video::video_format::VIDEO_FORMAT_BGRA:
      return NDIlib_FourCC_type_BGRA;
    default:
      throw "Unknown frame link video format";
      break;
  };
}
frame_link::frame_video::video_format ndi_to_framelink_format(int x) {
  switch(x) {
    case NDIlib_FourCC_type_UYVY:
      return frame_link::frame_video::video_format::VIDEO_FORMAT_UYVY;
    case NDIlib_FourCC_type_BGRA:
      return frame_link::frame_video::video_format::VIDEO_FORMAT_BGRA;
    default:
      throw "Unknown NDIlib video format";
  };
}

struct stream {
  std::thread thread;
  uint64_t tag;
  std::string name;
  std::atomic_bool die;
};

void worker_loop(std::shared_ptr<stream> stream, std::shared_ptr<frame_link> link) {
  printf("Stream worker starting for %d / %s\n", stream->tag, stream->name.c_str());

  NDIlib_source_t src;
  NDIlib_recv_create_t settings;
  settings.source_to_connect_to.p_ndi_name = stream->name.c_str();
  settings.source_to_connect_to.p_ip_address = 0;
  settings.color_format = NDIlib_recv_color_format_e_BGRX_BGRA;
  settings.bandwidth = NDIlib_recv_bandwidth_highest;
  settings.allow_video_fields = false;

  auto receiver = NDIlib_recv_create2(&settings);
  NDIlib_video_frame_t videodata;
  NDIlib_audio_frame_t audiodata;
  while(!stream->die && !mustdie) {
    auto type = NDIlib_recv_capture(receiver, &videodata, &audiodata, NULL, 1000);
    switch(type) {
      case NDIlib_frame_type_video:
        {
          auto frame = link->pop(0);
          frame->kind = frame_link::kind::FRAME_KIND_VIDEO;
          frame->tag = stream->tag;
          frame->timestamp = videodata.timecode * 100.0L;
          auto payload = frame->payload_video();
          payload->height = videodata.yres;
          payload->width = videodata.xres;
          payload->format = ndi_to_framelink_format(videodata.FourCC);
          payload->fps_num = videodata.frame_rate_N;
          payload->fps_denom = videodata.frame_rate_D;
          payload->aspect_ratio = videodata.picture_aspect_ratio;
          payload->stride = videodata.line_stride_in_bytes;
          if(payload->size() < frame->max_payload_length()) {
            std::memcpy(payload->data(), videodata.p_data, payload->data_size());
            frame->payload_length = payload->size();
            link->push(1, frame);
          } else {
            link->push(0, frame);
          }
          NDIlib_recv_free_video(receiver, &videodata);
        }
        break;
      case NDIlib_frame_type_audio:
        {
          auto frame = link->pop(0);
          frame->kind = frame_link::kind::FRAME_KIND_AUDIO;
          frame->tag = stream->tag;
          frame->timestamp = audiodata.timecode * 100.0L;
          auto payload = frame->payload_audio();
          payload->channels = audiodata.no_channels;
          payload->samples = audiodata.no_samples;
          payload->samplerate = audiodata.sample_rate;
          payload->stride = audiodata.channel_stride_in_bytes;
          if(payload->size() < frame->max_payload_length()) {
            std::memcpy(payload->data(), audiodata.p_data, payload->data_size());
            frame->payload_length = payload->size();
            link->push(1, frame);
          } else {
            link->push(0, frame);
          }
          NDIlib_recv_free_audio(receiver, &audiodata);
        }
        break;
    }
  }

  NDIlib_recv_destroy(receiver);
  printf("Stream worker ending for %d / %s\n", stream->tag, stream->name.c_str());
}

struct stream_manager {
  stream_manager(std::shared_ptr<frame_link> link, int target) : link(link), target(target) {
  }

  ~stream_manager() {
    wait_all_dead();
  }

  void subscribe(uint64_t tag, const std::string& name) {
    std::lock_guard<std::mutex> lock(mut);
    if(streams.find(tag) == streams.end()) {
      std::shared_ptr<stream> s = streams.emplace(tag, std::make_shared<stream>()).first->second;
      s->tag = tag;
      s->name = name;
      s->die = false;
      s->thread = std::thread(&worker_loop, s, std::ref(link));
    }
  }

  void unsubscribe(uint64_t tag) {
    std::lock_guard<std::mutex> lock(mut);
    auto ite = streams.find(tag);
    if(ite == streams.end()) return;
    ite->second->die = true;
    ite->second->thread.join();
    streams.erase(ite);
  }

  void wait_all_dead() {
    std::lock_guard<std::mutex> lock(mut);
    for(auto c : streams) {
      c.second->thread.join();
    }
    streams.clear();
  }

private:

  std::mutex mut;
  int target;
  std::shared_ptr<frame_link> link;
  std::map<uint64_t, std::shared_ptr<stream>> streams;
};

// Command loop. Uses a very simple parsing mechanism.
// There are two commands:
//  - SUBSCRIBE tag name
//    Subscribes to an NDI stream by name. The tag will be used as key to
//    reference the stream for unsubscription. It will also be passed back
//    in the frame link. Data will be sent as soon as it becomes available.
//    This command is encoded as follows:
//     * ASCII 'S'
//     * 64-bit unsigned integer tag
//     * 32-bit unsigned integer name length(including zero terminator)
//     * The name itself, UTF-8 encoded
//  - UNSUBSCRIBE tag
//    Unsubscribes from an NDI stream. The stream will stop sending data.
//    Note that data may still be inflight. To avoid problems, tags should
//    not be reused. The command is encoded as:
//     * ASCII 'U'
//     * 64-bit unsigned integer tag
// The calls may not fail and return no results. From the clients perspective
// they are purely fire-and-forget.
void command_loop(std::shared_ptr<stream_manager> manager) {
  using namespace std;
  char namebuf[512] = { 0 };
  while(!mustdie && cin.good()) {
    char c;
    cin.read(&c, 1);
    bool sub = c == 'S';
    bool unsub = c == 'U';
    if(!sub && !unsub) continue;
    uint64_t tag;
    cin.read(reinterpret_cast<char*>(&tag), sizeof(tag));
    if(sub) {
      uint32_t namelen;
      cin.read(reinterpret_cast<char*>(&namelen), sizeof(namelen));
      cin.read(namebuf, namelen >= sizeof(namebuf) ? sizeof(namebuf)-1 : namelen);
      if(!cin.good()) break;
      manager->subscribe(tag, std::string(namebuf, namelen));
    } else {
      if(!cin.good()) continue;
      manager->unsubscribe(tag);
    }
  }
}

void print_usage(char *argv[]);

// Receiver mode
int main_receive(int argc, char *argv[]) {
  if (argc != 4) {
    print_usage(argv);
    return 1;
  }

  auto link = std::make_shared<frame_link>(argv[2]);
  int target = boost::lexical_cast<int>(argv[3]);

  auto manager = std::make_shared<stream_manager>(link, target);

  std::thread(&command_loop, manager).detach();
  await_death();
  manager->wait_all_dead();
  printf("All workers dead, exiting.");
  // We exit here with the stream manager still being kept alive by the
  // command loop thread. This also means that the frame link is also kept
  // alive. This is okay, because cleaning up a slave frame link is a no-op.
}

// Master mode, mainly for testing. Can be used to tie together a receiver
// and transmit.
int main_master(int argc, char *argv[]) {
  if (argc != 3) {
    print_usage(argv);
    return 1;
  }
  int stages = boost::lexical_cast<int>(argv[2]);
  frame_link link(stages);
  printf("%s\n", link.get_name().c_str());
  await_death();
}

// Finder mode. Produces changes in the availble NDI sources to stdout.
int main_find(int argc, char *argv[]) {
  std::set<std::string> known;
  NDIlib_find_create_t settings;
  settings.show_local_sources = true;
  settings.p_groups = NULL;

  auto find = NDIlib_find_create(&settings);
  if(!find) {
    return 1;
  }

  while(!mustdie) {
    int cnt;
    auto sources = NDIlib_find_get_sources(find, &cnt, 100);
    std::set<std::string> notseen = known;
    for(unsigned int i = 0; i < cnt; i++) {
      std::string name(sources[i].p_ndi_name);
      if(known.count(name) == 0) {
        // This is a new entry
        printf("NEW %s\n", name.c_str());
        known.insert(name);
      }
      notseen.erase(name);
    }
    // notseen now contains the sources that have disappered
    for(auto name : notseen) {
      printf("DEL %s\n", name.c_str());
      known.erase(name);
    }
  }

  return 0;
}

// Transmit mode. Transmits a frame link stream as an NDI stream.
int main_transmit(int argc, char *argv[]) {
  if (argc != 5) {
    print_usage(argv);
    return 1;
  }

  frame_link link(argv[2]);
  int source = boost::lexical_cast<int>(argv[3]);
  std::string name(argv[4]);
  if(name.empty()) {
    print_usage(argv);
    return 1;
  }

  NDIlib_send_create_t settings;
  settings.p_ndi_name = name.c_str();
  settings.p_groups = "";
  settings.clock_audio = true;
  settings.clock_video = true;

  auto sender = NDIlib_send_create(&settings);
  if(!sender) {
    printf("Could not create NDI sender");
    return 1;
  }

  while(!mustdie) {
    auto frame = link.pop_timed(source, std::chrono::milliseconds(100));
    if(!frame) continue;
    switch(frame->kind) {
      case frame_link::kind::FRAME_KIND_VIDEO:
        {
          auto vframe = frame->payload_video();

          NDIlib_video_frame_t ndiframe;
          ndiframe.xres = vframe->width;
          ndiframe.yres = vframe->height;
          ndiframe.FourCC = framelink_to_ndi_format(vframe->format);
          ndiframe.frame_rate_N = vframe->fps_num;
          ndiframe.frame_rate_D = vframe->fps_denom;
          ndiframe.picture_aspect_ratio = vframe->aspect_ratio;
          ndiframe.frame_format_type = NDIlib_frame_format_type_progressive;
          ndiframe.timecode = frame->timestamp / 100L;
          ndiframe.p_data = vframe->data();
          ndiframe.line_stride_in_bytes = vframe->stride;
          NDIlib_send_send_video(sender, &ndiframe);
        }
        break;
      case frame_link::kind::FRAME_KIND_AUDIO:
        {
          auto aframe = frame->payload_audio();

          NDIlib_audio_frame_t ndiframe;
          ndiframe.sample_rate = aframe->samplerate;
          ndiframe.no_channels = aframe->channels;
          ndiframe.no_samples = aframe->samples;
          ndiframe.timecode = frame->timestamp / 100L;
          ndiframe.p_data = aframe->data();
          ndiframe.channel_stride_in_bytes = aframe->stride;
          NDIlib_send_send_audio(sender, &ndiframe);
        }
        break;
    }
    link.push(0, frame);
  }

  return 0;
}

void print_usage(char *argv[]) {
  printf("NDI to Frame Link bridge\n");
  printf("More on NDI, Network Device Interface: http://www.newtek.com/ndi.html\n");
  printf("Usage:\n");
  printf(" %s find\n", argv[0]);
  printf(" %s master <num stages>\n", argv[0]);
  printf(" %s receive <frame link name> <target stage>\n", argv[0]);
  printf(" %s transmit <frame link name> <source stage> <name>\n", argv[0]);
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    print_usage(argv);
    return 1;
  }
  NDIlib_initialize();
  std::signal(SIGINT, &diesignal_handler);
  std::signal(SIGTERM, &diesignal_handler);
  setvbuf(stdout, NULL, _IOLBF, 200);

  if(strcmp(argv[1], "master") == 0) {
    return main_master(argc, argv);
  } else if(strcmp(argv[1], "find") == 0) {
    return main_find(argc, argv);
  } else if(strcmp(argv[1], "receive") == 0) {
    return main_receive(argc, argv);
  } else if(strcmp(argv[1], "transmit") == 0) {
    return main_transmit(argc, argv);
  } else {
    print_usage(argv);
    return 1;
  }
}
