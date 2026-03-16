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

    ADD_PROPERTY(PropertyInfo(Variant::BOOL,  "loop"),   "set_loop",   "get_loop");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");

    ClassDB::bind_method(D_METHOD("set_loop",   "enable"), &FFmpegPlayer::set_loop);
    ClassDB::bind_method(D_METHOD("get_loop"),             &FFmpegPlayer::get_loop);
    ClassDB::bind_method(D_METHOD("set_volume", "vol"),    &FFmpegPlayer::set_volume);
    ClassDB::bind_method(D_METHOD("get_volume"),           &FFmpegPlayer::get_volume);

    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("video_loaded",   PropertyInfo(Variant::BOOL,   "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",  PropertyInfo(Variant::OBJECT, "texture")));
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

    // إعادة إنشاء AudioStreamPlayer إذا فُقد بعد _cleanup
    if (!audio_player) {
        audio_player = memnew(AudioStreamPlayer);
        audio_player->set_name("_AudioPlayer");
        add_child(audio_player);
    }

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
        emit_signal("playback_error", String("Failed to open file: ") + err);
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        emit_signal("video_loaded", false);
        emit_signal("playback_error", "Failed to read stream info");
        _cleanup();
        return false;
    }

    // إيجاد مقطعَي الفيديو والصوت
    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx < 0) {
        emit_signal("video_loaded", false);
        emit_signal("playback_error", "No video stream found");
        _cleanup();
        return false;
    }

    // ── فتح مفكك ترميز الفيديو ───────────────────────────────────────────────
    AVStream *vstream = fmt_ctx->streams[video_stream_idx];
    const AVCodec *vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
    if (!vcodec) {
        emit_signal("video_loaded", false);
        emit_signal("playback_error", "Unsupported video codec");
        _cleanup();
        return false;
    }

    video_codec_ctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);
    if (avcodec_open2(video_codec_ctx, vcodec, nullptr) < 0) {
        emit_signal("video_loaded", false);
        emit_signal("playback_error", "Failed to open video decoder");
        _cleanup();
        return false;
    }

    video_width  = video_codec_ctx->width;
    video_height = video_codec_ctx->height;
    AVRational fr = vstream->r_frame_rate;
    fps = (fr.den > 0) ? (double)fr.num / fr.den : 30.0;

    // محوّل الصورة RGB24
    sws_ctx = sws_getContext(
        video_width, video_height, video_codec_ctx->pix_fmt,
        video_width, video_height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        emit_signal("video_loaded", false);
        emit_signal("playback_error", "Failed to create image scaler");
        _cleanup();
        return false;
    }

    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);

    // ── إعداد الصوت ───────────────────────────────────────────────────────────
    if (audio_stream_idx >= 0) {
        AVStream *astream = fmt_ctx->streams[audio_stream_idx];
        _setup_audio(astream);
    } else {
        // لا يوجد صوت — أوقف أي تشغيل سابق
        audio_player->stop();
        audio_generator.unref();
    }

    // مدة الفيديو
    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE
               : 0.0;

    current_texture.instantiate();
    position = 0.0;

    emit_signal("video_loaded", true);
    return true;
}

// ─── إعداد مفكك ترميز الصوت + SWR + AudioStreamGenerator ─────────────────────
bool FFmpegPlayer::_setup_audio(AVStream *astream) {
    const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!acodec) return false;

    audio_codec_ctx = avcodec_alloc_context3(acodec);
    avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);
    if (avcodec_open2(audio_codec_ctx, acodec, nullptr) < 0) {
        avcodec_free_context(&audio_codec_ctx);
        return false;
    }

    // معدل العينات المُخرج: نستخدم معدل الملف الأصلي (أقصى جودة)
    audio_sample_rate = audio_codec_ctx->sample_rate;
    audio_channels    = 2; // دائماً stereo للـ AudioStreamGenerator

    // ── SwrContext: يحوّل أي صيغة صوت إلى Float Interleaved Stereo ──────────
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int swr_ret = swr_alloc_set_opts2(
        &swr_ctx,
        &out_layout,                    // output: stereo
        AV_SAMPLE_FMT_FLT,             // output format: float interleaved
        audio_sample_rate,              // output sample rate
        &audio_codec_ctx->ch_layout,   // input layout (من الملف)
        audio_codec_ctx->sample_fmt,   // input format (من الملف)
        audio_codec_ctx->sample_rate,  // input sample rate
        0, nullptr
    );

    if (swr_ret < 0 || swr_init(swr_ctx) < 0) {
        swr_free(&swr_ctx);
        avcodec_free_context(&audio_codec_ctx);
        return false;
    }

    // ── AudioStreamGenerator ─────────────────────────────────────────────────
    audio_generator.instantiate();
    audio_generator->set_mix_rate((float)audio_sample_rate);
    // حجم البفر = 0.1 ثانية (يمنع التقطّع دون تأخير ملحوظ)
    audio_generator->set_buffer_length(0.1f);

    audio_player->set_stream(audio_generator);
    audio_player->set_volume_db(UtilityFunctions::linear_to_db(volume));
    // لا نشغّل AudioStreamPlayer الآن — ينتظر play()

    return true;
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

    // النسخة القديمة أعطت دقة أكبر: تنظيف SWR
    if (swr_ctx) swr_drop_output(swr_ctx, swr_get_delay(swr_ctx, audio_sample_rate));

    position = seconds;
}
// ─── _process: يُستدعى كل إطار من Godot ─────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!playing || !fmt_ctx) return;

    position += delta;
    if (duration > 0.0 && position >= duration) {
        if (looping) {
            seek(0.0);
            if (audio_player && audio_generator.is_valid()) {
                audio_player->stop();
                audio_player->play();
            }
        } else {
            playing = false;
            if (audio_player) audio_player->stop();
            emit_signal("video_finished");
            return;
        }
    }

    _decode_next_frame();
}

// ─── فكّ الترميز: فيديو + صوت معاً ─────────────────────────────────────────
void FFmpegPlayer::_decode_next_frame() {
    AVPacket *packet     = av_packet_alloc();
    AVFrame  *frame      = av_frame_alloc();
    AVFrame  *rgb        = av_frame_alloc();
    AVFrame  *audio_frame = av_frame_alloc();

    bool got_video   = false;
    int  max_packets = 200; // حد أقصى لمنع التعليق

    while (!got_video && max_packets-- > 0 && av_read_frame(fmt_ctx, packet) >= 0) {

        // ── فيديو ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_decode_next_frame() {
    AVPacket *packet      = av_packet_alloc();
    AVFrame  *video_frame = av_frame_alloc();
    AVFrame  *rgb_frame   = av_frame_alloc();
    AVFrame  *audio_frame = av_frame_alloc();

    bool got_video = false;
    int  max_packets = 200; // من النسخة القديمة لمنع التعليق

    while (!got_video && max_packets-- > 0 && av_read_frame(fmt_ctx, packet) >= 0) {

        // فيديو
        if (packet->stream_index == video_stream_idx && video_codec_ctx) {
            if (avcodec_send_packet(video_codec_ctx, packet) == 0) {
                while (avcodec_receive_frame(video_codec_ctx, video_frame) == 0) {
                    av_image_fill_arrays(
                        rgb_frame->data, rgb_frame->linesize,
                        frame_buffer,
                        AV_PIX_FMT_RGB24,
                        video_width, video_height, 1
                    );

                    sws_scale(
                        sws_ctx,
                        video_frame->data, video_frame->linesize,
                        0, video_height,
                        rgb_frame->data, rgb_frame->linesize
                    );

                    PackedByteArray pba;
                    int sz = video_width * video_height * 3;
                    pba.resize(sz);
                    memcpy(pba.ptrw(), frame_buffer, sz);

                    Ref<Image> img = Image::create_from_data(
                        video_width, video_height, false,
                        Image::FORMAT_RGB8, pba
                    );

                    if (!current_texture.is_valid()) {
                        current_texture = ImageTexture::create_from_image(img);
                    } else {
                        current_texture->update(img);
                    }

                    emit_signal("frame_updated", current_texture);
                    av_frame_unref(video_frame);
                    got_video = true;
                    break;
                }
            }
        }

        // صوت
        if (packet->stream_index == audio_stream_idx && audio_codec_ctx && swr_ctx) {
            if (avcodec_send_packet(audio_codec_ctx, packet) == 0) {
                while (avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
                    _push_audio_samples(audio_frame);
                    av_frame_unref(audio_frame);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&audio_frame);
    av_frame_free(&video_frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
}

// ─── دفع عينات الصوت إلى Godot AudioStreamGeneratorPlayback ─────────────────
void FFmpegPlayer::_push_audio_samples(AVFrame *frame) {
    if (!audio_player || !audio_player->is_playing()) return;

    Ref<AudioStreamGeneratorPlayback> playback =
        audio_player->get_stream_playback();
    if (playback.is_null()) return;

    // احسب عدد العينات المُخرجة بعد إعادة التعيين
    int64_t delay      = swr_get_delay(swr_ctx, audio_codec_ctx->sample_rate);
    int out_count      = (int)av_rescale_rnd(
        delay + frame->nb_samples,
        audio_sample_rate,
        audio_codec_ctx->sample_rate,
        AV_ROUND_UP
    );

    // خصّص مخزن مؤقت لعينات Float Interleaved
    uint8_t *out_buf = nullptr;
    int out_linesize  = 0;
    if (av_samples_alloc(&out_buf, &out_linesize, 2, out_count, AV_SAMPLE_FMT_FLT, 0) < 0) {
        return;
    }

    int converted = swr_convert(
        swr_ctx,
        &out_buf,                        out_count,
        (const uint8_t **)frame->data,   frame->nb_samples
    );

    if (converted > 0) {
        const float *samples = reinterpret_cast<const float *>(out_buf);

        // تحقّق من مساحة البفر المتاحة
        int frames_available = playback->get_frames_available();
        int to_push          = MIN(converted, frames_available);

        PackedVector2Array audio_frames;
        audio_frames.resize(to_push);
        Vector2 *ptr = audio_frames.ptrw();

        for (int i = 0; i < to_push; i++) {
            float l = samples[i * 2]     * volume;
            float r = samples[i * 2 + 1] * volume;
            // حدّ القيم لمنع التشويه
            ptr[i] = Vector2(CLAMP(l, -1.0f, 1.0f), CLAMP(r, -1.0f, 1.0f));
        }

        playback->push_buffer(audio_frames);
    }

    av_freep(&out_buf);
}

// ─── Getters / Setters ────────────────────────────────────────────────────────
bool   FFmpegPlayer::is_playing()    const { return playing; }
double FFmpegPlayer::get_duration()  const { return duration; }
double FFmpegPlayer::get_position()  const { return position; }
int    FFmpegPlayer::get_video_width()  const { return video_width; }
int    FFmpegPlayer::get_video_height() const { return video_height; }
double FFmpegPlayer::get_fps()       const { return fps; }

Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const {
    return current_texture;
}

void FFmpegPlayer::set_loop(bool en)  { looping = en; }
bool FFmpegPlayer::get_loop()   const { return looping; }

void FFmpegPlayer::set_volume(float v) {
    volume = CLAMP(v, 0.0f, 2.0f);
    if (audio_player) {
        // تحويل linear إلى dB: 0.0→-80 dB، 1.0→0 dB، 2.0→~6 dB
        float db = (volume <= 0.0f) ? -80.0f : UtilityFunctions::linear_to_db(volume);
        audio_player->set_volume_db(db);
    }
}
float FFmpegPlayer::get_volume() const { return volume; }

// ─── تنظيف الموارد ───────────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup() {
    playing = false;

    if (audio_player) {
        audio_player->stop();
        audio_generator.unref();
    }

    if (swr_ctx)         { swr_free(&swr_ctx);                    swr_ctx = nullptr; }
    if (sws_ctx)         { sws_freeContext(sws_ctx);              sws_ctx = nullptr; }
    if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); audio_codec_ctx = nullptr; }
    if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); video_codec_ctx = nullptr; }
    if (fmt_ctx)         { avformat_close_input(&fmt_ctx);         fmt_ctx = nullptr; }
    if (frame_buffer)    { av_free(frame_buffer);                  frame_buffer = nullptr; }

    video_stream_idx = audio_stream_idx = -1;
    duration = position = 0.0;
    video_width = video_height = 0;
    fps = 0.0;

    // تحسين من القديم: التأكد من تحرير الـ Texture
    current_texture.unref();
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
