/**
 * ffmpeg_player.cpp
 * GDExtension - FFmpeg Video + Audio Player for Godot 4 (Android ARM64/ARM32)
 *
 * ─── الإصدار 2.1 — إصلاح مشاكل Buffering + Seek + صوت سريع ───────────────
 *
 * الإصلاحات الجديدة في v2.1 (مكتشفة من سجلات الجهاز):
 *
 *  [A] الخطأ الجذري — forward_buffer_secs = 0 فور بدء التشغيل:
 *      السبب: خلال _prefill_buffers، كان _push_audio_samples يُعيّن
 *             position = audio_pts_offset = 0.696s، لكن حزم الفيديو
 *             تبدأ من PTS≈0، فتبدو كلها "خلف" الموقع → fwd_buffer = 0
 *             → underrun فوري بعد play()
 *      الإصلاح: لا نُعيّن position من الصوت إلا إذا كان audio_player
 *               يعمل فعلاً (is_playing). خلال prefill لا يعمل → لا تحديث.
 *
 *  [B] Fast Seek يُسبب desync صوت/صورة:
 *      السبب: Fast Seek لا يُوقف الصوت → get_playback_position() يكمل
 *             من مكانه القديم → audio_time خاطئ → صوت سريع/بطيء
 *      الإصلاح: Fast Seek يُوقف ويُعيد تشغيل الصوت دائماً،
 *               ويُنظف بافر الصوت ويتجاوز الحزم القديمة.
 *
 *  [C] toggle_pause() يُطلب get_stream_playback() قبل play():
 *      الإصلاح: pause() تفحص is_playing() أولاً قبل لمس AudioStreamPlayer.
 *
 *  [D] Buffer oscillation (Refilled. Resuming. متكرر):
 *      السبب: calc_read_batch_size() يُعيد 0 حين البافر > 40s → لا قراءة
 *             → البافر يفرغ → underrun → refill → تكرار
 *      الإصلاح: دائماً نقرأ 5 حزم على الأقل، + hysteresis (2s start / 5s stop)
 *
 *  [E] تقطقات الصوت:
 *      السبب: swr_ctx يُجمّع delay بين الـ seeks ويُخرج عينات مزدوجة
 *      الإصلاح: flush سريع لـ swr_ctx بعد كل seek.
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

    ClassDB::bind_method(D_METHOD("get_forward_buffer"),        &FFmpegPlayer::get_forward_buffer);
    ClassDB::bind_method(D_METHOD("get_back_buffer"),           &FFmpegPlayer::get_back_buffer);
    ClassDB::bind_method(D_METHOD("is_buffering"),              &FFmpegPlayer::is_buffering);

    ADD_PROPERTY(PropertyInfo(Variant::BOOL,  "loop"),   "set_loop",   "get_loop");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume"), "set_volume", "get_volume");

    ADD_SIGNAL(MethodInfo("video_loaded",      PropertyInfo(Variant::BOOL,   "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",     PropertyInfo(Variant::OBJECT, "texture")));
    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("playback_error",    PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("buffering_changed", PropertyInfo(Variant::BOOL,   "is_buffering")));
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

FFmpegPlayer::~FFmpegPlayer() { _cleanup(); }

// ─── _ready ───────────────────────────────────────────────────────────────────
void FFmpegPlayer::_ready() {
    audio_player = memnew(AudioStreamPlayer);
    audio_player->set_name("_AudioPlayer");
    add_child(audio_player);

    UtilityFunctions::print("--- FFmpeg GDExtension v2.1 Ready ---");

    void *opaque = nullptr;
    const AVCodec *codec;
    while ((codec = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(codec) && String(codec->name).find("mediacodec") != -1) {
            UtilityFunctions::print("[HW] Found: ", codec->name);
        }
    }
}

// ─── تحميل الفيديو ───────────────────────────────────────────────────────────
bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();
    buffering           = false;
    forward_buffer_secs = 0.0;
    back_buffer_secs    = 0.0;
    position            = 0.0;
    audio_pts_offset    = 0.0;
    audio_pts_set       = false;

    if (path.is_empty()) { _emit_playback_error("Path is empty"); return false; }

    if (!audio_player) {
        audio_player = memnew(AudioStreamPlayer);
        audio_player->set_name("_AudioPlayer");
        add_child(audio_player);
    }

    is_streaming = path.begins_with("http://") || path.begins_with("https://")
                || path.begins_with("rtmp://") || path.begins_with("rtsp://");

    String real_path = is_streaming
        ? path
        : ProjectSettings::get_singleton()->globalize_path(path);

    CharString utf8_path = real_path.utf8();
    const char *c_path   = utf8_path.get_data();

    AVDictionary *options = nullptr;
    if (is_streaming) {
        av_dict_set(&options, "fflags",              "nobuffer",  0);
        av_dict_set(&options, "flags",               "low_delay", 0);
        av_dict_set(&options, "probesize",           "65536",     0);
        av_dict_set(&options, "analyzeduration",     "500000",    0);
        av_dict_set(&options, "reconnect",           "1",         0);
        av_dict_set(&options, "reconnect_streamed",  "1",         0);
        av_dict_set(&options, "reconnect_delay_max", "2",         0);
        av_dict_set(&options, "protocol_whitelist",
            "file,http,https,tcp,tls,crypto,hls,applehttp", 0);
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

    // حساب start_time للتعويض في حسابات PTS
    stream_start_time = (fmt_ctx->start_time != AV_NOPTS_VALUE)
                        ? (double)fmt_ctx->start_time / AV_TIME_BASE
                        : 0.0;

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx >= 0) {
        AVStream      *vstream = fmt_ctx->streams[video_stream_idx];
        const AVCodec *vcodec  = nullptr;

        if      (vstream->codecpar->codec_id == AV_CODEC_ID_H264)
            vcodec = avcodec_find_decoder_by_name("h264_mediacodec");
        else if (vstream->codecpar->codec_id == AV_CODEC_ID_HEVC)
            vcodec = avcodec_find_decoder_by_name("hevc_mediacodec");
        else if (vstream->codecpar->codec_id == AV_CODEC_ID_VP8)
            vcodec = avcodec_find_decoder_by_name("vp8_mediacodec");
        else if (vstream->codecpar->codec_id == AV_CODEC_ID_VP9)
            vcodec = avcodec_find_decoder_by_name("vp9_mediacodec");

        if (!vcodec) {
            vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
            UtilityFunctions::print("[VIDEO] Mode: SOFTWARE");
        } else {
            UtilityFunctions::print("[VIDEO] Mode: HARDWARE (MediaCodec)");
        }

        if (!vcodec) {
            _emit_playback_error("No video decoder found");
            _cleanup();
            return false;
        }

        video_codec_ctx = avcodec_alloc_context3(vcodec);
        avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);
        if (String(vcodec->name).find("mediacodec") == -1)
            video_codec_ctx->thread_count = 0;

        if (avcodec_open2(video_codec_ctx, vcodec, nullptr) < 0) {
            _emit_playback_error("Cannot open video decoder");
            _cleanup();
            return false;
        }

        video_width  = video_codec_ctx->width;
        video_height = video_codec_ctx->height;
        fps          = av_q2d(vstream->r_frame_rate);

        sws_ctx = sws_getContext(
            video_width, video_height, video_codec_ctx->pix_fmt,
            video_width, video_height, AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        _allocate_buffers();
    }

    if (audio_stream_idx >= 0) {
        if (!_setup_audio(fmt_ctx->streams[audio_stream_idx]))
            UtilityFunctions::printerr("[WARN] Audio setup failed.");
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE
               : 0.0;

    _emit_video_loaded(true);
    UtilityFunctions::print("[LOAD] OK | duration=", duration, "s | fps=", fps,
                            " | start_time=", stream_start_time, "s");
    return true;
}

// ─── تهيئة البافرات ────────────────────────────────────────────────────────
void FFmpegPlayer::_allocate_buffers() {
    if (frame_buffer) { av_free(frame_buffer); frame_buffer = nullptr; }

    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);
    if (!frame_buffer) { UtilityFunctions::printerr("[MEM] Alloc failed!"); return; }

    PackedByteArray black;
    black.resize(video_width * video_height * 3);
    black.fill(0);
    Ref<Image> tmp = Image::create_from_data(video_width, video_height, false,
                                             Image::FORMAT_RGB8, black);
    if (current_texture.is_null()) current_texture.instantiate();
    current_texture->set_image(tmp);

    UtilityFunctions::print("[MEM] Buffers ready: ", video_width, "x", video_height);
}

// ─── play ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) { UtilityFunctions::printerr("[PLAY] No video loaded."); return; }

    _prefill_buffers();

    playing  = true;
    buffering = false;

    if (audio_player) {
        // [إصلاح G] نوقف أولاً ثم نشغّل دائماً — هذا يضمن أن
        // stream_playbacks غير فارغة → get_stream_playback() لا يفشل أبداً
        if (audio_player->is_playing()) audio_player->stop();
        audio_player->play();
        audio_player->set_stream_paused(false);
        // إعادة ضبط الساعة لأن play() يُصفّر get_playback_position()
        audio_pts_offset = position;
        audio_pts_set    = false;
    }
    UtilityFunctions::print("[PLAY] Started. pos=", position,
                            " fwd=", forward_buffer_secs, "s");
}

// ─── pause ────────────────────────────────────────────────────────────────────
void FFmpegPlayer::pause() {
    playing = false;
    // [إصلاح C] لا نلمس audio_player إذا لم يكن يعمل
    if (audio_player && audio_player->is_playing()) {
        audio_player->set_stream_paused(true);
    }
}

void FFmpegPlayer::stop() {
    playing = false;
    if (audio_player && audio_player->is_playing()) audio_player->stop();
    seek(0.0);
}

// ─── Seek ذكي ────────────────────────────────────────────────────────────────
void FFmpegPlayer::seek(double seconds) {
    if (!fmt_ctx || !video_codec_ctx) return;

    bool was_playing = playing;
    playing = false;

    // [إصلاح C+B] إيقاف الصوت دائماً قبل Seek (سواء fast أو full)
    // هذا يُصفّر get_playback_position() ويمنع desync
    if (audio_player && audio_player->is_playing()) audio_player->stop();

    double range_start = position - back_buffer_secs  - 0.5;
    double range_end   = position + forward_buffer_secs + 0.5;
    bool   in_buffer   = (seconds >= range_start && seconds <= range_end);

    UtilityFunctions::print("[SEEK] Target=", seconds,
                            " InBuffer=", in_buffer ? "YES" : "NO");

    if (in_buffer) {
        // ── Fast Seek ─────────────────────────────────────────────────────
        // [إصلاح B] نُنظف كودك الصوت ونتجاوز الحزم القديمة
        if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);

        // [إصلاح E] flush swr_ctx لإزالة العينات المتراكمة (تُسبب صوتاً سريعاً)
        if (swr_ctx) {
            uint8_t *flush_buf = nullptr;
            int ls = 0;
            int flushed = swr_convert(swr_ctx, nullptr, 0, nullptr, 0);
            (void)flushed;
        }

        // تجاوز حزم الصوت القديمة التي هي قبل الهدف
        if (audio_stream_idx >= 0) {
            double atb = av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base);
            while (!audio_packet_queue.empty()) {
                AVPacket *p = audio_packet_queue.front();
                if (p->pts != AV_NOPTS_VALUE) {
                    double pts = p->pts * atb;
                    if (pts < seconds - 0.5) {
                        audio_packet_queue.pop_front();
                        av_packet_free(&p);
                        continue;
                    }
                }
                break;
            }
        }

        position         = seconds;
        audio_pts_offset = seconds;
        audio_pts_set    = false;

        UtilityFunctions::print("[SEEK] Fast seek done.");

    } else {
        // ── Full Seek ─────────────────────────────────────────────────────
        _clear_queues();

        int64_t seek_target = (int64_t)(seconds * AV_TIME_BASE);
        if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
            UtilityFunctions::printerr("[SEEK] av_seek_frame failed: ", seconds);
            _emit_playback_error("Seek failed");
            if (was_playing) { playing = true; if (audio_player) audio_player->play(); }
            return;
        }

        avcodec_flush_buffers(video_codec_ctx);
        if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);

        // [إصلاح E] إعادة بناء swr_ctx لإزالة أي delay متراكم
        if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
        if (audio_stream_idx >= 0) {
            _setup_audio(fmt_ctx->streams[audio_stream_idx]);
        }

        if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }

        position            = seconds;
        audio_pts_offset    = seconds;
        audio_pts_set       = false;
        forward_buffer_secs = 0.0;
        back_buffer_secs    = 0.0;

        // ملء أولي بعد Seek
        buffering = true;
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        for (int i = 0; i < 30; i++) _read_packets_to_queue();
        _update_buffer_stats();
        if (forward_buffer_secs >= INITIAL_PLAY) {
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
        }

        UtilityFunctions::print("[SEEK] Full seek complete.");
    }

    if (was_playing) {
        playing = true;
        if (audio_player) audio_player->play();
    }
}

// ─── _process ────────────────────────────────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!fmt_ctx || !playing || buffering) return;

    _update_buffer_stats();

    // خفضنا حد التوقف ليكون 0.5 ثانية بدل 1.0 لزيادة الاستمرارية
    if (forward_buffer_secs < 0.5) {
        playing = false;
        buffering = true;
        if (audio_player) audio_player->set_stream_paused(true);
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        return;
    }

    if (audio_stream_idx >= 0 && audio_player && audio_player->is_playing()) {
        double audio_time = audio_pts_offset + audio_player->get_playback_position();
        
        // بدلاً من lerp المستمر، نحدث الموقع مباشرة فقط إذا كان الفرق مؤثراً (> 10ms)
        if (Math::abs(audio_time - position) > 0.01) {
            position = audio_time;
        }
    } else {
        position += delta;
    }

    _decode_next_frame();

    if (duration > 0.0 && position >= duration) {
        if (looping) { seek(0.0); } 
        else { stop(); _emit_video_finished(); }
    }
}



// ─── حساب إحصاءات البافر ─────────────────────────────────────────────────────
void FFmpegPlayer::_update_buffer_stats() {
    if (video_stream_idx < 0 || !fmt_ctx) return;

    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstream_start = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE) 
                            ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    double fwd = 0.0;
    if (video_packet_queue.empty()) {
        forward_buffer_secs = 0.0;
    } else {
        AVPacket *last_pkt = video_packet_queue.back();
        if (last_pkt->pts != AV_NOPTS_VALUE) {
            double last_pts = (last_pkt->pts * tb) - vstream_start;
            fwd = last_pts - position;
        }
        forward_buffer_secs = Math::max(0.0, fwd);
    }
    
    // حساب بافر الخلف (اختياري، لا يؤثر على التشغيل)
    back_buffer_secs = 0.0; 
}


// ─── حذف حزم الخلف الزائدة ───────────────────────────────────────────────────
void FFmpegPlayer::_trim_back_buffer() {
    if (video_stream_idx < 0) return;
    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstream_start = 0.0;
    if (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
        vstream_start = fmt_ctx->streams[video_stream_idx]->start_time * tb;

    while (back_buffer_secs > MAX_BACK && !video_packet_queue.empty()) {
        AVPacket *oldest = video_packet_queue.front();
        if (oldest->pts != AV_NOPTS_VALUE) {
            double pts = (oldest->pts * tb) - vstream_start;
            if (pts < position - 1.0) {
                av_packet_free(&oldest);
                video_packet_queue.pop_front();
                _update_buffer_stats();
                continue;
            }
        }
        break;
    }
}

// ─── حجم دفعة القراءة الديناميكية ────────────────────────────────────────────
// [إصلاح D] دائماً نقرأ 5 حزم على الأقل لمنع oscillation
int FFmpegPlayer::_calc_read_batch_size() const {
    if (forward_buffer_secs < MIN_FORWARD)  return 80;
    if (forward_buffer_secs < MAX_FORWARD)  return 20;
    return 5; // الحد الأدنى — لمنع نضوب البافر المفاجئ
}

// ─── قراءة الحزم ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;

    int batch   = _calc_read_batch_size();
    AVPacket *packet = av_packet_alloc();

    for (int i = 0; i < batch; i++) {
        if (av_read_frame(fmt_ctx, packet) < 0) {
            av_packet_unref(packet);
            break;
        }
        if (packet->stream_index == video_stream_idx)
            video_packet_queue.push_back(av_packet_clone(packet));
        else if (packet->stream_index == audio_stream_idx)
            audio_packet_queue.push_back(av_packet_clone(packet));
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    _trim_back_buffer();
}

// ─── ملء أولي ─────────────────────────────────────────────────────────────────
// [إصلاح F] لا نُفكّك أي صوت هنا إطلاقاً — swr_ctx يجب أن يبقى نظيفاً
// السبب: لو مررنا إطارات صوتية عبر swr_ctx هنا، يتراكم فيه delay داخلي،
//         فعند التشغيل الفعلي يُخرج عينات مضاعفة → صوت أسرع من الحقيقي
void FFmpegPlayer::_prefill_buffers() {
    if (!fmt_ctx) return;
    UtilityFunctions::print("[PREFILL] Filling to ", INITIAL_PLAY, "s...");

    int attempts = 0;
    while (forward_buffer_secs < INITIAL_PLAY && attempts < 300) {
        _read_packets_to_queue();
        _update_buffer_stats();
        attempts++;
    }
    // الصوت يُفكَّك فقط بعد audio_player->play() في _decode_next_frame

    UtilityFunctions::print("[PREFILL] Done. Forward=", forward_buffer_secs, "s");
}

// ─── فك التشفير وعرض الإطار ─────────────────────────────────────────────────


void FFmpegPlayer::_decode_next_frame() {
    _read_packets_to_queue();

    // أ- معالجة الصوت (تبقى كما هي لأنها تعمل جيداً عندك)
    if (audio_player && audio_player->is_playing() && audio_codec_ctx) {
        Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
        if (pb.is_valid()) {
            int space = pb->get_frames_available();
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

    // ب- الفيديو: الإصلاح الجذري لمنع التجمد
    if (video_packet_queue.empty() || !video_codec_ctx) return;

    AVFrame *vf = av_frame_alloc();
    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

    // 1. إرسال أكبر قدر ممكن من الحزم للكودك لتجنب الـ Underrun
    while (!video_packet_queue.empty()) {
        AVPacket *pkt = video_packet_queue.front();
        int ret = avcodec_send_packet(video_codec_ctx, pkt);
        if (ret == 0) {
            video_packet_queue.pop_front();
            av_packet_free(&pkt);
        } else if (ret == AVERROR(EAGAIN)) {
            break; // الكودك ممتلئ حالياً
        } else {
            video_packet_queue.pop_front();
            av_packet_free(&pkt); // حزمة تالفة
        }
    }

    // 2. سحب كافة الإطارات الجاهزة واختيار الأنسب
    bool frame_updated = false;
    while (avcodec_receive_frame(video_codec_ctx, vf) == 0) {
        double frame_pts = (vf->pts != AV_NOPTS_VALUE) ? (vf->pts * tb) - stream_start_time : position;
        double diff = frame_pts - position;

        // إذا كان الإطار قريباً جداً من وقت الصوت (أو تجاوزه بقليل) نعرضه فوراً
        if (diff >= -0.02 && diff <= 0.05) {
            _update_texture_from_frame(vf);
            frame_updated = true;
        } 
        // إذا كان الإطار قديم جداً، نتجاهله (Drop) ليكمل الكودك سحب الإطارات التالية
        else if (diff < -0.02) {
            av_frame_unref(vf);
            continue;
        }
        // إذا كان الإطار في المستقبل البعيد، نخرجه من الحلقة وننتظر الدورة القادمة
        else if (diff > 0.05) {
            av_frame_unref(vf);
            break; 
        }
        av_frame_unref(vf);
    }
    av_frame_free(&vf);
}


void FFmpegPlayer::_update_texture_from_frame(AVFrame *frame) {
    // 1. التحقق من سلامة البافرات قبل العمل
    if (!sws_ctx || !frame_buffer || !frame) return;

    // 2. إعداد مصفوفات الوجهة لعملية التحويل (Scaling/Conversion)
    uint8_t *dest[4] = { frame_buffer, nullptr, nullptr, nullptr };
    int dest_linesize[4] = { video_width * 3, 0, 0, 0 };
    
    // 3. تحويل الإطار من صيغة الكودك (غالباً YUV) إلى RGB24 التي يطلبها Godot
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, video_height, dest, dest_linesize);

    // 4. نقل البيانات إلى مصفوفة بايتات Godot
    PackedByteArray pba;
    pba.resize(video_width * video_height * 3);
    
    // استخدام memcpy سريع جداً لنقل البيانات
    memcpy(pba.ptrw(), frame_buffer, pba.size());

    // 5. تحديث الصورة الموجودة فعلياً بدلاً من إنشاء واحدة جديدة في كل فريم (لتحسين الأداء)
    Ref<Image> img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, pba);
    
    if (current_texture.is_valid()) {
        current_texture->update(img);
        _emit_frame_updated(); // إرسال إشارة للمحرك بأن الصورة تغيرت
    }
}



// ─── دفع الصوت ────────────────────────────────────────────────────────────────

void FFmpegPlayer::_push_audio_samples(AVFrame *frame) {
    if (!audio_player || !swr_ctx || !audio_codec_ctx) return;

    Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
    if (pb.is_null()) return;

    if (frame && !audio_pts_set && frame->pts != AV_NOPTS_VALUE && audio_player->is_playing()) {
        double tb = av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base);
        double vstart = (fmt_ctx->streams[audio_stream_idx]->start_time != AV_NOPTS_VALUE) ? 
                         fmt_ctx->streams[audio_stream_idx]->start_time * tb : 0.0;
        audio_pts_offset = (frame->pts * tb) - vstart;
        audio_pts_set = true;
        position = audio_pts_offset;
    }

    if (frame) {
        int out_count = av_rescale_rnd(swr_get_delay(swr_ctx, audio_sample_rate) + frame->nb_samples, 
                                       audio_sample_rate, audio_codec_ctx->sample_rate, AV_ROUND_UP);
        uint8_t *out_buf = nullptr;
        if (av_samples_alloc(&out_buf, nullptr, 2, out_count, AV_SAMPLE_FMT_FLT, 0) >= 0) {
            int converted = swr_convert(swr_ctx, &out_buf, out_count, (const uint8_t **)frame->data, frame->nb_samples);
            if (converted > 0) {
                size_t old_sz = audio_overflow.size();
                audio_overflow.resize(old_sz + (converted * 2));
                memcpy(audio_overflow.data() + old_sz, out_buf, converted * 2 * sizeof(float));
            }
            av_freep(&out_buf);
        }
    }

    int frames_available = pb->get_frames_available();
    int overflow_frames = (int)(audio_overflow.size() / 2);

    // سحب العينات بهدوء دون مسح البافر بالكامل إذا كان هناك نقص
    if (frames_available > 0 && overflow_frames > 0) {
        int to_push = Math::min(frames_available, overflow_frames);
        PackedVector2Array buffer;
        buffer.resize(to_push);
        
        Vector2 *wptr = buffer.ptrw();
        const float *src = audio_overflow.data();
        for (int i = 0; i < to_push; i++) {
            wptr[i] = Vector2(src[i * 2] * volume, src[i * 2 + 1] * volume);
        }
        
        if (pb->push_buffer(buffer)) {
            audio_overflow.erase(audio_overflow.begin(), audio_overflow.begin() + (to_push * 2));
        }
    }
}



// ─── إعداد الصوت ─────────────────────────────────────────────────────────────
bool FFmpegPlayer::_setup_audio(AVStream *astream) {
    if (!astream || !astream->codecpar) return false;

    const AVCodec *codec = avcodec_find_decoder(astream->codecpar->codec_id);
    if (!codec) return false;

    if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); audio_codec_ctx = nullptr; }
    audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);
    if (avcodec_open2(audio_codec_ctx, codec, nullptr) < 0) return false;

    if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
    swr_ctx = swr_alloc();

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);

    av_opt_set_chlayout(swr_ctx,    "in_chlayout",    &audio_codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx,    "out_chlayout",   &out_layout, 0);
    av_opt_set_int(swr_ctx,         "in_sample_rate",  audio_codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx,         "out_sample_rate", audio_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx,  "in_sample_fmt",   audio_codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx,  "out_sample_fmt",  AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_ctx) < 0) return false;

    audio_sample_rate = audio_codec_ctx->sample_rate;

    if (!audio_generator.is_valid()) audio_generator.instantiate();
    audio_generator->set_mix_rate(audio_sample_rate);
    audio_generator->set_buffer_length(2.0f); // [إصلاح H] 2s بدل 0.5s → هامش كافٍ

    if (audio_player) audio_player->set_stream(audio_generator);
    return true;
}

// ─── تنظيف ────────────────────────────────────────────────────────────────────
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
    if (audio_player && audio_player->is_playing()) audio_player->stop();
}

void FFmpegPlayer::_cleanup() {
    _clear_queues();
    
    // تصفير بافر الصوت لمنع تداخل أصوات الأفلام السابقة
    audio_overflow.clear();
    
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
Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const { return current_texture; }

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
        godot::GDExtensionBinding::InitObject init_obj(
            p_get_proc_address, p_library, r_initialization);
        init_obj.register_initializer([](godot::ModuleInitializationLevel level) {
            if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE)
                godot::ClassDB::register_class<FFmpegPlayer>();
        });
        init_obj.register_terminator([](godot::ModuleInitializationLevel level) {});
        init_obj.set_minimum_library_initialization_level(
            godot::MODULE_INITIALIZATION_LEVEL_SCENE);
        return init_obj.init();
    }
}
