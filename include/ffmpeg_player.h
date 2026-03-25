/**
 * ffmpeg_player.h
 * GDExtension - FFmpeg Video + Audio Player for Godot 4 (Android ARM64/ARM32)
 * تم التحديث لدعم فك التشفير العتادي (Hardware Acceleration) وتحسين الأداء.
 */

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/core/class_db.hpp>

// لإدارة مخزن الحزم (Queues)
#include <list>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
// تم إضافة هيدر الميديا كودك لضمان التعرف على الوظائف العتادية
#include <libavcodec/mediacodec.h> 
}

namespace godot {

class FFmpegPlayer : public Node {
    GDCLASS(FFmpegPlayer, Node)

public:
    FFmpegPlayer();
    ~FFmpegPlayer();

    // ── واجهة GDScript (المتصلة بـ Godot) ──────────────────────────────────
    bool load_video(const String &path);
    void play();
    void pause();
    void stop();
    void seek(double seconds);

    bool   is_playing()              const;
    double get_duration()            const;
    double get_position()            const;
    int    get_video_width()         const;
    int    get_video_height()        const;
    double get_fps()                 const;
    Ref<ImageTexture> get_current_frame_texture() const;

    void  set_loop(bool enable);
    bool  get_loop() const;
    void  set_volume(float vol);
    float get_volume() const;

    // ── Override Godot ───────────────────────────────────────────────────────
    void _ready()               override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    // ── FFmpeg: سياق الملف والفيديو ──────────────────────────────────────────
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext  *video_codec_ctx = nullptr;
    SwsContext      *sws_ctx = nullptr;
    int              video_stream_idx = -1;

    int    video_width = 0;
    int    video_height = 0;
    double fps = 0.0;

    uint8_t           *frame_buffer = nullptr;
    Ref<ImageTexture>  current_texture;

    // ── FFmpeg: سياق الصوت ───────────────────────────────────────────────────
    AVCodecContext *audio_codec_ctx = nullptr;
    SwrContext     *swr_ctx = nullptr;
    int             audio_stream_idx = -1;
    int             audio_sample_rate = 0;
    int             audio_channels = 2;

    // ── Godot Audio ───────────────────────────────────────────────────────────
    AudioStreamPlayer         *audio_player = nullptr;
    Ref<AudioStreamGenerator>  audio_generator;

    // ── نظام مخزن الحزم (The Queues) ──────────────────────────────────────────
    std::list<AVPacket*> video_packet_queue;
    std::list<AVPacket*> audio_packet_queue;
    const int MAX_QUEUE_SIZE = 120; 

    // ── حالة التشغيل والمزامنة ───────────────────────────────────────────────
    bool   playing = false;
    bool   looping = false;
    float  volume = 1.0f;
    double duration = 0.0;
    double position = 0.0;

    // إزاحة الساعة المرجعية للمزامنة بين الصوت والصورة
    double audio_pts_offset = 0.0; 
    bool   audio_pts_set    = false;

    // ── دوال مساعدة داخلية ────────────────────────────────────────────────────
    bool _setup_audio(AVStream *astream);
    void _read_packets_to_queue(); 
    void _prefill_buffers();      
    void _decode_next_frame();     
    void _push_audio_samples(AVFrame *frame);
    void _clear_queues();          
    void _cleanup();
    void _allocate_buffers();      
    void _clear_audio_buffers();   

    // ── دوال إرسال الإشارات ───────────────────────────────────────────────────
    void _emit_video_loaded(bool success);
    void _emit_video_finished();
    void _emit_frame_updated();
    void _emit_playback_error(const String &message);
};

} // namespace godot
