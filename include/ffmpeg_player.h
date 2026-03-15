/**
 * ffmpeg_player.h
 * تعريف كلاس FFmpegPlayer - GDExtension لـ Godot 4
 * نسخة معدلة لتوافق التعديلات الأخيرة على الـ CPP
 */

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/string.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace godot {

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

    bool   is_playing() const;
    double get_duration() const;
    double get_position() const;
    int    get_video_width() const;
    int    get_video_height() const;
    double get_fps() const;
    Ref<ImageTexture> get_current_frame_texture() const;

    void set_loop(bool en);
    bool get_loop() const;

    void set_volume(float v);
    float get_volume() const;

    // ── Override Godot ───────────────────────────────────────────────────────
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    // FFmpeg contexts
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *video_codec_ctx;
    AVCodecContext   *audio_codec_ctx;
    SwsContext       *sws_ctx;
    SwrContext       *swr_ctx;

    int video_stream_idx;
    int audio_stream_idx;

    // حالة التشغيل
    bool playing;
    bool looping;
    float volume;
    double duration;
    double position;

    // معلومات الفيديو
    int video_width;
    int video_height;
    double fps;

    // مخزن الإطار الحالي
    uint8_t          *frame_buffer;
    Ref<ImageTexture> current_texture;

    // ── دوال داخلية ─────────────────────────────────────────────────────────
    void _decode_next_frame();
    void _cleanup();
};

} // namespace godot
