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
    // تهيئة الصوت
    audio_player = memnew(AudioStreamPlayer);
    audio_player->set_name("_AudioPlayer");
    add_child(audio_player);

    // --- نظام فحص ميزات المكتبة المتاحة ---
    UtilityFunctions::print("--- FFmpeg Capabilities Check ---");
    void *opaque = nullptr;
    const AVCodec *codec;
    bool has_mediacodec = false;

    while ((codec = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(codec)) {
            String name = String(codec->name);
            if (name.find("mediacodec") != -1) {
                UtilityFunctions::print("[FOUND] Hardware Decoder: ", name);
                has_mediacodec = true;
            }
        }
    }

    if (!has_mediacodec) {
        UtilityFunctions::printerr("[CRITICAL] MediaCodec support NOT found in current FFmpeg build!");
    }
    UtilityFunctions::print("---------------------------------");
}

// ─── تحميل الفيديو ───────────────────────────────────────────────────────────

bool FFmpegPlayer::load_video(const String &path) {
    _cleanup();
    
    if (path.is_empty()) {
        UtilityFunctions::printerr("[LOAD] Error: Path is empty!");
        return false;
    }

    UtilityFunctions::print("[LOAD] Searching for Hardware Decoders for: ", path);

    if (!audio_player) {
        audio_player = memnew(AudioStreamPlayer);
        add_child(audio_player);
    }

    String real_path = ProjectSettings::get_singleton()->globalize_path(path);
    CharString utf8_path = real_path.utf8();
    const char *c_path = utf8_path.get_data();

    if (avformat_open_input(&fmt_ctx, c_path, nullptr, nullptr) < 0) {
        _emit_video_loaded(false);
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        _cleanup();
        return false;
    }

    video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_idx >= 0) {
        AVStream *vstream = fmt_ctx->streams[video_stream_idx];
        const AVCodec *vcodec = nullptr;

        // --- محاولة صيد مشفر MediaCodec العتادي ---
        if (vstream->codecpar->codec_id == AV_CODEC_ID_H264) {
            vcodec = avcodec_find_decoder_by_name("h264_mediacodec");
        } else if (vstream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            vcodec = avcodec_find_decoder_by_name("hevc_mediacodec");
        }

        // صمام أمان: إذا لم نجد العتاد، نستخدم المشفر البرمجي الافتراضي
        if (!vcodec) {
            vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
            UtilityFunctions::print("[VIDEO] Mode: SOFTWARE (Expect more heat).");
        } else {
            UtilityFunctions::print("[VIDEO] Mode: HARDWARE (MediaCodec - Stay cool).");
        }

        if (vcodec) {
            video_codec_ctx = avcodec_alloc_context3(vcodec);
            avcodec_parameters_to_context(video_codec_ctx, vstream->codecpar);
            
            // تفعيل تعدد الأنوية فقط في حال المشفر البرمجي
            if (String(vcodec->name).find("mediacodec") == -1) {
                video_codec_ctx->thread_count = 0; 
            }

            if (avcodec_open2(video_codec_ctx, vcodec, nullptr) >= 0) {
                video_width = video_codec_ctx->width;
                video_height = video_codec_ctx->height;
                fps = av_q2d(vstream->r_frame_rate);
                
                // تحسين: SWS_FAST_BILINEAR لسرعة معالجة البكسلات
                sws_ctx = sws_getContext(video_width, video_height, video_codec_ctx->pix_fmt,
                                         video_width, video_height, AV_PIX_FMT_RGB24,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                _allocate_buffers();
            }
        }
    }

    if (audio_stream_idx >= 0) {
        _setup_audio(fmt_ctx->streams[audio_stream_idx]);
    }

    duration = (fmt_ctx->duration != AV_NOPTS_VALUE) ? (double)fmt_ctx->duration / AV_TIME_BASE : 0.0;
    _emit_video_loaded(true);
    return true;
}



void FFmpegPlayer::_allocate_buffers() {
    // 1. تنظيف الذاكرة القديمة (RAM) لمنع التسريب في Realme C33
    if (frame_buffer) {
        av_free(frame_buffer);
        frame_buffer = nullptr;
    }

    // 2. حجز بفر جديد لبيانات الـ RGB24 الخام
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, video_width, video_height, 1);
    frame_buffer = (uint8_t *)av_malloc(buf_size);
    
    if (!frame_buffer) {
        UtilityFunctions::printerr("[MEMORY] Critical: Failed to allocate frame buffer!");
        return;
    }

    // 3. تهيئة الصورة السوداء الأولية (Dummy Image)
    PackedByteArray black_data;
    black_data.resize(video_width * video_height * 3);
    black_data.fill(0); // تعبئة باللون الأسود

    Ref<Image> temp_img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, black_data);

    // 4. تهيئة التكستشر وحجزه في الـ GPU
    if (current_texture.is_null()) {
        current_texture.instantiate();
    }
    
    // المفتاح السحري: set_image تقوم بعمل Initialize للـ TextureResource
    current_texture->set_image(temp_img);
    
    UtilityFunctions::print("[MEMORY] Buffers allocated and Texture initialized: ", video_width, "x", video_height);
}



// ─── تشغيل / إيقاف / توقف ─────────────────────────────────────────────────
void FFmpegPlayer::play() {
    if (!fmt_ctx) {
        UtilityFunctions::printerr("[PLAY] No video loaded.");
        return;
    }
    playing = true;

    if (audio_player) {
        audio_player->set_stream_paused(false);
        if (!audio_player->is_playing()) {
            audio_player->play();
        }
    }
    UtilityFunctions::print("[PLAY] Started at position: ", position);
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
    if (!fmt_ctx || !video_codec_ctx) return;
    
    bool was_playing = playing;
    playing = false; 

    if (audio_player) audio_player->stop();
    
    // 1. مسح شامل للطوابير لضمان عدم وجود بيانات قديمة
    _clear_queues();

    // 2. حساب التوقيت بدقة بصيغة FFmpeg
    int64_t seek_target = (int64_t)(seconds * AV_TIME_BASE);
    
    // 3. القفز للإطار الرئيسي الأقرب (AVSEEK_FLAG_BACKWARD يضمن عدم تجاوز الوقت المطلوب)
    if (av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
        UtilityFunctions::printerr("[SEEK] Failed to seek to: ", seconds);
        return;
    }

    // 4. أهم خطوة للعتاد: تنظيف بفر المشفر (Flush) ليتخلص من الحالة القديمة
    avcodec_flush_buffers(video_codec_ctx);
    if (audio_codec_ctx) avcodec_flush_buffers(audio_codec_ctx);
    
    if (swr_ctx) { 
        swr_close(swr_ctx); 
        swr_init(swr_ctx); 
    }

    // 5. إعادة ضبط الساعة المرجعية
    position = seconds;
    audio_pts_offset = seconds; 
    audio_pts_set = false; 
    
    // 6. "حل مشكلة الصيغ العتادية المتغيرة":
    // نقوم بتصفير sws_ctx ليتم إعادة بنائه بناءً على أول إطار يصل بعد القفز
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr; 
    }

    // 7. تعبئة أولية سريعة للطابور قبل الاستئناف
    for (int i = 0; i < 15; i++) {
        _read_packets_to_queue();
    }

    if (was_playing) {
        playing = true;
        if (audio_player) audio_player->play();
    }

    UtilityFunctions::print("[SEEK] Hardware Sync Complete at: ", seconds);
}



// ─── _process: يُستدعى كل إطار من Godot ─────────────────────────────────────
void FFmpegPlayer::_process(double delta) {
    if (!playing || !fmt_ctx) return;

    // 1. تحديث الساعة المرجعية بدقة
    if (audio_stream_idx >= 0 && audio_player && audio_player->is_playing()) {
        // نستخدم الوقت الفعلي من AudioStreamPlayer لضمان التطابق مع ما يسمعه المستخدم
        double audio_time = audio_pts_offset + audio_player->get_playback_position();
        
        // منع القفزات المفاجئة في الوقت (تنعيم الساعة)
        if (Math::abs(audio_time - position) > 0.5) {
            position = audio_time; // قفزة كبيرة (مثلاً بعد Seek)
        } else {
            position = Math::lerp(position, audio_time, 0.5); // مزامنة ناعمة
        }
    } else {
        // في حال عدم وجود صوت، نعتمد على توقيت النظام (Delta)
        position += delta;
    }

    // 2. استدعاء فك التشفير للإطار التالي بناءً على الساعة الجديدة
    _decode_next_frame();

    // 3. التحقق من نهاية الفيديو أو التكرار
    if (duration > 0.0 && position >= duration) {
        if (looping) {
            seek(0.0);
        } else {
            stop();
            _emit_video_finished();
        }
    }
}




bool FFmpegPlayer::_setup_audio(AVStream *astream) {
    if (!astream || !astream->codecpar) return false;

    const AVCodec *codec = avcodec_find_decoder(astream->codecpar->codec_id);
    audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(audio_codec_ctx, astream->codecpar);
    
    if (avcodec_open2(audio_codec_ctx, codec, nullptr) < 0) return false;

    if (swr_ctx) swr_free(&swr_ctx);
    swr_ctx = swr_alloc();

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); 

    av_opt_set_chlayout(swr_ctx, "in_chlayout",  &audio_codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",    audio_codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",   audio_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt",  audio_codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_ctx) < 0) return false;

    audio_generator.instantiate();
    audio_generator->set_mix_rate(audio_codec_ctx->sample_rate);
    audio_generator->set_buffer_length(0.5); 

    if (audio_player) {
        audio_player->set_stream(audio_generator);
        // تم حذف audio_player->play() من هنا ❌
        UtilityFunctions::print("[AUDIO_DEBUG] Ready. Waiting for video buffer to start...");
    }

    audio_sample_rate = audio_codec_ctx->sample_rate;
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

void FFmpegPlayer::_read_packets_to_queue() {
    if (!fmt_ctx) return;
    
    // الحد الأقصى للطابور لمنع استهلاك الرام
    if ((video_packet_queue.size() + audio_packet_queue.size()) > MAX_QUEUE_SIZE) return;

    AVPacket *packet = av_packet_alloc();
    // نقرأ 32 حزمة في كل دورة لضمان سرعة الاستجابة
    for (int i = 0; i < 32; i++) {
        if (av_read_frame(fmt_ctx, packet) < 0) {
            av_packet_free(&packet);
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
}


void FFmpegPlayer::_clear_queues() {
    int v_size = video_packet_queue.size();
    int a_size = audio_packet_queue.size();

    // تنظيف طابور الفيديو
    while (!video_packet_queue.empty()) {
        AVPacket *pkt = video_packet_queue.front();
        video_packet_queue.pop_front();
        av_packet_free(&pkt);
    }

    // تنظيف طابور الصوت
    while (!audio_packet_queue.empty()) {
        AVPacket *pkt = audio_packet_queue.front();
        audio_packet_queue.pop_front();
        av_packet_free(&pkt);
    }

    UtilityFunctions::print("[QUEUE_CLEAN] Freed ", v_size, " video and ", a_size, " audio packets.");
}
void FFmpegPlayer::_clear_audio_buffers() {
    if (swr_ctx) {
        swr_close(swr_ctx);
        swr_init(swr_ctx);
    }
    
    if (audio_player) {
        audio_player->stop(); 
        // لا نقوم بعمل play هنا أبداً، نترك الـ _decode_next_frame تقرر متى يبدأ
    }
    
    // تصفير عداد الوقت لضمان أن المزامنة ستبدأ من نقطة الصفر الحقيقية
    position = (fmt_ctx && fmt_ctx->start_time != AV_NOPTS_VALUE) ? (double)fmt_ctx->start_time / AV_TIME_BASE : position;
    
    UtilityFunctions::print("[AUDIO] Buffers cleared and ready for fresh start.");
}

void FFmpegPlayer::_prefill_buffers() {
    if (!fmt_ctx || !audio_codec_ctx) return;

    UtilityFunctions::print("[PREFILL] Starting buffer warm-up for Realme C33...");

    // 1. ملء طابور الحزم الخام أولاً (قراءة 60 حزمة لضمان وجود مادة للعمل)
    for (int i = 0; i < 60; i++) {
        _read_packets_to_queue();
    }

    // 2. فك تشفير كمية أولية من الصوت لملء بفر جودو قبل انطلاق التوقيت
    int prefill_count = 0;
    while (!audio_packet_queue.empty() && prefill_count < 20) {
        AVPacket *a_pkt = audio_packet_queue.front();
        
        if (avcodec_send_packet(audio_codec_ctx, a_pkt) == 0) {
            AVFrame *audio_frame = av_frame_alloc();
            while (avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
                _push_audio_samples(audio_frame);
            }
            av_frame_free(&audio_frame);
        }
        
        audio_packet_queue.pop_front();
        av_packet_free(&a_pkt);
        prefill_count++;
    }

    UtilityFunctions::print("[PREFILL] Warm-up finished. Audio packets processed: ", prefill_count);
}


// ─── فكّ الترميز: فيديو + صوت ─────────────────────────────────────────

void FFmpegPlayer::_decode_next_frame() {
    _read_packets_to_queue();

    // 1. معالجة الصوت: نتركها تعمل باستمرار لتوفير الساعة المرجعية للمزامنة
    if (audio_player && audio_player->is_playing() && audio_codec_ctx) {
        Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
        if (pb.is_valid()) {
            int space = pb->get_frames_available();
            while (!audio_packet_queue.empty() && space > 2048) {
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

    // 2. إرسال الحزم لرقاقة العتاد (MediaCodec)
    while (!video_packet_queue.empty()) {
        AVPacket *pkt = video_packet_queue.front();
        int send_res = avcodec_send_packet(video_codec_ctx, pkt);
        
        if (send_res == 0) {
            video_packet_queue.pop_front();
            av_packet_free(&pkt);
        } else if (send_res == AVERROR(EAGAIN)) {
            break;
        } else {
            video_packet_queue.pop_front();
            av_packet_free(&pkt);
        }
    }

    // 3. استقبال الإطارات من العتاد مع المزامنة الذكية
    AVFrame *vf = av_frame_alloc();

    while (avcodec_receive_frame(video_codec_ctx, vf) == 0) {
        double frame_pts = vf->pts * av_q2d(fmt_ctx->streams[video_stream_idx]->time_base);

        // --- نظام إسقاط الإطارات المتأخرة (حل الـ Lag) ---
        if (frame_pts < position - 0.04) {
            av_frame_unref(vf);
            continue; 
        }

        // إطار مستقبلي، ننتظر الدورة القادمة
        if (frame_pts > position + 0.04) {
            av_frame_unref(vf);
            break;
        }

        // --- نظام كشف الصيغة العتادية المتغيرة ---
        // إذا تغيرت صيغة بكسلات العتاد (مثلاً من YUV إلى NV12)، نعيد بناء السياق فوراً
        if (!sws_ctx) {
            sws_ctx = sws_getContext(video_width, video_height, (AVPixelFormat)vf->format,
                                     video_width, video_height, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            UtilityFunctions::print("[SWS] Hardware Format Auto-Detected: ", vf->format);
        }

        // تحويل الإطار إلى RGB24
        // نستخدم linesize المناسب للصيغة المكتشفة تلقائياً
        uint8_t *dest[4] = { frame_buffer, nullptr, nullptr, nullptr };
        int dest_linesize[4] = { video_width * 3, 0, 0, 0 };
        
        sws_scale(sws_ctx, vf->data, vf->linesize, 0, video_height, dest, dest_linesize);

        // تحديث التكستشر في جودو
        PackedByteArray pba;
        pba.resize(video_width * video_height * 3);
        memcpy(pba.ptrw(), frame_buffer, pba.size());

        Ref<Image> img = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGB8, pba);
        if (current_texture.is_valid()) {
            current_texture->update(img);
        }
        _emit_frame_updated();
        
        av_frame_unref(vf);
        break; 
    }

    av_frame_free(&vf);
}



// ─── دفع عينات الصوت إلى Godot AudioStreamGeneratorPlayback ─────────────────

void FFmpegPlayer::_push_audio_samples(AVFrame *frame) {
    if (!frame || !audio_player || !swr_ctx) return;

    Ref<AudioStreamGeneratorPlayback> pb = audio_player->get_stream_playback();
    if (pb.is_null()) return;

    // 1. إعادة ضبط إزاحة الساعة (Offset) بعد الـ Seek أو عند أول إطار
    if (!audio_pts_set && frame->pts != AV_NOPTS_VALUE && audio_stream_idx >= 0) {
        double frame_pts = frame->pts * av_q2d(fmt_ctx->streams[audio_stream_idx]->time_base);
        audio_pts_offset = frame_pts;
        audio_pts_set = true;
        // تحديث الموقع الحالي فوراً ليتطابق مع نقطة البداية الجديدة
        position = audio_pts_offset;
        UtilityFunctions::print("[AUDIO_SYNC] Clock Reset. New Offset: ", audio_pts_offset);
    }

    // 2. التأكد من وجود مساحة في بفر جودو
    int frames_available = pb->get_frames_available();
    if (frames_available < frame->nb_samples) return; 

    // 3. حساب عدد العينات المخرجة المتوقع بعد التحويل
    int out_count = (int)av_rescale_rnd(swr_get_delay(swr_ctx, audio_sample_rate) + frame->nb_samples,
        audio_sample_rate, audio_codec_ctx->sample_rate, AV_ROUND_UP);

    uint8_t *out_buf = nullptr;
    int linesize = 0;
    
    // تخصيص ذاكرة مؤقتة للعينات المحولة (بصيغة Float Stereo)
    if (av_samples_alloc(&out_buf, &linesize, 2, out_count, AV_SAMPLE_FMT_FLT, 0) < 0) return;

    // 4. عملية التحويل الفعلية (Resampling)
    int converted = swr_convert(swr_ctx, &out_buf, out_count, (const uint8_t **)frame->data, frame->nb_samples);

    if (converted > 0) {
        const float *samples = reinterpret_cast<const float *>(out_buf);
        PackedVector2Array buffer;
        buffer.resize(converted);
        Vector2 *write_ptr = buffer.ptrw();

        for (int i = 0; i < converted; i++) {
            // تحويل العينات إلى Vector2 (L, R) مع التحكم في مستوى الصوت (Volume)
            write_ptr[i] = Vector2(
                CLAMP(samples[i * 2]     * volume, -1.0f, 1.0f),
                CLAMP(samples[i * 2 + 1] * volume, -1.0f, 1.0f)
            );
        }
        
        // 5. دفع البيانات إلى جودو
        pb->push_buffer(buffer);
    }

    // 6. تنظيف الذاكرة المؤقتة للعينات
    if (out_buf) {
        av_freep(&out_buf);
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
    _clear_queues(); // 👈 تفريغ الحزم العالقة في الذاكرة
    
    if (video_codec_ctx) { avcodec_free_context(&video_codec_ctx); video_codec_ctx = nullptr; }
    if (audio_codec_ctx) { avcodec_free_context(&audio_codec_ctx); audio_codec_ctx = nullptr; }
    if (fmt_ctx) { avformat_close_input(&fmt_ctx); fmt_ctx = nullptr; }
    if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
    if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (frame_buffer) { av_free(frame_buffer); frame_buffer = nullptr; }
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
