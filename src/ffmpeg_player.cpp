/**
 * ffmpeg_player.cpp
 * GDExtension - FFmpeg Video + Audio Player for Godot 4 (Android ARM64/ARM32)
 *
 * الإصدار: 2.0 - نظام بث احترافي مع Buffer ديناميكي + Seek ذكي
 *
 * الإصلاحات الجوهرية (v1 → v2):
 *  1. [حرج] إصلاح منطق PTS الذي كان يُسقط كل إطارات العتاد → الفيديو يتوقف بعد لحظة
 *  2. [حرج] حذف break داخل حلقة avcodec_receive_frame → كان يُجمد الفيديو
 *  3. [مهم] إصلاح رفض الصوت حين يكون البافر ممتلئاً → يوقف المزامنة
 *  4. [تحذير] إزالة #include <libavcodec/mediacodec.h> → غير موجود في FFmpeg 7.0
 *
 * الميزات الجديدة:
 *  - دعم البث من الإنترنت (HTTP / HTTPS / HLS m3u8)
 *  - Buffer ديناميكي: 5s بداية → 20s أمام → 60s خلف
 *  - Seek ذكي: سريع داخل البافر، إعادة تحميل خارجه
 *  - إعادة اتصال تلقائية عند انقطاع الإنترنت
 *  - إدارة أخطاء شاملة عبر emit_playback_error
 */

#include "ffmpeg_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
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

// ─── تسجيل الكلاس ────────────────────────────────────────────────────────────
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

    ClassDB::bind_method(D_METHOD("set_loop",   "enable"), &FFmpegPlayer::set_loop);
    ClassDB::bind_method(D_METHOD("get_loop"),             &FFmpegPlayer::get_loop);
    ClassDB::bind_method(D_METHOD("set_volume", "vol"),    &FFmpegPlayer::set_volume);
    ClassDB::bind_method(D_METHOD("get_volume"),           &FFmpegPlayer::get_volume);

    // ── معلومات البافر للـ UI ─────────────────────────────────────────────────
    ClassDB::bind_method(D_METHOD("get_forward_buffer"),        &FFmpegPlayer::get_forward_buffer);
    ClassDB::bind_method(D_METHOD("get_back_buffer"),           &FFmpegPlayer::get_back_buffer);
    ClassDB::bind_method(D_METHOD("is_buffering"),              &FFmpegPlayer::is_buffering);

    ADD_PROPERTY(PropertyInfo(Variant::BOOL,  "loop"),   "set_loop",   "get_loop");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");

    ADD_SIGNAL(MethodInfo("video_loaded",   PropertyInfo(Variant::BOOL,   "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",  PropertyInfo(Variant::OBJECT, "texture")));
    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("playback_error", PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("buffering_changed", PropertyInfo(Variant::BOOL, "is_buffering")));
}

// ─── البنّاء والهادم ──────────────────────────────────────────────────────────
FFmpegPlayer::FFmpegPlayer()
    : fmt_ctx(nullptr), video_codec_ctx(nullptr), sws_ctx(nullptr),
      video_stream_idx(-1), video_width(0), video_height(0), fps(0.0),
      frame_buffer(nullptr), audio_codec_ctx(nullptr), swr_ctx(nullptr),
      audio_stream_idx(-1), audio_sample_rate(44100), audio_channels(2),
      audio_player(nullptr), playing(false), looping(false), volume(1.0f),
      duration(0.0), position(0.0)
{}

FFmpegPlayer::~FFmpegPlayer() {
    _cleanup();
}

// ─── _ready ───────────────────────────────────────────────────────────────────
void FFmpegPlayer::_ready() {
    audio_player = memnew(AudioStreamPlayer);
    audio_player->set_name("_AudioPlayer");
    add_child(audio_player);

    UtilityFunctions::print("--- FFmpeg GDExtension v2.0 Ready ---");
    UtilityFunctions::print("[INFO] Dynamic Buffer: ", MIN_FORWARD, "s forward / ", MAX_BACK, "s back");

    void *opaque = nullptr;
    const AVCodec *codec;
    bool has_mediacodec = false;
    while ((codec = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(codec) && String(codec->name).find("mediacodec") != -1) {
            UtilityFunctions::print("[HW] Found: ", codec->name);
            has_mediacodec = true;
        }
    }
    if (!has_mediacodec) {
        UtilityFunctions::printerr("[WARN] MediaCodec decoders not found — will use software.");
    }
}

// ─── تحميل الفيديو ───────────────────────────────────────────────────────────
bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();
    buffering = false;
    forward_buffer_secs = 0.0;
    back_buffer_secs    = 0.0;

    if (path.is_empty()) {
        _emit_playback_error("Path is empty");
        return false;
    }

    if (!audio_player) {
        audio_player = memnew(AudioStreamPlayer);
        add_child(audio_player);
    }

    // ── تحديد نوع المصدر (ملف محلي / إنترنت) ──────────────────────────────
    is_streaming = path.begins_with("http://") || path.begins_with("https://")
                || path.begins_with("rtmp://") || path.begins_with("rtsp://");

    String real_path;
    if (is_streaming) {
        real_path = path;
        UtilityFunctions::print("[LOAD] Streaming mode: ", real_path);
    } else {
        real_path = ProjectSettings::get_singleton()->globalize_path(path);
        UtilityFunctions::print("[LOAD] Local file: ", real_path);
    }

    CharString utf8_path = real_path.utf8();
    const char *c_path   = utf8_path.get_data();

    // ── إعداد خيارات FFmpeg للبث ──────────────────────────────────────────
    AVDictionary *options = nullptr;
    if (is_streaming) {
        av_dict_set(&options, "fflags",            "nobuffer",  0);
        av_dict_set(&options, "flags",             "low_delay", 0);
        av_dict_set(&options, "probesize",         "65536",     0);
        av_dict_set(&options, "analyzeduration",   "500000",    0);
        av_dict_set(&options, "reconnect",         "1",         0);
        av_dict_set(&options, "reconnect_streamed","1",         0);
        av_dict_set(&options, "reconnect_delay_max","2",        0);
        av_dict_set(&options, "protocol_whitelist","file,http,https,tcp,tls,crypto,hls,applehttp", 0);
    }

    if (avformat_open_input(&fmt_ctx, c_path, nullptr, &options) < 0) {
        av_dict_free(&options);
        _emit_playback_error("Cannot open: " + path);
        _emit_video_loaded(false);
        return false;
    }
    av_dict_free(&options);

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        _cleanup();
        _emit_playback_error("Cannot read stream info");
        return false;
    }

    // ── فتح مشفر الفيديو ─────────────────────────────────────────────────
    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx >= 0) {
        AVStream      *vstream = fmt_ctx->streams[video_stream_idx];
        const AVCodec *vcodec  = nullptr;

        // تجربة العتاد أولاً
        if (vstream->codecpar->codec_id == AV_CODEC_ID_H264) {
            vcodec = avcodec_find_decoder_by_name("h264_mediacodec");
        } else if (vstream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            vcodec = avcodec_find_decoder_by_name("hevc_mediacodec");
        } else if (vstream->codecpar->codec_id == AV_CODEC_ID_VP8) {
            vcodec = avcodec_find_decoder_by_name("vp8_mediacodec");
        } else if (vstream->codecpar->codec_id == AV_CODEC_ID_VP9) {
            vcodec = avcodec_find_decoder_by_name("vp9_mediacodec");
        }

        if (!vcodec) {
            // صمام أمان: المشفر البرمجي
            vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
            UtilityFunctions::print("[VIDEO] Mode: SOFTWARE");
        } else {
            UtilityFunctions::print("[VIDEO] Mode: HARDWARE (MediaCodec)");
        }

        if (!vcodec) {
            _emit_playback_error("No decoder found for video stream");
            _cleanup();
            return false;
        }

        video_codec_ctx = avcodec_alloc_context3(vcodec);
        avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);

        // تعدد الأنوية للمشفر البرمجي فقط
        if (String(vcodec->name).find("mediacodec") == -1) {
            video_codec_ctx->thread_count = 0;
        }

        // ── [إصلاح #4] إزالة محاولة ضبط hw_device_ctx يدوياً
        // MediaCodec عبر FFmpeg لا يحتاجها — تُسبب SEGFAULT ──────────────
        AVDictionary *codec_opts = nullptr;
        if (avcodec_open2(video_codec_ctx, vcodec, &codec_opts) < 0) {
            av_dict_free(&codec_opts);
            _emit_playback_error("Cannot open video decoder");
            _cleanup();
            return false;
        }
        av_dict_free(&codec_opts);

        video_width  = video_codec_ctx->width;
        video_height = video_codec_ctx->height;
        fps          = av_q2d(vstream->r_frame_rate);

        // SWS_FAST_BILINEAR أسرع على الموبايل
        sws_ctx = sws_getContext(
            video_width, video_height, video_codec_ctx->pix_fmt,
            video_width, video_height, AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );

        _allocate_buffers();
    }

    if (audio_stream_idx >= 0) {
        if (!_setup_audio(fmt_ctx->streams[audio_stream_idx])) {
            UtilityFunctions::printerr("[WARN] Audio setup failed — video only.");
        }
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE
               : 0.0;

    _emit_video_loaded(true);
    UtilityFunctions::print("[LOAD] OK — duration: ", duration, "s | fps: ", fps);
    return true;
}

// ─── تهيئة البافرات ────────────────────────────────────────────────────────
void FFmpegPlayer::_allocate_buffers() {
    if (frame_buffer) { av_free(frame_buffer); frame_buffer = nullptr; }

    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);
    if (!frame_buffer) {
        UtilityFunctions::printerr("[MEM] Failed to allocate frame buffer!");
        return;
    }

    PackedByteArray black;
    black.resize(video_width * video_height * 3);
    black.fill(0);

    Ref<Image> tmp = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, black);
    if (current_texture.is_null()) current_texture.instantiate();
    current_texture->set_image(tmp);

    UtilityFunctions::print("[MEM] Buffers ready: ", video_width, "x", video_height);
}

// ─── play ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) {
        UtilityFunctions::printerr("[PLAY] No video loaded.");
        return;
    }

    // ملء أولي: انتظر حتى INITIAL_PLAY ثواني قبل البدء
    _prefill_buffers();

    playing = true;
    buffering = false;

    if (audio_player) {
        audio_player->set_stream_paused(false);
        if (!audio_player->is_playing()) audio_player->play();
    }
    UtilityFunctions::print("[PLAY] Started.");
}

void FFmpegPlayer::pause() {
    playing = false;
    if (audio_player) audio_player->set_stream_paused(true);
}

void FFmpegPlayer::stop() {
    playing = false;
    if (audio_player) audio_player->stop();
    seek(0.0);
}

// ─── Seek ذكي ────────────────────────────────────────────────────────────────
// المنطق:
//  • إذا كان الهدف داخل البافر الخلفي أو الأمامي → Seek سريع بدون تحميل
//  • خارج البافر → flush + إعادة تحميل
void FFmpegPlayer::seek(double seconds) {
    if (!fmt_ctx || !video_codec_ctx) return;

    bool was_playing = playing;
    playing = false;
    if (audio_player) audio_player->stop();

    double range_start = position - back_buffer_secs  - 0.5;
    double range_end   = position + forward_buffer_secs + 0.5;
    bool   in_buffer   = (seconds >= range_start && seconds <= range_end);

    UtilityFunctions::print("[SEEK] Target=", seconds,
                            " InBuffer=", in_buffer ? "YES" : "NO");

    if (in_buffer) {
        // ── Seek سريع: فقط نُحدّث الموقع وندع نظام PTS يُسقط الإطارات القديمة
        position        = seconds;
        audio_pts_set   = false;
        audio_pts_offset = seconds;
        UtilityFunctions::print("[SEEK] Fast seek (buffer intact).");
    } else {
        // ── Seek كامل: تنظيف + إعادة تحميل
        _clear_queues();

        int64_t seek_target = (int64_t)(seconds * AV_TIME_BASE);
        if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
            UtilityFunctions::printerr("[SEEK] av_seek_frame failed for: ", seconds);
            _emit_playback_error("Seek failed");
            if (was_playing) { playing = true; if (audio_player) audio_player->play(); }
            return;
        }

        avcodec_flush_buffers(video_codec_ctx);
        if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);
        _clear_audio_buffers();

        // إعادة بناء sws_ctx: بعض صيغ العتاد تُغير pixel format بعد flush
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }

        position         = seconds;
        audio_pts_offset = seconds;
        audio_pts_set    = false;
        forward_buffer_secs = 0.0;
        back_buffer_secs    = 0.0;

        // ملء أولي سريع قبل استئناف التشغيل
        buffering = true;
        if (is_inside_tree()) emit_signal("buffering_changed", true);

        for (int i = 0; i < 30; i++) _read_packets_to_queue();
        _update_buffer_stats();

        if (forward_buffer_secs >= INITIAL_PLAY) {
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
        }

        UtilityFunctions::print("[SEEK] Full seek + reload complete.");
    }

    if (was_playing) {
        playing = true;
        if (audio_player) audio_player->play();
    }
}

// ─── _process ────────────────────────────────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!fmt_ctx) return;

    // حساب البافر الحالي
    _update_buffer_stats();

    // حالة Buffering: إذا انتهى البافر نوقف الفيديو حتى يمتلئ
    if (playing && forward_buffer_secs < 1.0 && !is_streaming) {
        playing  = false;
        buffering = true;
        if (audio_player) audio_player->set_stream_paused(true);
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        UtilityFunctions::printerr("[BUFFER] Underrun! Pausing to refill...");
    }

    if (buffering) {
        _read_packets_to_queue();
        _update_buffer_stats();
        if (forward_buffer_secs >= INITIAL_PLAY) {
            buffering = false;
            playing   = true;
            if (audio_player) audio_player->set_stream_paused(false);
            if (!audio_player->is_playing()) audio_player->play();
            if (is_inside_tree()) emit_signal("buffering_changed", false);
            UtilityFunctions::print("[BUFFER] Refilled. Resuming.");
        }
        return;
    }

    if (!playing) return;

    // تحديث الساعة المرجعية
    if (audio_stream_idx >= 0 && audio_player && audio_player->is_playing()) {
        double audio_time = audio_pts_offset + audio_player->get_playback_position();
        if (Math::abs(audio_time - position) > 0.5) {
            position = audio_time;
        } else {
            position = Math::lerp(position, audio_time, 0.5);
        }
    } else {
        position += delta;
    }

    _decode_next_frame();

    // نهاية الفيديو
    if (duration > 0.0 && position >= duration) {
        if (looping) {
            seek(0.0);
        } else {
            stop();
            _emit_video_finished();
        }
    }
}

// ─── حساب حجم البافر الديناميكي ──────────────────────────────────────────────
void FFmpegPlayer::_update_buffer_stats() {
    if (video_stream_idx < 0) return;

    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double fwd = 0.0, bck = 0.0;

    for (AVPacket *pkt : video_packet_queue) {
        if (pkt->pts == AV_NOPTS_VALUE) continue;
        double pts = pkt->pts * tb;
        if (pts >= position) fwd = Math::max(fwd, pts - position);
        else                 bck = Math::max(bck, position - pts);
    }
    forward_buffer_secs = fwd;
    back_buffer_secs    = bck;
}

// ─── حذف الحزم القديمة من الخلف ──────────────────────────────────────────────
void FFmpegPlayer::_trim_back_buffer() {
    if (video_stream_idx < 0) return;
    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

    while (back_buffer_secs > MAX_BACK && !video_packet_queue.empty()) {
        AVPacket *oldest = video_packet_queue.front();
        if (oldest->pts != AV_NOPTS_VALUE) {
            double pts = oldest->pts * tb;
            if (pts < position) {
                back_buffer_secs -= (position - pts);
                av_packet_free(&oldest);
                video_packet_queue.pop_front();
                continue;
            }
        }
        break;
    }
}

// ─── حجم الدفعة الديناميكية ───────────────────────────────────────────────────
int FFmpegPlayer::_calc_read_batch_size() const {
    if (forward_buffer_secs < MIN_FORWARD)  return 80;
    if (forward_buffer_secs < MAX_FORWARD)  return 40;
    return 0; // البافر ممتلئ، لا داعي للقراءة
}

// ─── قراءة الحزم إلى الطابور ─────────────────────────────────────────────────
void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;

    int batch = _calc_read_batch_size();
    if (batch == 0) return;

    AVPacket *packet = av_packet_alloc();
    for (int i = 0; i < batch; i++) {
        if (av_read_frame(fmt_ctx, packet) < 0) {
            // نهاية الملف أو خطأ قراءة
            av_packet_unref(packet);
            break;
        }
        if (packet->stream_index == video_stream_idx) {
            video_packet_queue.push_back(av_packet_clone(packet));
        } else if (packet->stream_index == audio_stream_idx) {
            audio_packet_queue.push_back(av_packet_clone(packet));
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    _trim_back_buffer();
}

// ─── ملء أولي قبل التشغيل ────────────────────────────────────────────────────
void FFmpegPlayer::_prefill_buffers() {
    if (!fmt_ctx) return;
    UtilityFunctions::print("[PREFILL] Filling to ", INITIAL_PLAY, "s...");

    // نملأ حتى يصل البافر لـ INITIAL_PLAY ثواني
    int attempts = 0;
    while (forward_buffer_secs < INITIAL_PLAY && attempts < 200) {
        _read_packets_to_queue();
        _update_buffer_stats();
        attempts++;
    }

    // فك تشفير صوت مسبق لملء بفر Godot
    if (audio_codec_ctx) {
        int prefill_count = 0;
        while (!audio_packet_queue.empty() && prefill_count < 30) {
            AVPacket *a_pkt = audio_packet_queue.front();
            if (avcodec_send_packet(audio_codec_ctx, a_pkt) == 0) {
                AVFrame *af = av_frame_alloc();
                while (avcodec_receive_frame(audio_codec_ctx, af) == 0) {
                    _push_audio_samples(af);
                    av_frame_unref(af);
                }
                av_frame_free(&af);
            }
            audio_packet_queue.pop_front();
            av_packet_free(&a_pkt);
            prefill_count++;
        }
    }

    UtilityFunctions::print("[PREFILL] Done. Forward=", forward_buffer_secs, "s");
}

// ─── فك التشفير وعرض الإطار ─────────────────────────────────────────────────
// الإصلاحات:
//  [#1] لم نعد نُسقط إطارات العتاد بسبب PTS مختلف بسيط
//  [#2] تم حذف break — الحلقة تُفرغ البافر بالكامل
//  [#3] الصوت يُرسل دائماً حتى حين البافر قريب الامتلاء
void FFmpegPlayer::_decode_next_frame() {
    _read_packets_to_queue();

    // ── 1. معالجة الصوت ─────────────────────────────────────────────────────
    if (audio_player && audio_player->is_playing() && audio_codec_ctx) {
        Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
        if (pb.is_valid()) {
            int space = pb->get_frames_available();
            // [إصلاح #3] إرسال ما يتسع فقط، لا رفض الكل إذا البفر ممتلئ
            while (!audio_packet_queue.empty() && space > 512) {
                AVPacket *a_pkt = audio_packet_queue.front();
                if (avcodec_send_packet(audio_codec_ctx, a_pkt) == 0) {
                    AVFrame *af = av_frame_alloc();
                    while (avcodec_receive_frame(audio_codec_ctx, af) == 0) {
                        _push_audio_samples(af);
                        space -= af->nb_samples;
                        av_frame_unref(af);
                    }
                    av_frame_free(&af);
                }
                audio_packet_queue.pop_front();
                av_packet_free(&a_pkt);
            }
        }
    }

    if (video_packet_queue.empty() || !video_codec_ctx) return;

    // ── 2. إرسال حزم الفيديو للمشفر ─────────────────────────────────────────
    while (!video_packet_queue.empty()) {
        AVPacket *pkt    = video_packet_queue.front();
        int       result = avcodec_send_packet(video_codec_ctx, pkt);
        if (result == 0) {
            video_packet_queue.pop_front();
            av_packet_free(&pkt);
        } else if (result == AVERROR(EAGAIN)) {
            // المشفر ممتلئ، يجب استقبال إطارات أولاً → خروج من حلقة الإرسال
            break;
        } else {
            // خطأ في الحزمة → تجاهلها والمضي
            video_packet_queue.pop_front();
            av_packet_free(&pkt);
        }
    }

    // ── 3. استقبال الإطارات ─────────────────────────────────────────────────
    // [إصلاح #1 + #2] لا نسقط الإطار فوراً بسبب PTS، نعرض أقرب إطار للحظة الحالية
    AVFrame *vf           = av_frame_alloc();
    AVFrame *best_frame   = nullptr;
    double   best_diff    = 1e9;
    const double TOLERANCE = 1.0 / (fps > 0 ? fps : 30.0) * 2.0; // مرونة بضعف مدة الإطار

    // [إصلاح #2] لا break — نُفرغ كل الإطارات ونختار الأفضل
    while (avcodec_receive_frame(video_codec_ctx, vf) == 0) {
        double frame_pts = (vf->pts != AV_NOPTS_VALUE)
            ? vf->pts * av_q2d(fmt_ctx->streams[video_stream_idx]->time_base)
            : position;

        double diff = frame_pts - position;

        // إطار قديم جداً (أكثر من ثانية خلف) → تجاهل كلياً
        if (diff < -1.0) {
            av_frame_unref(vf);
            continue;
        }

        // [إصلاح #1] نختار الإطار الأقرب للحظة الحالية (الأقل diff إيجابياً)
        if (best_frame == nullptr || (diff >= 0 && diff < best_diff)) {
            if (best_frame) av_frame_free(&best_frame);
            best_frame = av_frame_clone(vf);
            best_diff  = diff;
        }
        av_frame_unref(vf);
    }
    av_frame_free(&vf);

    if (!best_frame) return;

    // ── 4. تحويل الإطار وإرساله إلى Godot ───────────────────────────────────
    // إعادة بناء sws_ctx إذا تغير pixel format (يحدث مع MediaCodec)
    if (!sws_ctx || video_codec_ctx->pix_fmt != (AVPixelFormat)best_frame->format) {
        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
        sws_ctx = sws_getContext(
            video_width, video_height, (AVPixelFormat)best_frame->format,
            video_width, video_height, AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!sws_ctx) {
            av_frame_free(&best_frame);
            return;
        }
        UtilityFunctions::print("[SWS] Rebuilt for format: ", (int)best_frame->format);
    }

    uint8_t *dest[4]     = { frame_buffer, nullptr, nullptr, nullptr };
    int dest_linesize[4] = { video_width * 3, 0, 0, 0 };
    sws_scale(sws_ctx, best_frame->data, best_frame->linesize, 0, video_height, dest, dest_linesize);

    PackedByteArray pba;
    pba.resize(video_width * video_height * 3);
    memcpy(pba.ptrw(), frame_buffer, pba.size());

    Ref<Image> img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, pba);
    if (current_texture.is_valid()) current_texture->update(img);
    _emit_frame_updated();

    av_frame_free(&best_frame);
}

// ─── دفع الصوت إلى Godot ─────────────────────────────────────────────────────
void FFmpegPlayer::_push_audio_samples(AVFrame *frame) {
    if (!frame || !audio_player || !swr_ctx) return;

    Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
    if (pb.is_null()) return;

    // ضبط الساعة المرجعية عند أول إطار
    if (!audio_pts_set && frame->pts != AV_NOPTS_VALUE && audio_stream_idx >= 0) {
        double frame_pts = frame->pts * av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base);
        audio_pts_offset = frame_pts;
        audio_pts_set    = true;
        position         = audio_pts_offset;
        UtilityFunctions::print("[AUDIO] Clock reset. Offset=", audio_pts_offset);
    }

    // [إصلاح #3] إرسال جزء من العينات إذا لم تكن هناك مساحة كاملة
    int frames_available = pb->get_frames_available();
    if (frames_available <= 0) return;

    int out_count = (int)av_rescale_rnd(
        swr_get_delay(swr_ctx, audio_sample_rate) + frame->nb_samples,
        audio_sample_rate, audio_codec_ctx->sample_rate, AV_ROUND_UP
    );

    uint8_t *out_buf  = nullptr;
    int      linesize = 0;
    if (av_samples_alloc(&out_buf, &linesize, 2, out_count, AV_SAMPLE_FMT_FLT, 0) < 0) return;

    int converted = swr_convert(swr_ctx, &out_buf, out_count,
                                (const uint8_t **)frame->data, frame->nb_samples);

    if (converted > 0) {
        // إرسال ما يتسع فقط (لا رفض الكل)
        int to_push = Math::min(converted, frames_available);
        const float *samples = reinterpret_cast<const float *>(out_buf);
        PackedVector2Array buffer;
        buffer.resize(to_push);
        Vector2 *wptr = buffer.ptrw();

        for (int i = 0; i < to_push; i++) {
            wptr[i] = Vector2(
                CLAMP(samples[i * 2]     * volume, -1.0f, 1.0f),
                CLAMP(samples[i * 2 + 1] * volume, -1.0f, 1.0f)
            );
        }
        pb->push_buffer(buffer);
    }

    if (out_buf) av_freep(&out_buf);
}

// ─── إعداد الصوت ─────────────────────────────────────────────────────────────
bool FFmpegPlayer::_setup_audio(AVStream *astream) {
    if (!astream || !astream->codecpar) return false;

    const AVCodec *codec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!codec) return false;

    audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);
    if (avcodec_open2(audio_codec_ctx, codec, nullptr) < 0) return false;

    if (swr_ctx) swr_free(&swr_ctx);
    swr_ctx = swr_alloc();

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);

    av_opt_set_chlayout(swr_ctx, "in_chlayout",  &audio_codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout, 0);
    av_opt_set_int(swr_ctx,      "in_sample_rate",  audio_codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx,      "out_sample_rate", audio_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",  audio_codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_ctx) < 0) return false;

    audio_sample_rate = audio_codec_ctx->sample_rate;

    audio_generator.instantiate();
    audio_generator->set_mix_rate(audio_sample_rate);
    audio_generator->set_buffer_length(0.5f);

    if (audio_player) {
        audio_player->set_stream(audio_generator);
    }
    return true;
}

// ─── تنظيف الطوابير ──────────────────────────────────────────────────────────
void FFmpegPlayer::_clear_queues() {
    while (!video_packet_queue.empty()) {
        AVPacket *p = video_packet_queue.front();
        video_packet_queue.pop_front();
        av_packet_free(&p);
    }
    while (!audio_packet_queue.empty()) {
        AVPacket *p = audio_packet_queue.front();
        audio_packet_queue.pop_front();
        av_packet_free(&p);
    }
    forward_buffer_secs = 0.0;
    back_buffer_secs    = 0.0;
}

void FFmpegPlayer::_clear_audio_buffers() {
    if (swr_ctx) { swr_close(swr_ctx); swr_init(swr_ctx); }
    if (audio_player) audio_player->stop();
}

void FFmpegPlayer::_cleanup() {
    _clear_queues();
    if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); video_codec_ctx = nullptr; }
    if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); audio_codec_ctx = nullptr; }
    if (fmt_ctx)         { avformat_close_input(&fmt_ctx);         fmt_ctx         = nullptr; }
    if (sws_ctx)         { sws_freeContext(sws_ctx);               sws_ctx         = nullptr; }
    if (swr_ctx)         { swr_free(&swr_ctx);                     swr_ctx         = nullptr; }
    if (frame_buffer)    { av_free(frame_buffer);                  frame_buffer    = nullptr; }
    duration = position = 0.0;
    is_streaming = false;
}

// ─── إشارات ──────────────────────────────────────────────────────────────────
void FFmpegPlayer::_emit_video_loaded(bool s) {
    if (is_inside_tree()) emit_signal("video_loaded", s);
}
void FFmpegPlayer::_emit_video_finished() {
    if (is_inside_tree()) emit_signal("video_finished");
}
void FFmpegPlayer::_emit_frame_updated() {
    if (is_inside_tree()) emit_signal("frame_updated", current_texture);
}
void FFmpegPlayer::_emit_playback_error(const String &msg) {
    UtilityFunctions::printerr("[ERROR] ", msg);
    if (is_inside_tree()) emit_signal("playback_error", msg);
}

// ─── Getters / Setters ────────────────────────────────────────────────────────
bool   FFmpegPlayer::is_playing()       const { return playing; }
double FFmpegPlayer::get_duration()     const { return duration; }
double FFmpegPlayer::get_position()     const { return position; }
int    FFmpegPlayer::get_video_width()  const { return video_width; }
int    FFmpegPlayer::get_video_height() const { return video_height; }
double FFmpegPlayer::get_fps()          const { return fps; }

Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const {
    return current_texture;
}

void FFmpegPlayer::set_loop(bool en) { looping = en; }
bool FFmpegPlayer::get_loop() const  { return looping; }

void FFmpegPlayer::set_volume(float v) {
    volume = v;
    if (audio_player) {
        float db = (volume <= 0.0001f) ? -80.0f : 20.0f * log10(volume);
        audio_player->set_volume_db(db);
    }
}
float FFmpegPlayer::get_volume() const { return volume; }

// ─── نقطة دخول GDExtension ───────────────────────────────────────────────────
extern "C" {
    GDExtensionBool GDE_EXPORT gdffmpeg_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr   p_library,
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
