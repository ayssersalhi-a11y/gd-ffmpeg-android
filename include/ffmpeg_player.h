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
 *   # اربط إشارة frame_updated بـ TextureRect لعرض الفيديو
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

protected:
    static void _bind_methods();

private:
    // ── FFmpeg: فيديو ────────────────────────────────────────────────────────
    AVFormatContext *fmt_ctx;
    AVCodecContext  *video_codec_ctx;
    SwsContext      *sws_ctx;
    int              video_stream_idx;

    int    video_width;
    int    video_height;
    double fps;

    uint8_t           *frame_buffer;
    Ref<ImageTexture>  current_texture;

    // ── FFmpeg: صوت ──────────────────────────────────────────────────────────
    AVCodecContext *audio_codec_ctx;
    SwrContext     *swr_ctx;
    int             audio_stream_idx;
    int             audio_sample_rate;  // معدل العينات المُخرجة (مطابق للملف)
    int             audio_channels;     // دائماً 2 (ستيريو) للإخراج

    // ── Godot Audio ───────────────────────────────────────────────────────────
    AudioStreamPlayer         *audio_player;
    Ref<AudioStreamGenerator>  audio_generator;

    // ── حالة التشغيل ─────────────────────────────────────────────────────────
    bool   playing;
    bool   looping;
    float  volume;
    double duration;
    double position;

    // ── دوال مساعدة ──────────────────────────────────────────────────────────
    bool _setup_audio(AVStream *astream);
    void _decode_next_frame();
    void _push_audio_samples(AVFrame *frame);
    void _cleanup();
};

} // namespace godot
