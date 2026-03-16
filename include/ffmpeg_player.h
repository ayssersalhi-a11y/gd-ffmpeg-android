/**
 * ffmpeg_player.h
 * تعريف كلاس FFmpegPlayer - GDExtension لـ Godot 4
 */

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace godot {

/**
 * FFmpegPlayer
 * ────────────
 * Node يُضاف لمشهد Godot ويتحكم في تشغيل الفيديو + الصوت عبر FFmpeg.
 * يُصدر كل إطار فيديو كـ ImageTexture، والصوت يُعزف تلقائياً عبر AudioStreamGenerator.
 *
 * الاستخدام في GDScript:
 *   var player = FFmpegPlayer.new()
 *   add_child(player)
 *   player.load_video("res://videos/sample.mp4")
 *   player.play()
 *   player.frame_updated.connect(func(tex): $TextureRect.texture = tex)
 */
class FFmpegPlayer : public Node {
    GDCLASS(FFmpegPlayer, Node)

public:
    FFmpegPlayer();
    ~FFmpegPlayer();

    // ── واجهة GDScript ──────────────────────────────────────────────────────
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

signals:
    // يُطلق عند تحديث إطار الفيديو، لربطه مباشرة مع TextureRect
    void frame_updated(Ref<ImageTexture> texture);

protected:
    static void _bind_methods();

private:
    // ── FFmpeg: فيديو ────────────────────────────────────────────────────────
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext  *video_codec_ctx = nullptr;
    SwsContext      *sws_ctx = nullptr;
    int              video_stream_idx = -1;

    int    video_width = 0;
    int    video_height = 0;
    double fps = 0.0;

    uint8_t           *frame_buffer = nullptr;
    Ref<ImageTexture>  current_texture;

    // ── FFmpeg: صوت ──────────────────────────────────────────────────────────
    AVCodecContext *audio_codec_ctx = nullptr;
    SwrContext     *swr_ctx = nullptr;
    int             audio_stream_idx = -1;
    int             audio_sample_rate = 0;  // معدل العينات
    int             audio_channels = 2;     // ستيريو دائمًا

    // ── Godot Audio ───────────────────────────────────────────────────────────
    AudioStreamPlayer         *audio_player = nullptr;
    Ref<AudioStreamGenerator>  audio_generator;

    // ── حالة التشغيل ─────────────────────────────────────────────────────────
    bool   playing = false;
    bool   looping = false;
    float  volume = 1.0f;
    double duration = 0.0;
    double position = 0.0;

    // ── دوال مساعدة ──────────────────────────────────────────────────────────
    // تجهيز الصوت وربطه بالملف
    bool _setup_audio(AVStream *astream);

    // فك ترميز الإطار التالي للفيديو
    void _decode_next_frame();

    // تحويل وإرسال عينات الصوت إلى AudioStreamGenerator
    void _push_audio_samples(AVFrame *frame);

    // تنظيف جميع الموارد عند التوقف أو تدمير الكلاس
    void _cleanup();

    // ── مستقبلي: دعم الترجمات أو تأثيرات الفيديو ─────────────────────────────
    // std::vector<AVSubtitle> subtitles;
    // SwsFilterContext* filter_ctx;
};

} // namespace godot
