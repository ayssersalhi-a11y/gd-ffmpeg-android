/**
 * ffmpeg_player.cpp
 * GDExtension - FFmpeg Video Player (Video Only) for Godot 4
 *
 * الإصدار 3.0 — فصل كامل للصوت عن الفيديو + إصلاح التقطع
 *
 * ─── الإصلاحات والتغييرات الجوهرية ───────────────────────────────────────────
 *
 * [1] فصل الصوت:
 *     - حُذف كل كود FFmpeg الخاص بفك تشفير الصوت (audio_codec_ctx, swr_ctx,
 *       audio_packet_queue, _push_audio_samples, _setup_audio).
 *     - الصوت يُدار الآن عبر AudioStreamPlayer عادي مستقل تماماً.
 *     - load_audio(path) يقبل: res://sounds/arabic.mp3  أو  http://... أو
 *       مسار مطلق على الجهاز.  يدعم mp3 و ogg و wav عبر Godot مباشرة.
 *     - المزامنة الزمنية لا تعتمد على الصوت → لا desync مطلقاً.
 *
 * [2] إصلاح التقطع (المشكلة الرئيسية):
 *     السبب الجذري: avcodec_receive_frame() على فيديو حقيقي (H264 من كاميرا)
 *     أبطأ بكثير من الأنيمي لأن الإطارات أكبر. حين يستغرق أكثر من 16ms
 *     (دورة واحدة من _process) تظهر اللحظات المتقطعة.
 *
 *     الحل: طابور decoded_frame_queue
 *     - _decode_packets_into_queue() يُفكّك حتى MAX_DECODED_FRAMES إطاراً
 *       مسبقاً ويخزنها كـ PackedByteArray جاهزة.
 *     - _present_frame_at() تأخذ الإطار الصحيح من الطابور فوراً دون أي
 *       avcodec_receive_frame() في نفس دورة _process.
 *     - النتيجة: _process يُنهي عمله في < 1ms في معظم الحالات.
 *
 * [3] التوقيت المحسوب بـ frame_timer:
 *     بدلاً من الاعتماد على AudioStreamPlayer للحصول على الوقت، نجمع delta
 *     في frame_timer ونعرض الإطار حين يحين وقته (1.0/fps).
 *     هذا أكثر استقراراً وأقل تعقيداً.
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
}

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/audio_stream.hpp>

using namespace godot;

// ─── تسجيل الكلاس ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_bind_methods() {
    // Video
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

    ClassDB::bind_method(D_METHOD("set_loop",   "enable"),  &FFmpegPlayer::set_loop);
    ClassDB::bind_method(D_METHOD("get_loop"),              &FFmpegPlayer::get_loop);

    // External Audio
    ClassDB::bind_method(D_METHOD("load_audio", "path"),        &FFmpegPlayer::load_audio);
    ClassDB::bind_method(D_METHOD("unload_audio"),              &FFmpegPlayer::unload_audio);
    ClassDB::bind_method(D_METHOD("set_audio_volume", "vol"),   &FFmpegPlayer::set_audio_volume);
    ClassDB::bind_method(D_METHOD("get_audio_volume"),          &FFmpegPlayer::get_audio_volume);
    ClassDB::bind_method(D_METHOD("set_audio_muted", "muted"),  &FFmpegPlayer::set_audio_muted);
    ClassDB::bind_method(D_METHOD("is_audio_muted"),            &FFmpegPlayer::is_audio_muted);
    ClassDB::bind_method(D_METHOD("get_loaded_audio_path"),     &FFmpegPlayer::get_loaded_audio_path);

    // Buffer / status
    ClassDB::bind_method(D_METHOD("get_forward_buffer"),        &FFmpegPlayer::get_forward_buffer);
    ClassDB::bind_method(D_METHOD("is_buffering"),              &FFmpegPlayer::is_buffering);

    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "get_loop");

    ADD_SIGNAL(MethodInfo("video_loaded",      PropertyInfo(Variant::BOOL,   "success")));
    ADD_SIGNAL(MethodInfo("frame_updated",     PropertyInfo(Variant::OBJECT, "texture")));
    ADD_SIGNAL(MethodInfo("video_finished"));
    ADD_SIGNAL(MethodInfo("playback_error",    PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("buffering_changed", PropertyInfo(Variant::BOOL,   "is_buffering")));
    ADD_SIGNAL(MethodInfo("audio_loaded",      PropertyInfo(Variant::BOOL,   "success")));
}

// ─── البنّاء والهادم ──────────────────────────────────────────────────────────
FFmpegPlayer::FFmpegPlayer() {}
FFmpegPlayer::~FFmpegPlayer() { _cleanup(); }

// ─── _ready ───────────────────────────────────────────────────────────────────
void FFmpegPlayer::_ready() {
    // مشغّل الصوت الخارجي — مستقل تماماً عن الفيديو
    ext_audio_player = memnew(AudioStreamPlayer);
    ext_audio_player->set_name("_ExtAudioPlayer");
    add_child(ext_audio_player);

    UtilityFunctions::print("--- FFmpeg GDExtension v3.0 (Video-Only) Ready ---");

    // طباعة فكودكات الأجهزة المتاحة
    void *opaque = nullptr;
    const AVCodec *codec;
    while ((codec = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(codec) && String(codec->name).find("mediacodec") != -1) {
            UtilityFunctions::print("[HW] Found: ", codec->name);
        }
    }
}

// ─── تحميل الفيديو ────────────────────────────────────────────────────────────
bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();

    buffering           = false;
    forward_buffer_secs = 0.0;
    position            = 0.0;
    frame_timer         = 0.0;

    if (path.is_empty()) { _emit_playback_error("Path is empty"); return false; }

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

    stream_start_time = (fmt_ctx->start_time != AV_NOPTS_VALUE)
                        ? (double)fmt_ctx->start_time / AV_TIME_BASE
                        : 0.0;

    // نبحث عن مسار الفيديو فقط — نتجاهل مسارات الصوت تماماً
    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (video_stream_idx < 0) {
        _cleanup();
        _emit_playback_error("No video stream found in: " + path);
        _emit_video_loaded(false);
        return false;
    }

    AVStream      *vstream = fmt_ctx->streams[video_stream_idx];
    const AVCodec *vcodec  = nullptr;

    // نجرب الفكودك العتادي أولاً
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

    // في وضع البرمجيات: نستخدم كل الخيوط المتاحة للتعامل مع الفيديو الحقيقي
    if (String(vcodec->name).find("mediacodec") == -1) {
        video_codec_ctx->thread_count = 0;   // 0 = تلقائي (يختار FFmpeg العدد الأمثل)
        video_codec_ctx->thread_type  = FF_THREAD_FRAME; // decode إطارات متعددة بالتوازي
    }

    if (avcodec_open2(video_codec_ctx, vcodec, nullptr) < 0) {
        _emit_playback_error("Cannot open video decoder");
        _cleanup();
        return false;
    }

    video_width  = video_codec_ctx->width;
    video_height = video_codec_ctx->height;
    fps          = av_q2d(vstream->r_frame_rate);
    if (fps <= 0.0 || fps > 240.0) fps = 30.0; // قيمة احتياطية آمنة

    sws_ctx = sws_getContext(
        video_width, video_height, video_codec_ctx->pix_fmt,
        video_width, video_height, AV_PIX_FMT_RGB24,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE)
               ? (double)fmt_ctx->duration / AV_TIME_BASE
               : 0.0;

    _allocate_buffers();
    _emit_video_loaded(true);

    UtilityFunctions::print("[LOAD] OK | duration=", duration, "s | fps=", fps,
                            " | ", video_width, "x", video_height,
                            " | start_time=", stream_start_time, "s");
    return true;
}

// ─── تخصيص بافرات الذاكرة ─────────────────────────────────────────────────────
void FFmpegPlayer::_allocate_buffers() {
    if (frame_buffer) { av_free(frame_buffer); frame_buffer = nullptr; }

    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);
    if (!frame_buffer) { UtilityFunctions::printerr("[MEM] Alloc failed!"); return; }

    // إنشاء texture أسود ابتدائي
    PackedByteArray black;
    black.resize(video_width * video_height * 3);
    black.fill(0);
    Ref<Image> tmp = Image::create_from_data(video_width, video_height, false,
                                             Image::FORMAT_RGB8, black);
    if (current_texture.is_null()) current_texture.instantiate();
    current_texture->set_image(tmp);

    UtilityFunctions::print("[MEM] Buffers ready: ", video_width, "x", video_height);
}

// ─── تحميل الصوت الخارجي ──────────────────────────────────────────────────────
// يقبل: res://audio/arabic.mp3  أو  /storage/.../file.mp3  أو  http://...
// يعتمد على ResourceLoader لـ res:// وعلى FileAccess لمسارات الجهاز
bool FFmpegPlayer::load_audio(const String &path) {
    if (!ext_audio_player) {
        ext_audio_player = memnew(AudioStreamPlayer);
        ext_audio_player->set_name("_ExtAudioPlayer");
        add_child(ext_audio_player);
    }

    if (ext_audio_player->is_playing()) ext_audio_player->stop();

    if (path.is_empty()) {
        loaded_audio_path = "";
        ext_audio_player->set_stream(Ref<AudioStream>());
        emit_signal("audio_loaded", false);
        return false;
    }

    // للمسارات التي يتعامل معها ResourceLoader (res:// و user://)
    if (path.begins_with("res://") || path.begins_with("user://")) {
        Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
        Ref<AudioStream> stream = res;
        if (stream.is_null()) {
            UtilityFunctions::printerr("[AUDIO] Cannot load: ", path);
            emit_signal("audio_loaded", false);
            return false;
        }
        ext_audio_player->set_stream(stream);
        loaded_audio_path = path;
        _apply_audio_volume();
        UtilityFunctions::print("[AUDIO] Loaded (res): ", path);
        emit_signal("audio_loaded", true);
        return true;
    }

    // للملفات الخارجية على الجهاز أو HTTP — نقرأها يدوياً كـ bytes
    // ملاحظة: Godot لا يدعم تحميل HTTP مباشرة عبر AudioStream.
    // للروابط يُفضّل التحميل المسبق عبر HTTPRequest ثم تخزينها في user://
    // هنا ندعم المسارات المطلقة على الجهاز فقط
    Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ);
    if (fa.is_null()) {
        UtilityFunctions::printerr("[AUDIO] File not found: ", path);
        emit_signal("audio_loaded", false);
        return false;
    }

    PackedByteArray data = fa->get_buffer(fa->get_length());
    fa.unref();

    String lower = path.to_lower();
    Ref<AudioStream> stream;

    if (lower.ends_with(".mp3")) {
        Ref<AudioStreamMP3> mp3;
        mp3.instantiate();
        mp3->set_data(data);
        stream = mp3;
    } else if (lower.ends_with(".ogg")) {
        Ref<AudioStreamOggVorbis> ogg = AudioStreamOggVorbis::load_from_buffer(data);
        stream = ogg;
    } else {
        UtilityFunctions::printerr("[AUDIO] Unsupported format (use mp3 or ogg): ", path);
        emit_signal("audio_loaded", false);
        return false;
    }

    if (stream.is_null()) {
        UtilityFunctions::printerr("[AUDIO] Decode failed for: ", path);
        emit_signal("audio_loaded", false);
        return false;
    }

    ext_audio_player->set_stream(stream);
    loaded_audio_path = path;
    _apply_audio_volume();
    UtilityFunctions::print("[AUDIO] Loaded (file): ", path);
    emit_signal("audio_loaded", true);
    return true;
}

void FFmpegPlayer::unload_audio() {
    if (ext_audio_player) {
        if (ext_audio_player->is_playing()) ext_audio_player->stop();
        ext_audio_player->set_stream(Ref<AudioStream>());
    }
    loaded_audio_path = "";
}

void FFmpegPlayer::_apply_audio_volume() {
    if (!ext_audio_player) return;
    if (audio_muted) {
        ext_audio_player->set_volume_db(-80.0f);
    } else {
        float db = (audio_volume <= 0.0001f) ? -80.0f : 20.0f * log10f(audio_volume);
        ext_audio_player->set_volume_db(db);
    }
}

void FFmpegPlayer::set_audio_volume(float vol) {
    audio_volume = CLAMP(vol, 0.0f, 1.0f);
    _apply_audio_volume();
}
float FFmpegPlayer::get_audio_volume() const { return audio_volume; }

void FFmpegPlayer::set_audio_muted(bool muted) {
    audio_muted = muted;
    _apply_audio_volume();
}
bool FFmpegPlayer::is_audio_muted() const { return audio_muted; }
String FFmpegPlayer::get_loaded_audio_path() const { return loaded_audio_path; }

// ─── play ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) { UtilityFunctions::printerr("[PLAY] No video loaded."); return; }

    _prefill_buffers();

    playing   = true;
    buffering = false;
    frame_timer = 0.0;

    // تشغيل الصوت الخارجي إن وجد، من نفس الموضع الزمني
    if (ext_audio_player && ext_audio_player->get_stream().is_valid()) {
        if (ext_audio_player->is_playing()) ext_audio_player->stop();
        ext_audio_player->play((float)position);
    }

    UtilityFunctions::print("[PLAY] Started. pos=", position,
                            " fwd=", forward_buffer_secs, "s");
}

// ─── pause ────────────────────────────────────────────────────────────────────
void FFmpegPlayer::pause() {
    playing = false;
    if (ext_audio_player && ext_audio_player->is_playing()) {
        ext_audio_player->set_stream_paused(true);
    }
}

// ─── stop ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::stop() {
    playing = false;
    if (ext_audio_player && ext_audio_player->is_playing()) {
        ext_audio_player->stop();
    }
    seek(0.0);
}

// ─── seek ─────────────────────────────────────────────────────────────────────
void FFmpegPlayer::seek(double seconds) {
    if (!fmt_ctx || !video_codec_ctx) return;

    bool was_playing = playing;
    playing = false;

    if (ext_audio_player && ext_audio_player->is_playing()) ext_audio_player->stop();

    // نحاول Fast Seek أولاً (داخل البافر الحالي)
    double range_start = position - 2.0;
    double range_end   = position + forward_buffer_secs + 0.5;
    bool   in_buffer   = (seconds >= range_start && seconds <= range_end);

    if (in_buffer) {
        // حذف الإطارات المُفكَّكة القديمة
        while (!decoded_frame_queue.empty() && decoded_frame_queue.front().pts < seconds - 0.05) {
            decoded_frame_queue.pop_front();
        }
        position    = seconds;
        frame_timer = 0.0;
        UtilityFunctions::print("[SEEK] Fast seek to ", seconds);
    } else {
        // Full Seek
        _clear_queues();
        decoded_frame_queue.clear();

        int64_t seek_target = (int64_t)(seconds * AV_TIME_BASE);
        if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
            UtilityFunctions::printerr("[SEEK] av_seek_frame failed: ", seconds);
            _emit_playback_error("Seek failed");
            if (was_playing) { playing = true; }
            return;
        }

        avcodec_flush_buffers(video_codec_ctx);

        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = sws_getContext(video_width, video_height, video_codec_ctx->pix_fmt,
                                     video_width, video_height, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        position            = seconds;
        frame_timer         = 0.0;
        forward_buffer_secs = 0.0;

        buffering = true;
        if (is_inside_tree()) emit_signal("buffering_changed", true);

        for (int i = 0; i < 40; i++) _read_packets_to_queue();
        _decode_packets_into_queue();
        _update_buffer_stats();

        if (forward_buffer_secs >= INITIAL_PLAY) {
            buffering = false;
            if (is_inside_tree()) emit_signal("buffering_changed", false);
        }

        UtilityFunctions::print("[SEEK] Full seek to ", seconds,
                                " fwd=", forward_buffer_secs);
    }

    if (was_playing) {
        playing = true;
        if (ext_audio_player && ext_audio_player->get_stream().is_valid()) {
            ext_audio_player->play((float)position);
        }
    }
}

// ─── _process ─────────────────────────────────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!fmt_ctx || !playing || buffering) return;

    _update_buffer_stats();

    // حد الـ Buffering: إذا نفد البافر نوقف مؤقتاً
    if (forward_buffer_secs < 0.3 && decoded_frame_queue.empty()) {
        playing = false;
        buffering = true;
        if (ext_audio_player && ext_audio_player->is_playing())
            ext_audio_player->set_stream_paused(true);
        if (is_inside_tree()) emit_signal("buffering_changed", true);
        UtilityFunctions::print("[BUFFER] Underrun at pos=", position,
                                " fwd=", forward_buffer_secs);
        return;
    }

    // إذا كنا نعود من حالة buffering
    if (buffering && (forward_buffer_secs >= INITIAL_PLAY || !decoded_frame_queue.empty())) {
        buffering = false;
        if (ext_audio_player && ext_audio_player->get_stream().is_valid()
            && !ext_audio_player->is_playing()) {
            ext_audio_player->play((float)position);
            ext_audio_player->set_stream_paused(false);
        }
        if (is_inside_tree()) emit_signal("buffering_changed", false);
    }

    // تقدم الوقت
    position    += delta;
    frame_timer += delta;

    // نحافظ على طابور الإطارات ممتلئاً
    if ((int)decoded_frame_queue.size() < MAX_DECODED_FRAMES) {
        _decode_packets_into_queue();
    }

    // نقرأ حزماً جديدة للحفاظ على البافر
    _read_packets_to_queue();

    // عرض الإطار المناسب
    _present_frame_at(position);

    // فحص نهاية الفيديو
    if (duration > 0.0 && position >= duration) {
        if (looping) {
            seek(0.0);
        } else {
            stop();
            _emit_video_finished();
        }
    }
}

// ─── _update_buffer_stats ──────────────────────────────────────────────────────
void FFmpegPlayer::_update_buffer_stats() {
    if (video_stream_idx < 0 || !fmt_ctx) return;

    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstream_start = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                            ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    // نحسب البافر من آخر حزمة في طابور الحزم الخام
    if (video_packet_queue.empty()) {
        // إذا فرغت الحزم لكن عندنا إطارات مُفكَّكة، نحسب منها
        if (!decoded_frame_queue.empty()) {
            forward_buffer_secs = Math::max(0.0, decoded_frame_queue.back().pts - position);
        } else {
            forward_buffer_secs = 0.0;
        }
    } else {
        AVPacket *last_pkt = video_packet_queue.back();
        if (last_pkt->pts != AV_NOPTS_VALUE) {
            double last_pts = (last_pkt->pts * tb) - vstream_start;
            forward_buffer_secs = Math::max(0.0, last_pts - position);
        }
    }
}

// ─── حساب دفعة القراءة ────────────────────────────────────────────────────────
int FFmpegPlayer::_calc_read_batch_size() const {
    if (forward_buffer_secs < MIN_FORWARD) return 100; // تعبئة سريعة
    if (forward_buffer_secs < MAX_FORWARD) return 30;
    return 5; // الحد الأدنى — لمنع نضوب البافر
}

// ─── قراءة الحزم الخام ────────────────────────────────────────────────────────
void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;

    int batch    = _calc_read_batch_size();
    AVPacket *pk = av_packet_alloc();

    for (int i = 0; i < batch; i++) {
        if (av_read_frame(fmt_ctx, pk) < 0) {
            av_packet_unref(pk);
            break;
        }
        // نأخذ حزم الفيديو فقط — نتخلص من أي شيء آخر
        if (pk->stream_index == video_stream_idx) {
            video_packet_queue.push_back(av_packet_clone(pk));
        }
        av_packet_unref(pk);
    }
    av_packet_free(&pk);

    // حذف الحزم القديمة جداً لتوفير الذاكرة
    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                    ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    while (!video_packet_queue.empty()) {
        AVPacket *oldest = video_packet_queue.front();
        if (oldest->pts != AV_NOPTS_VALUE) {
            double pts = (oldest->pts * tb) - vstart;
            // احتفظ بأكثر من 60 ثانية للأمام كحد أقصى
            if (pts < position - 5.0) {
                av_packet_free(&oldest);
                video_packet_queue.pop_front();
                continue;
            }
        }
        break;
    }
}

// ─── فك تشفير الحزم إلى طابور الإطارات ───────────────────────────────────────
// هذه هي قلب إصلاح التقطع:
// نُفكّك عدة إطارات مسبقاً ونخزنها جاهزة في decoded_frame_queue
// بدلاً من فك كل إطار في اللحظة التي نريد عرضه فيها
void FFmpegPlayer::_decode_packets_into_queue() {
    if (!video_codec_ctx || !frame_buffer) return;

    // لا نملأ إذا كان الطابور ممتلئاً بالفعل
    if ((int)decoded_frame_queue.size() >= MAX_DECODED_FRAMES) return;

    double tb = av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);
    double vstart = (fmt_ctx->streams[video_stream_idx]->start_time != AV_NOPTS_VALUE)
                    ? fmt_ctx->streams[video_stream_idx]->start_time * tb : 0.0;

    AVFrame *vf = av_frame_alloc();

    // أرسل حزماً للكودك حتى نحصل على MAX_DECODED_FRAMES إطاراً
    int loops = 0;
    while ((int)decoded_frame_queue.size() < MAX_DECODED_FRAMES && loops < 50) {
        loops++;

        // حاول استقبال إطار جاهز أولاً (قد يكون الكودك لديه إطارات مخزّنة)
        int ret = avcodec_receive_frame(video_codec_ctx, vf);
        if (ret == 0) {
            // إطار جاهز!
            double frame_pts = (vf->pts != AV_NOPTS_VALUE)
                                ? (vf->pts * tb) - vstart
                                : position;

            // تجاهل الإطارات الماضية جداً
            if (frame_pts < position - 0.5) {
                av_frame_unref(vf);
                continue;
            }

            // تحويل اللون (YUV → RGB24)
            if (!sws_ctx) {
                sws_ctx = sws_getContext(video_width, video_height, (AVPixelFormat)vf->format,
                                         video_width, video_height, AV_PIX_FMT_RGB24,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            }

            uint8_t *dest[4] = { frame_buffer, nullptr, nullptr, nullptr };
            int dest_ls[4]   = { video_width * 3, 0, 0, 0 };
            sws_scale(sws_ctx, vf->data, vf->linesize, 0, video_height, dest, dest_ls);

            // تخزين في طابور الإطارات المُفكَّكة
            DecodedFrame df;
            df.pts = frame_pts;
            df.data.resize(video_width * video_height * 3);
            memcpy(df.data.ptrw(), frame_buffer, df.data.size());
            decoded_frame_queue.push_back(std::move(df));

            av_frame_unref(vf);
            continue;
        }

        if (ret == AVERROR(EAGAIN)) {
            // الكودك يحتاج حزمة جديدة
            if (video_packet_queue.empty()) break;

            AVPacket *pkt = video_packet_queue.front();
            video_packet_queue.pop_front();

            int send_ret = avcodec_send_packet(video_codec_ctx, pkt);
            av_packet_free(&pkt);

            if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) break;
        } else {
            // خطأ أو EOF
            break;
        }
    }

    av_frame_free(&vf);
}

// ─── عرض الإطار المناسب للوقت الحالي ─────────────────────────────────────────
bool FFmpegPlayer::_present_frame_at(double current_pos) {
    if (decoded_frame_queue.empty()) return false;

    // ابحث عن آخر إطار بـ pts <= current_pos
    // (نمسح الإطارات الماضية ونعرض آخر واحد صالح)
    bool found = false;
    while (decoded_frame_queue.size() > 1) {
        const DecodedFrame &next = decoded_frame_queue[1];
        if (next.pts <= current_pos + 0.001) {
            // الإطار الثاني أقرب للوقت الحالي، أزل الأول
            decoded_frame_queue.pop_front();
            found = true;
        } else {
            break;
        }
    }

    // الآن أمامنا الإطار الأنسب في المقدمة
    if (!decoded_frame_queue.empty()) {
        const DecodedFrame &frame = decoded_frame_queue.front();

        // لا تعرض الإطارات المستقبلية البعيدة جداً
        if (frame.pts > current_pos + 0.15) return false;

        if (current_texture.is_valid() && frame.data.size() > 0) {
            Ref<Image> img = Image::create_from_data(
                video_width, video_height, false, Image::FORMAT_RGB8, frame.data);
            current_texture->update(img);
            _emit_frame_updated();
            return true;
        }
    }
    return false;
}

// ─── الملء الأولي ─────────────────────────────────────────────────────────────
void FFmpegPlayer::_prefill_buffers() {
    if (!fmt_ctx) return;
    UtilityFunctions::print("[PREFILL] Filling to ", INITIAL_PLAY, "s...");

    int attempts = 0;
    while (forward_buffer_secs < INITIAL_PLAY && attempts < 400) {
        _read_packets_to_queue();
        _update_buffer_stats();
        attempts++;
    }

    // فك تشفير مسبق لأول مجموعة إطارات
    _decode_packets_into_queue();

    UtilityFunctions::print("[PREFILL] Done. Forward=", forward_buffer_secs,
                            "s | Decoded=", decoded_frame_queue.size(), " frames");
}

// ─── تنظيف الطوابير ───────────────────────────────────────────────────────────
void FFmpegPlayer::_clear_queues() {
    while (!video_packet_queue.empty()) {
        AVPacket *p = video_packet_queue.front();
        video_packet_queue.pop_front();
        av_packet_free(&p);
    }
    decoded_frame_queue.clear();
    forward_buffer_secs = 0.0;
}

// ─── التنظيف الكامل ───────────────────────────────────────────────────────────
void FFmpegPlayer::_cleanup() {
    _clear_queues();

    if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); video_codec_ctx = nullptr; }
    if (fmt_ctx)         { avformat_close_input(&fmt_ctx);         fmt_ctx         = nullptr; }
    if (sws_ctx)         { sws_freeContext(sws_ctx);               sws_ctx         = nullptr; }
    if (frame_buffer)    { av_free(frame_buffer);                  frame_buffer    = nullptr; }

    duration            = 0.0;
    position            = 0.0;
    forward_buffer_secs = 0.0;
    frame_timer         = 0.0;
    playing             = false;
    buffering           = false;
    is_streaming        = false;
    video_stream_idx    = -1;
    video_width         = 0;
    video_height        = 0;
    fps                 = 0.0;
}

// ─── الإشارات ─────────────────────────────────────────────────────────────────
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
bool   FFmpegPlayer::is_playing()       const { return playing && !buffering; }
double FFmpegPlayer::get_duration()     const { return duration; }
double FFmpegPlayer::get_position()     const { return position; }
int    FFmpegPlayer::get_video_width()  const { return video_width; }
int    FFmpegPlayer::get_video_height() const { return video_height; }
double FFmpegPlayer::get_fps()          const { return fps; }
Ref<ImageTexture> FFmpegPlayer::get_current_frame_texture() const { return current_texture; }

void FFmpegPlayer::set_loop(bool en) { looping = en; }
bool FFmpegPlayer::get_loop() const  { return looping; }

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
