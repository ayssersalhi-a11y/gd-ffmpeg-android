 /**

ffmpeg_player.cpp

GDExtension - FFmpeg Video Player for Godot 4 (Android ARM64/ARM32)

يربط هذا الملف مكتبة FFmpeg بمحرك Godot 4 عبر GDExtension

يدعم صيغ MP4 و MKV على نظام Android

المتطلبات:

godot-cpp (تُبنى كـ static lib)


FFmpeg مُجمَّع لـ Android (libavcodec, libavformat, libavutil, libswscale, libswresample)


بعد البناء ستحصل على:

libgdffmpeg.so  <- هذا هو الملف الذي يُحمَّل في Godot
*/


#include "ffmpeg_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <cstring>

using namespace godot;

// ─── تسجيل الكلاس في Godot ───────────────────────────────────────────────────
void FFmpegPlayer::_bind_methods() {
// --- تحميل الفيديو ---
ClassDB::bind_method(D_METHOD("load_video", "path"),        &FFmpegPlayer::load_video);
ClassDB::bind_method(D_METHOD("play"),                      &FFmpegPlayer::play);
ClassDB::bind_method(D_METHOD("pause"),                     &FFmpegPlayer::pause);
ClassDB::bind_method(D_METHOD("stop"),                      &FFmpegPlayer::stop);
ClassDB::bind_method(D_METHOD("seek", "seconds"),           &FFmpegPlayer::seek);

// --- استعلام الحالة ---  
ClassDB::bind_method(D_METHOD("is_playing"),                &FFmpegPlayer::is_playing);  
ClassDB::bind_method(D_METHOD("get_duration"),              &FFmpegPlayer::get_duration);  
ClassDB::bind_method(D_METHOD("get_position"),              &FFmpegPlayer::get_position);  
ClassDB::bind_method(D_METHOD("get_video_width"),           &FFmpegPlayer::get_video_width);  
ClassDB::bind_method(D_METHOD("get_video_height"),          &FFmpegPlayer::get_video_height);  
ClassDB::bind_method(D_METHOD("get_fps"),                   &FFmpegPlayer::get_fps);  
ClassDB::bind_method(D_METHOD("get_current_frame_texture"), &FFmpegPlayer::get_current_frame_texture);  

// --- خصائص ---  


ClassDB::bind_method(D_METHOD("set_loop",   "enable"), &FFmpegPlayer::set_loop);  
ClassDB::bind_method(D_METHOD("get_loop"),             &FFmpegPlayer::get_loop);  
ClassDB::bind_method(D_METHOD("set_volume", "vol"),    &FFmpegPlayer::set_volume);  
ClassDB::bind_method(D_METHOD("get_volume"),           &FFmpegPlayer::get_volume);  


ADD_PROPERTY(PropertyInfo(Variant::BOOL,  "loop"),   "set_loop",   "get_loop");  
ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");  
// --- إشارات ---  
ADD_SIGNAL(MethodInfo("video_finished"));  
ADD_SIGNAL(MethodInfo("video_loaded",   PropertyInfo(Variant::BOOL, "success")));  
ADD_SIGNAL(MethodInfo("frame_updated",  PropertyInfo(Variant::OBJECT, "texture")));  
ADD_SIGNAL(MethodInfo("playback_error", PropertyInfo(Variant::STRING, "message")));

}

// ─── البنّاء والهادم ──────────────────────────────────────────────────────────
FFmpegPlayer::FFmpegPlayer()
: fmt_ctx(nullptr),
video_codec_ctx(nullptr),
audio_codec_ctx(nullptr),
sws_ctx(nullptr),
swr_ctx(nullptr),
video_stream_idx(-1),
audio_stream_idx(-1),
playing(false),
looping(false),
volume(1.0f),
duration(0.0),
position(0.0),
video_width(0),
video_height(0),
fps(0.0),
frame_buffer(nullptr)
{
set_process(true);
}

FFmpegPlayer::~FFmpegPlayer() {
_cleanup();
}

// ─── تحميل الفيديو ───────────────────────────────────────────────────────────
bool FFmpegPlayer::load_video(const String &path) {
_cleanup();

// تحويل مسار Godot إلى مسار نظام الملفات  
String real_path = ProjectSettings::get_singleton()->globalize_path(path);  
CharString cs = real_path.utf8();  
const char *file_path = cs.get_data();  

// فتح الملف  
int ret = avformat_open_input(&fmt_ctx, file_path, nullptr, nullptr);  
if (ret < 0) {  
    char err[256];  
    av_strerror(ret, err, sizeof(err));  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", String("فشل فتح الملف: ") + err);  
    return false;  
}  

// قراءة معلومات المقاطع  
if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", "تعذّر قراءة معلومات المقاطع");  
    _cleanup();  
    return false;  
}  

// إيجاد مقطع الفيديو والصوت  
video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);  
audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);  

if (video_stream_idx < 0) {  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", "لا يوجد مقطع فيديو في الملف");  
    _cleanup();  
    return false;  
}  

// فتح مفكك ترميز الفيديو  
AVStream *vstream = fmt_ctx->streams[video_stream_idx];  
const AVCodec *vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);  
if (!vcodec) {  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", "الترميز غير مدعوم");  
    _cleanup();  
    return false;  
}  

video_codec_ctx = avcodec_alloc_context3(vcodec);  
avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);  
if (avcodec_open2(video_codec_ctx, vcodec, nullptr) < 0) {  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", "تعذّر فتح مفكك ترميز الفيديو");  
    _cleanup();  
    return false;  
}  

// إعداد مقياس الصورة (تحويل إلى RGB24 لـ Godot)  
video_width  = video_codec_ctx->width;  
video_height = video_codec_ctx->height;  

AVRational fr = vstream->r_frame_rate;  
fps = (fr.den > 0) ? (double)fr.num / fr.den : 30.0;  

sws_ctx = sws_getContext(  
    video_width, video_height, video_codec_ctx->pix_fmt,  
    video_width, video_height, AV_PIX_FMT_RGB24,  
    SWS_BILINEAR, nullptr, nullptr, nullptr  
);  
if (!sws_ctx) {  
    emit_signal("video_loaded", false);  
    emit_signal("playback_error", "تعذّر إنشاء محوّل الصورة");  
    _cleanup();  
    return false;  
}  

// تخصيص مخزن الإطار  
int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);  
frame_buffer = (uint8_t *)av_malloc(buf_size);  

if (!frame_buffer) {  
    emit_signal("playback_error", "فشل تخصيص الذاكرة");  
    _cleanup();  
    return false;  
}  

// الصوت (اختياري)  
if (audio_stream_idx >= 0) {  
    AVStream *astream = fmt_ctx->streams[audio_stream_idx];  
    const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);  
    if (acodec) {  
        audio_codec_ctx = avcodec_alloc_context3(acodec);  
        avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);  
        avcodec_open2(audio_codec_ctx, acodec, nullptr);  
    }  
}  

// مدة الفيديو  
duration = (fmt_ctx->duration != AV_NOPTS_VALUE)  
           ? (double)fmt_ctx->duration / AV_TIME_BASE  
           : 0.0;  

  
position = 0.0;  

emit_signal("video_loaded", true);  
return true;

}

// ─── تشغيل / إيقاف ───────────────────────────────────────────────────────────
void FFmpegPlayer::play()  { playing = true;  }
void FFmpegPlayer::pause() { playing = false; }
void FFmpegPlayer::stop()  {
playing = false;
seek(0.0);
}

void FFmpegPlayer::seek(double seconds) {
if (!fmt_ctx) return;
int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
if (video_codec_ctx) avcodec_flush_buffers(video_codec_ctx);
if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);
position = seconds;
}

// ─── _process: يُستدعى كل إطار من Godot ─────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
if (!playing || !fmt_ctx) return;

position += delta;  
if (duration > 0.0 && position >= duration) {  
    if (looping) {  
        seek(0.0);  
    } else {  
        playing = false;  
        emit_signal("video_finished");  
        return;  
    }  
}  

_decode_next_frame();

}

// ─── فكّ ترميز الإطار التالي ─────────────────────────────────────────────────
void FFmpegPlayer::_decode_next_frame() {
AVPacket *packet = av_packet_alloc();
AVFrame  *frame  = av_frame_alloc();
AVFrame  *rgb    = av_frame_alloc();

bool got_frame = false;  

while (!got_frame && av_read_frame(fmt_ctx, packet) >= 0) {  
    if (packet->stream_index == video_stream_idx) {  
        if (avcodec_send_packet(video_codec_ctx, packet) == 0) {  
            while (avcodec_receive_frame(video_codec_ctx, frame) == 0) {  
                // تحويل الإطار إلى RGB24  
                av_image_fill_arrays(  
                    rgb->data, rgb->linesize,  
                    frame_buffer,  
                    AV_PIX_FMT_RGB24,  
                    video_width, video_height, 1  
                );  
                sws_scale(  
                    sws_ctx,  
                    frame->data, frame->linesize, 0, video_height,  
                    rgb->data,   rgb->linesize  
                );  

                // بناء صورة Godot  
                PackedByteArray pba;  
                int sz = video_width * video_height * 3;  
                pba.resize(sz);  
                memcpy(pba.ptrw(), frame_buffer, sz);  
                Ref<Image> img = Image::create_from_data(  
                    video_width, video_height, false,  
                    Image::FORMAT_RGB8, pba  
                );  

                 // إنشاء الـ Texture أول مرة  
                if (!current_texture.is_valid()) {  
                    current_texture = ImageTexture::create_from_image(img);  
                } else {  
                    current_texture->update(img);  
                }  

                emit_signal("frame_updated", current_texture);  

                got_frame = true;  
                break;  
            }  
        }  
    }  
    av_packet_unref(packet);  
    if (got_frame) break;  
}  

av_frame_free(&frame);  
av_frame_free(&rgb);  
av_packet_free(&packet);

}

// ─── Getters / Setters ────────────────────────────────────────────────────────
bool   FFmpegPlayer::is_playing()              const { return playing; }
double FFmpegPlayer::get_duration()            const { return duration; }
double FFmpegPlayer::get_position()            const { return position; }
int    FFmpegPlayer::get_video_width()         const { return video_width; }
int    FFmpegPlayer::get_video_height()        const { return video_height; }
double FFmpegPlayer::get_fps()                 const { return fps; }

Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const {
return current_texture;
}

void  FFmpegPlayer::set_loop(bool en)  { looping = en; }
bool  FFmpegPlayer::get_loop()   const { return looping; }
void  FFmpegPlayer::set_volume(float v){ volume = CLAMP(v, 0.0f, 2.0f); }
float FFmpegPlayer::get_volume() const { return volume; }

// ─── تنظيف الموارد ───────────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup() {
playing = false;

if (sws_ctx) {  
    sws_freeContext(sws_ctx);  
    sws_ctx = nullptr;  
}  

if (swr_ctx) {  
    swr_free(&swr_ctx);  
    swr_ctx = nullptr;  
}  

if (video_codec_ctx) {  
    avcodec_free_context(&video_codec_ctx);  
}  

if (audio_codec_ctx) {  
    avcodec_free_context(&audio_codec_ctx);  
}  

if (fmt_ctx) {  
    avformat_close_input(&fmt_ctx);  
}  

if (frame_buffer) {  
    av_free(frame_buffer);  
    frame_buffer = nullptr;  
}  

video_stream_idx = -1;  
audio_stream_idx = -1;  

duration = 0.0;  
position = 0.0;  

video_width = 0;  
video_height = 0;  
fps = 0.0;  

current_texture.unref(); // إصلاح خطأ Ref<ImageTexture>

}

// ─── نقطة دخول GDExtension ───────────────────────────────────────────────────
extern "C" {
GDExtensionBool GDE_EXPORT gdffmpeg_init(
GDExtensionInterfaceGetProcAddress p_get_proc_address,
const GDExtensionClassLibraryPtr  p_library,
GDExtensionInitialization         *r_initialization
) {
godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
init_obj.register_initializer([](godot::ModuleInitializationLevel level) {
if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
godot::ClassDB::register_class<FFmpegPlayer>();
}
});
init_obj.register_terminator([](godot::ModuleInitializationLevel level) {});
init_obj.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
return init_obj.init();
}
}
