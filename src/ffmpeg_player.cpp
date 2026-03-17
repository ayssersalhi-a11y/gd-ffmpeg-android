/**
 * ffmpeg_player.cpp
 * GDExtension - FFmpeg Video + Audio Player for Godot 4 (Android ARM64/ARM32)
 *
 * الصوت: AudioStreamGenerator + SwrContext (إعادة تعيين لـ stereo float)
 * الفيديو: sws_scale → RGB24 → ImageTexture
 */

#include "ffmpeg_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>

using namespace godot;

// ─── تسجيل الكلاس في Godot ───────────────────────────────────────────────────
void FFmpegPlayer::_bind_methods() {
    // ── دوال GDScript ─────────────────────────────────────────────
    ClassDB::bind_method(D_METHOD("load_video", "path"),        &FFmpegPlayer::load_video);
    ClassDB::bind_method(D_METHOD("play"),                      &FFmpegPlayer::play);
    ClassDB::bind_method(D_METHOD("pause"),                     &FFmpegPlayer::pause);
    ClassDB::bind_method(D_METHOD("stop"),                      &FFmpegPlayer::stop);
    ClassDB::bind_method(D_METHOD("seek", "seconds"),           &FFmpegPlayer::seek);

    ClassDB::bind_method(D_METHOD("is_playing"),                &FFmpegPlayer::is_playing);
    ClassDB::bind_method(D_METHOD("get_duration"),              &FFmpegPlayer::get_duration);
    ClassDB::bind_method(D_METHOD("get_position"),              &FFmpegPlayer::get_position);
    ClassDB::bind_method(D_METHOD("get_video_width"),           &FFmpegPlayer::get_video_width);
    ClassDB::bind_method(D_METHOD("get_video_height"),          &FFmpegPlayer::get_video_height);
    ClassDB::bind_method(D_METHOD("get_fps"),                   &FFmpegPlayer::get_fps);
    ClassDB::bind_method(D_METHOD("get_current_frame_texture"), &FFmpegPlayer::get_current_frame_texture);

    // ── دوال للتحكم بالحلقة والصوت ──────────────────────────────
    ClassDB::bind_method(D_METHOD("set_loop",   "enable"), &FFmpegPlayer::set_loop);
    ClassDB::bind_method(D_METHOD("get_loop"),             &FFmpegPlayer::get_loop);
    ClassDB::bind_method(D_METHOD("set_volume", "vol"),    &FFmpegPlayer::set_volume);
    ClassDB::bind_method(D_METHOD("get_volume"),           &FFmpegPlayer::get_volume);

    // ── خصائص يمكن تعديلها من Godot Inspector ───────────────
    ADD_PROPERTY(PropertyInfo(Variant::BOOL,  "loop"),   "set_loop",   "get_loop");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");

    // ── إشارات (Signals) ──────────────────────────────────────
    ADD_SIGNAL(MethodInfo("video_loaded",   PropertyInfo(Variant::BOOL,   "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",  PropertyInfo(Variant::OBJECT, "texture")));
    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("playback_error", PropertyInfo(Variant::STRING, "message")));
}

// ─── البنّاء والهادم ──────────────────────────────────────────────────────────
FFmpegPlayer::FFmpegPlayer()
    : fmt_ctx(nullptr),
      video_codec_ctx(nullptr),
      sws_ctx(nullptr),
      video_stream_idx(-1),
      video_width(0),
      video_height(0),
      fps(0.0),
      frame_buffer(nullptr),
      audio_codec_ctx(nullptr),
      swr_ctx(nullptr),
      audio_stream_idx(-1),
      audio_sample_rate(44100),
      audio_channels(2),
      audio_player(nullptr),
      playing(false),
      looping(false),
      volume(1.0f),
      duration(0.0),
      position(0.0)
{}

FFmpegPlayer::~FFmpegPlayer() {
    _cleanup();
}

// ─── _ready: تهيئة AudioStreamPlayer كـ child node ───────────────────────────
void FFmpegPlayer::_ready() {
    audio_player = memnew(AudioStreamPlayer);
    audio_player->set_name("_AudioPlayer");
    add_child(audio_player);
}

// ─── تحميل الفيديو ───────────────────────────────────────────────────────────

bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();
    
    if (path.is_empty()) {
        UtilityFunctions::printerr("[LOAD] Error: Path is empty!");
        return false;
    }

    UtilityFunctions::print("[LOAD] Attempting to open: ", path);

    if (!audio_player) {
        audio_player = memnew(AudioStreamPlayer);
        add_child(audio_player);
    }

    String real_path = ProjectSettings::get_singleton()->globalize_path(path);
    
    // تحويل المسار ليكون متوافقاً مع C-Style وتجنب الانهيار
    CharString utf8_path = real_path.utf8();
    const char *c_path = utf8_path.get_data();

    int ret = avformat_open_input(&fmt_ctx, c_path, nullptr, nullptr);
    if (ret < 0) {
        char errbuff[512];
        av_strerror(ret, errbuff, sizeof(errbuff));
        UtilityFunctions::printerr("[LOAD] FFmpeg Error: ", errbuff);
        _emit_video_loaded(false);
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        UtilityFunctions::printerr("[LOAD] Could not find stream info");
        _cleanup();
        return false;
    }

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx >= 0) {
        AVStream *vstream = fmt_ctx->streams[video_stream_idx];
        const AVCodec *vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
        if (vcodec) {
            video_codec_ctx = avcodec_alloc_context3(vcodec);
            avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);
            avcodec_open2(video_codec_ctx, vcodec, nullptr);
            video_width = video_codec_ctx->width;
            video_height = video_codec_ctx->height;
            fps = av_q2d(vstream->r_frame_rate);
            
            sws_ctx = sws_getContext(video_width, video_height, video_codec_ctx->pix_fmt,
                                     video_width, video_height, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
            _allocate_buffers();
        }
    }

    if (audio_stream_idx >= 0) {
        _setup_audio(fmt_ctx->streams[audio_stream_idx]);
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE) ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;
    _emit_video_loaded(true);
    UtilityFunctions::print("[LOAD] Success. Duration: ", duration);
    return true;
}



void FFmpegPlayer::_allocate_buffers() {
    // تنظيف البفر القديم فوراً لتحرير الـ RAM في Realme C33
    if (frame_buffer) {
        av_free(frame_buffer);
        frame_buffer = nullptr;
    }

    // حساب الحجم المطلوب للـ RGB24
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);
    
    if (!frame_buffer) {
        UtilityFunctions::printerr("[MEMORY] Critical: Failed to allocate frame buffer!");
        return;
    }

    // إعادة تهيئة التكستشر فقط إذا لزم الأمر
    if (current_texture.is_null()) {
        current_texture.instantiate();
        UtilityFunctions::print("[MEMORY] New ImageTexture instantiated.");
    }
    
    UtilityFunctions::print("[MEMORY] Buffers allocated for: ", video_width, "x", video_height);
}


void FFmpegPlayer::_clear_audio_buffers() {
    // تنظيف مكتبة إعادة العينات (FFmpeg Side)
    if (swr_ctx) {
        swr_close(swr_ctx);
        swr_init(swr_ctx);
    }
    
    // تنظيف مخزن Godot الصوتي (Godot Side)
    // بما أن دالة clear() غير مدعومة، نقوم بإيقاف وتشغيل المشغل لتفريغ الـ Buffer داخلياً
    if (audio_player) {
        bool was_playing = playing;
        audio_player->stop(); 
        if (was_playing) {
            audio_player->play();
        }
    }
}


// ─── تشغيل / إيقاف / توقف ─────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) return;
    playing = true;
    if (audio_player && audio_generator.is_valid()) {
        if (!audio_player->is_playing()) {
            audio_player->play();
        } else {
            audio_player->set_stream_paused(false);
        }
    }
}

void FFmpegPlayer::pause() {
    playing = false;
    if (audio_player) {
        audio_player->set_stream_paused(true);
    }
}

void FFmpegPlayer::stop() {
    playing = false;
    if (audio_player) {
        audio_player->stop();
    }
    seek(0.0);
}

void FFmpegPlayer::seek(double seconds) {
    if (!fmt_ctx) return;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);

    if (video_codec_ctx) avcodec_flush_buffers(video_codec_ctx);
    if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);

    // --- تنظيف الصوت فوراً عند القفز لمنع الضجيج ---
    _clear_audio_buffers();

    position = seconds;
}

// ─── _process: يُستدعى كل إطار من Godot ─────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!playing || !fmt_ctx) return;

    position += delta;
    
    // برنت كل ثانية لمراقبة النبض (Heartbeat)
    static double debug_timer = 0.0;
    debug_timer += delta;
    if (debug_timer >= 1.0) {
        UtilityFunctions::print("[HEARTBEAT] Position: ", position, " | Playing: ", playing);
        debug_timer = 0.0;
    }

    static double accumulator = 0.0;
    accumulator += delta;

    double frame_time = (fps > 0) ? 1.0 / fps : 0.04; 

    int frames_decoded = 0;
    while (accumulator >= frame_time) {
        _decode_next_frame();
        accumulator -= frame_time;
        frames_decoded++;
        if (frames_decoded > 5) break; 
    }

    if (duration > 0.0 && position >= duration) {
        if (looping) { seek(0.0); } 
        else { stop(); _emit_video_finished(); }
    }
}



bool FFmpegPlayer::_setup_audio(AVStream *astream) {
    if (!astream || !astream->codecpar) {
        UtilityFunctions::printerr("[AUDIO_SETUP] ERROR: Stream invalid");
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!codec) {
        UtilityFunctions::printerr("[AUDIO_SETUP] ERROR: Codec not found");
        return false;
    }

    audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);
    
    if (avcodec_open2(audio_codec_ctx, codec, nullptr) < 0) {
        UtilityFunctions::printerr("[AUDIO_SETUP] ERROR: Could not open codec");
        return false;
    }

    swr_ctx = swr_alloc();
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); // Stereo

    av_opt_set_chlayout(swr_ctx, "in_chlayout",  &audio_codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",    audio_codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",   audio_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",  audio_codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_ctx) < 0) {
        UtilityFunctions::printerr("[AUDIO_SETUP] ERROR: Swr init failed");
        return false;
    }

    audio_generator.instantiate();
    audio_generator->set_mix_rate(audio_codec_ctx->sample_rate);
    audio_generator->set_buffer_length(0.2); // زدنا البفر قليلاً للاستقرار

    if (audio_player) {
        audio_player->set_stream(audio_generator);
        UtilityFunctions::print("[AUDIO_SETUP] SUCCESS: Rate=", audio_codec_ctx->sample_rate);
    }

    audio_sample_rate = audio_codec_ctx->sample_rate;
    audio_channels = 2;
    return true;
}

// ── إشارات آمنة ─────────────────────────────────────────────
void FFmpegPlayer::_emit_video_loaded(bool success) {
    if (is_inside_tree()) {
        emit_signal("video_loaded", success);
    }
}

void FFmpegPlayer::_emit_video_finished() {
    if (is_inside_tree()) {
        emit_signal("video_finished");
    }
}

void FFmpegPlayer::_emit_frame_updated() {
    if (is_inside_tree()) {
        emit_signal("frame_updated", current_texture);
    }
}

void FFmpegPlayer::_emit_playback_error(const String &message) {
    if (is_inside_tree()) {
        emit_signal("playback_error", message);
    }
}
// ─── فكّ الترميز: فيديو + صوت ─────────────────────────────────────────

void FFmpegPlayer::_decode_next_frame() {
    if (!fmt_ctx) return;

    AVPacket *packet = av_packet_alloc();
    AVFrame *video_frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    
    bool got_video = false;
    int packets_read = 0;

    // سنحاول قراءة كمية كافية من الحزم
    while (packets_read < 100 && av_read_frame(fmt_ctx, packet) >= 0) {
        packets_read++;

        if (packet->stream_index == video_stream_idx && !got_video) {
            if (avcodec_send_packet(video_codec_ctx, packet) == 0) {
                if (avcodec_receive_frame(video_codec_ctx, video_frame) == 0) {
                    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, frame_buffer, AV_PIX_FMT_RGB24, video_width, video_height, 1);
                    sws_scale(sws_ctx, video_frame->data, video_frame->linesize, 0, video_height, rgb_frame->data, rgb_frame->linesize);
                    
                    PackedByteArray pba; pba.resize(video_width * video_height * 3);
                    memcpy(pba.ptrw(), frame_buffer, pba.size());
                    Ref<Image> img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, pba);
                    
                    if (current_texture.is_null() || current_texture->get_width() != video_width) 
                        current_texture = ImageTexture::create_from_image(img);
                    else 
                        current_texture->update(img);

                    _emit_frame_updated();
                    got_video = true;
                    // UtilityFunctions::print("[DEBUG] Video Frame Rendered");
                }
            }
        } 
        else if (packet->stream_index == audio_stream_idx) {
            if (avcodec_send_packet(audio_codec_ctx, packet) == 0) {
                AVFrame *audio_frame = av_frame_alloc();
                while (avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
                    _push_audio_samples(audio_frame);
                }
                av_frame_free(&audio_frame);
            }
        }
        av_packet_unref(packet);
        
        // نخرج فقط إذا حصلنا على فيديو وقرأنا بعض الصوت لضمان المزامنة
        if (got_video && packets_read > 20) break;
    }

    if (!got_video) {
        UtilityFunctions::print("[DEBUG] Warning: No video frame found in ", packets_read, " packets.");
    }

    av_frame_free(&video_frame); 
    av_frame_free(&rgb_frame); 
    av_packet_free(&packet);
}



// ─── دفع عينات الصوت إلى Godot AudioStreamGeneratorPlayback ─────────────────
void FFmpegPlayer::_push_audio_samples(AVFrame *frame) {
    if (!frame || !audio_player) return;

    Ref<AudioStreamGeneratorPlayback> playback = audio_player->get_stream_playback();
    if (playback.is_null()) return;

    int frames_available = playback->get_frames_available();
    if (frames_available <= 0) return; // البفر ممتلئ تماماً، ننتظر التكة القادمة

    int delay = swr_get_delay(swr_ctx, audio_sample_rate);
    int out_samples = av_rescale_rnd(delay + frame->nb_samples, audio_sample_rate, audio_sample_rate, AV_ROUND_UP);

    float **converted_data = nullptr;
    av_samples_alloc_array_and_samples((uint8_t ***)&converted_data, nullptr, 2, out_samples, AV_SAMPLE_FMT_FLT, 0);
    
    int converted_count = swr_convert(swr_ctx, (uint8_t **)converted_data, out_samples, (const uint8_t **)frame->data, frame->nb_samples);

    if (converted_count > 0) {
        // دفع فقط ما يتحمله البفر حالياً لتجنب التجمد
        int to_push = (converted_count < frames_available) ? converted_count : frames_available;
        
        PackedVector2Array audio_buffer;
        audio_buffer.resize(to_push);
        Vector2 *writer = audio_buffer.ptrw();

        for (int i = 0; i < to_push; ++i) {
            writer[i] = Vector2(converted_data[0][i], converted_data[1][i]);
        }

        playback->push_buffer(audio_buffer);
        
        if (to_push < converted_count) {
             // UtilityFunctions::print("[AUDIO] Partial push: ", to_push, "/", converted_count);
        }
    }

    if (converted_data) {
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
    }
}



// ─── Getters / Setters ────────────────────────────────────────────────────────
bool   FFmpegPlayer::is_playing()    const { return playing; }
double FFmpegPlayer::get_duration()  const { return duration; }
double FFmpegPlayer::get_position()  const { return position; }
int    FFmpegPlayer::get_video_width()  const { return video_width; }
int    FFmpegPlayer::get_video_height() const { return video_height; }
double FFmpegPlayer::get_fps() const {
    UtilityFunctions::print("[GETTER] FPS Requested: ", fps);
    return fps;
}

Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const {
    return current_texture;
}

void FFmpegPlayer::set_loop(bool en)  { looping = en; }
bool FFmpegPlayer::get_loop()   const { return looping; }

void FFmpegPlayer::set_volume(float v) {
    volume = v; // القيمة تأتي من 0.0 إلى 1.0 أو أكثر
    if (audio_player) {
        // Godot يستخدم الـ Decibels، الصفر يعني الصوت الأصلي، والسالب يعني خفض
        float db = (volume <= 0.0001f) ? -80.0f : 20.0f * log10(volume);
        audio_player->set_volume_db(db);
        UtilityFunctions::print("[AUDIO] Volume set to: ", volume, " (", db, " dB)");
    }
}

float FFmpegPlayer::get_volume() const { return volume; }

// ─── تنظيف الموارد ───────────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup() {
    if (video_codec_ctx) {
        avcodec_free_context(&video_codec_ctx);
        video_codec_ctx = nullptr;
    }

    if (audio_codec_ctx) {
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        fmt_ctx = nullptr;
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
    }

    if (frame_buffer) {
        av_free(frame_buffer);
        frame_buffer = nullptr;
    }
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
